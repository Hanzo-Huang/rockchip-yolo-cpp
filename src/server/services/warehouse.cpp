#include "server/services/warehouse.hpp"

#include <fstream>
#include <algorithm>
#include <regex>
#include <fcntl.h>
#include <unistd.h>

namespace nvr::web
{
namespace
{
constexpr uint64_t MiB = 1024 * 1024;

std::string extension(std::string name)
{
    auto result = fs::path(name).extension().string();
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c)
                   {
                       return char(std::tolower(c));
                   });
    return result;
}

void valid_filename(const std::string &name)
{
    check(!name.empty() && name.size() <= 200 && fs::path(name).filename() == fs::path(name) &&
              name != "." && name != "..",
          "Invalid upload filename");
    for (unsigned char c : name)
    {
        check(c >= 32 && c != 127 && c != '\\', "Invalid upload filename");
    }
}

struct HubFile
{
    std::string repo, revision, path;
    bool archive;
};

HubFile hub_file(const std::string &url)
{
    static const std::regex pattern(
        R"(https://huggingface\.co/([A-Za-z0-9_][A-Za-z0-9_.-]*/[A-Za-z0-9_][A-Za-z0-9_.-]*)/(?:blob|resolve)/([A-Za-z0-9_][A-Za-z0-9_.-]*)/((?:[A-Za-z0-9_][A-Za-z0-9_.-]*/)*[A-Za-z0-9_][A-Za-z0-9_.-]*)(?:\?download=true)?)");
    std::smatch match;
    check(url.size() <= 2048 && std::regex_match(url, match, pattern),
          "Paste a public Hugging Face model file link");
    auto ext = extension(match[3]);
    check(ext == ".rknn" || ext == ".zip", "Choose an RKNN file or a model ZIP");
    return {match[1], match[2], match[3], ext == ".zip"};
}

std::vector<std::string> hub_request(const std::string &url)
{
    // Hub file resolution can redirect large files to the Hub's storage CDN.
    return {"curl", "--disable", "--fail", "--silent", "--globoff", "--location",
            "--max-redirs", "4", "--proto", "=https", "--proto-redir", "=https",
            "--connect-timeout", "10", "--speed-limit", "1024", "--speed-time", "30", "--", url};
}

void download_hub_file(const std::string &url, const fs::path &destination, size_t limit,
                       const std::string &label, int timeout, const std::atomic_bool &stopping)
{
    std::ofstream output(destination, std::ios::binary);
    check(bool(output), "Cannot open model staging file", 507);
    try
    {
        capture(hub_request(url), timeout, limit, &output, &stopping);
    }
    catch (const Error &error)
    {
        if (stopping)
        {
            throw;
        }
        throw Error(error.status, "Could not download " + label +
                                      " from Hugging Face. Check the link, file size and connection.");
    }
    output.close();
    check(bool(output) && fs::file_size(destination) > 0, "Incomplete Hugging Face download", 422);
}

void prepare_hub_model(const HubFile &file, const fs::path &staging, const fs::path &prepared,
                       const std::atomic_bool &stopping)
{
    const std::string hub = "https://huggingface.co";
    std::string revision = file.revision;
    if (!file.archive)
    {
        // Resolve the revision once so the binary and its metadata come from the same commit.
        const auto url = hub + "/api/models/" + file.repo + "/revision/" + revision + "?expand=sha";
        try
        {
            auto response = capture(hub_request(url), 20, MiB, nullptr, &stopping);
            revision = Json::parse(response).at("sha").get<std::string>();
        }
        catch (const std::exception &)
        {
            if (stopping)
            {
                throw;
            }
            throw Error(422,
                        "Could not resolve the Hugging Face revision. Check that the model and revision are public.");
        }
        check(std::regex_match(revision, std::regex("[a-f0-9]{40}")),
              "Invalid Hugging Face model revision", 422);
    }
    const auto base = hub + "/" + file.repo + "/resolve/" + revision + "/";
    const auto available = fs::space(staging).available;
    check(available >= 3 * MiB, "Insufficient storage for model download", 507);
    const auto limit = size_t(std::min<uint64_t>(256 * MiB, (available - MiB) / 2));
    if (file.archive)
    {
        auto archive = staging / "model.zip";
        download_hub_file(base + file.path, archive, limit, "model ZIP", 900, stopping);
        extract_model_zip(archive, prepared);
    }
    else
    {
        const auto parent = fs::path(file.path).parent_path();
        download_hub_file(base + (parent / "metadata.yaml").generic_string(),
                          prepared / "metadata.yaml", 65536, "the matching metadata.yaml", 30,
                          stopping);
        download_hub_file(base + file.path, prepared / "model.rknn", limit, "the RKNN model",
                          900, stopping);
    }
}
} // namespace

Warehouse::Warehouse(Store &store, Supervisor &supervisor, fs::path root, fs::path bin,
                     std::mutex &mutation)
    : store_(store), supervisor_(supervisor), root_(std::move(root)), bin_(std::move(bin)),
      mutation_(mutation)
{
    for (const auto *name : {"staging", "videos", "models", "trash"})
    {
        fs::create_directories(root_ / name);
    }
    validator_ = std::thread(
        [this]
        {
            validate_loop();
        });
}

Warehouse::~Warehouse()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    validator_.join();
}

// Upload lifecycle.
Json Warehouse::begin(const Json &input)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string kind = input.at("kind"), name = display_name(input);
    check(kind == "video" || kind == "model", "Unknown warehouse type");
    auto files = input.at("files");
    check(files.is_array() && !files.empty() && files.size() <= 2,
          "Provide one video, one ZIP, or two model files");
    uint64_t total = 0;
    bool yaml = false, rknn = false, zip = false;
    for (auto &file : files)
    {
        check(file["name"].is_string() && file["size"].is_number_unsigned(),
              "Invalid file declaration");
        std::string filename = file["name"];
        valid_filename(filename);
        uint64_t size = file["size"];
        check(size > 0, "File is empty");
        total += size;
        auto ext = extension(filename);
        yaml = yaml || filename == "metadata.yaml";
        rknn = rknn || ext == ".rknn";
        zip = zip || ext == ".zip";
        file["received"] = 0;
        if (kind == "video")
        {
            check(ext == ".mp4" && files.size() == 1, "Upload one MP4 video");
        }
    }
    if (kind == "model")
    {
        check((files.size() == 1 && zip) || (files.size() == 2 && yaml && rknn),
              "Model upload needs metadata.yaml + one .rknn, or a ZIP containing both");
    }
    check(total < fs::space(root_).available / 2, "Insufficient storage for upload and validation",
          507);
    Json job = {{"id", new_id()}, {"kind", kind},   {"name", name},  {"state", "uploading"},
                {"files", files}, {"total", total}, {"received", 0}, {"created_at", now_iso()}};
    fs::create_directories(root_ / "staging" / job["id"].get<std::string>());
    store_.save_job(job);
    return job;
}

Json Warehouse::append(const std::string &id, size_t index, uint64_t offset, std::string_view bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto job = store_.job(id);
    check(job["state"] == "uploading", "Upload is not accepting data", 409);
    check(index < job["files"].size() && !bytes.empty() && bytes.size() <= 4 * MiB,
          "Invalid upload chunk");
    auto &file = job["files"][index];
    uint64_t received = file["received"], expected = file["size"];
    check(offset == received, "Upload offset mismatch", 409);
    check(bytes.size() <= expected - received, "Upload exceeds declared size");
    check(fs::space(root_).available > bytes.size(), "Insufficient free storage", 507);
    auto path = root_ / "staging" / id / ("part-" + std::to_string(index));
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    check(fd >= 0, "Cannot open upload staging file", 500);
    if (lseek(fd, 0, SEEK_END) != off_t(offset))
    {
        close(fd);
        throw Error(409, "Staging offset mismatch");
    }
    size_t written = 0;
    while (written < bytes.size())
    {
        ssize_t n = write(fd, bytes.data() + written, bytes.size() - written);
        if (n <= 0)
        {
            int rollback = ftruncate(fd, off_t(offset));
            close(fd);
            throw Error(
                507, rollback == 0
                         ? "Upload write failed"
                         : "Upload write failed and rollback failed; cancel and retry the upload");
        }
        written += size_t(n);
    }
    int result = fsync(fd);
    close(fd);
    check(result == 0, "Upload sync failed", 507);
    file["received"] = received + bytes.size();
    job["received"] = job["received"].get<uint64_t>() + bytes.size();
    store_.save_job(job);
    return job;
}

Json Warehouse::complete(const std::string &id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto job = store_.job(id);
    check(job["state"] == "uploading", "Upload was already submitted", 409);
    for (auto &file : job["files"])
    {
        check(file["size"] == file["received"], "Upload is incomplete", 409);
    }
    job["state"] = "validating";
    store_.save_job(job);
    pending_.push_back(id);
    ready_.notify_one();
    return job;
}

Json Warehouse::import_hugging_face(const Json &input)
{
    const std::string url = input.value("url", std::string());
    hub_file(url);
    const auto name = display_name(input);
    std::lock_guard<std::mutex> lock(mutex_);
    check(pending_.size() < 8, "Warehouse is busy. Try again after the current jobs finish.", 429);
    Json job = {{"id", new_id()}, {"kind", "model"}, {"name", name}, {"state", "downloading"},
                {"hub_url", url}, {"total", 0}, {"received", 0}, {"created_at", now_iso()}};
    store_.save_job(job);
    pending_.push_back(job["id"].get<std::string>());
    ready_.notify_one();
    return job;
}

void Warehouse::cancel(const std::string &id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto job = store_.job(id);
    check(job["state"] == "uploading" || job["state"] == "rejected",
          "Cannot cancel this validation", 409);
    auto staging = root_ / "staging" / id;
    if (fs::exists(staging))
    {
        fs::rename(staging, root_ / "trash" / ("upload-" + id));
    }
    job["state"] = "cancelled";
    store_.save_job(job);
}

// Existing model and asset management.
Json Warehouse::revalidate_model(const std::string &id)
{
    std::lock_guard<std::mutex> lock(mutation_);
    auto asset = store_.asset(id);
    check(asset["kind"] == "model", "Not a model", 404);
    check(!supervisor_.uses_asset(id), "Stop channels using this model before revalidation", 409);
    auto info = inspect_model(fs::path(asset["path"].get<std::string>()).parent_path(), bin_);
    store_.update_asset_info(id, info.value("compatible", false) ? "ready" : "incompatible", info);
    return store_.asset(id);
}

void Warehouse::remove(const std::string &id, const std::string &confirmation)
{
    std::lock_guard<std::mutex> lock(mutation_);
    auto asset = store_.asset(id);
    check(confirmation == asset["name"], "Confirmation name does not match", 409);
    check(store_.references(id).empty() && !supervisor_.uses_asset(id),
          "Asset is assigned to a channel or active preview", 409);
    supervisor_.forget("preview-" + id);
    fs::path source = fs::path(asset["path"].get<std::string>()).parent_path();
    auto trash = root_ / "trash" / id;
    check(!fs::exists(trash), "Recovery directory already exists", 409);
    fs::rename(source, trash);
    try
    {
        store_.remove_asset(id);
    }
    catch (...)
    {
        fs::rename(trash, source);
        throw;
    }
    private_write(trash / "recovery.json", asset.dump(2));
}

// Background validation after an upload is complete.
void Warehouse::validate_loop()
{
    while (true)
    {
        std::string id;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock,
                        [&]
                        {
                            return stopping_ || !pending_.empty();
                        });
            if (stopping_)
            {
                break;
            }
            id = pending_.front();
            pending_.pop_front();
        }
        try
        {
            validate(id);
        }
        catch (const std::exception &error)
        {
            auto job = store_.job(id);
            if (job.contains("hub_url") && !fs::exists(root_ / "models" / id))
            {
                std::error_code ignored;
                fs::remove_all(root_ / "staging" / id, ignored);
            }
            job["state"] = "rejected";
            job["error"] = std::string(error.what()).substr(0, 1000);
            store_.save_job(job);
        }
    }
}

void Warehouse::validate(const std::string &id)
{
    auto job = store_.job(id);
    const std::string kind = job["kind"];
    auto staging = root_ / "staging" / id, prepared = staging / "prepared";
    fs::create_directories(prepared);
    Json info;
    if (kind == "video")
    {
        fs::copy_file(staging / "part-0", prepared / "original.mp4");
        info = inspect_video(prepared / "original.mp4");
        if (info.value("compatible", false))
        {
            Json config = {{"id", "thumbnail"},
                           {"run_id", new_id()},
                           {"uri", (prepared / "original.mp4").string()},
                           {"source_type", "file"},
                           {"preview_only", true},
                           {"recording", false},
                           {"output", (staging / "probe").string()},
                           {"thumbnail_file", (prepared / "thumbnail.jpg").string()}};
            private_write(staging / "probe-config.json", config.dump());
            try
            {
                capture(
                    {(bin_ / "rk-nvr-worker").string(), (staging / "probe-config.json").string()},
                    15);
                info["thumbnail"] = fs::exists(prepared / "thumbnail.jpg");
            }
            catch (const std::exception &)
            {
                info["thumbnail"] = false;
                info["preview_warning"] = "Hardware thumbnail could not be generated";
            }
        }
    }
    else
    {
        if (job.contains("hub_url"))
        {
            prepare_hub_model(hub_file(job["hub_url"]), staging, prepared, stopping_);
            job["state"] = "validating";
            store_.save_job(job);
        }
        else if (job["files"].size() == 1)
        {
            extract_model_zip(staging / "part-0", prepared);
        }
        else
        {
            for (size_t i = 0; i < job["files"].size(); ++i)
            {
                std::string name = job["files"][i]["name"];
                fs::copy_file(staging / ("part-" + std::to_string(i)),
                              prepared /
                                  (name == "metadata.yaml" ? "metadata.yaml" : "model.rknn"));
            }
        }
        info = inspect_model(prepared, bin_);
        bundle_model(prepared);
    }
    auto destination = root_ / (kind == "video" ? "videos" : "models") / id;
    fs::rename(prepared, destination);
    Json asset = {
        {"id", id},
        {"kind", kind},
        {"name", job["name"]},
        {"status", info.value("compatible", false) ? "ready" : "incompatible"},
        {"path", (destination / (kind == "video" ? "original.mp4" : "model.rknn")).string()},
        {"info", info},
        {"created_at", job["created_at"]}};
    {
        std::lock_guard<std::mutex> lock(mutation_);
        store_.add_asset(asset);
    }
    job["state"] = "ready";
    job["asset_id"] = id;
    job["asset_status"] = asset["status"];
    // The immutable original is now committed in the asset directory. Only this
    // job's duplicate temporary parts/probe artifacts can be discarded.
    std::error_code cleanup_error;
    fs::remove_all(staging, cleanup_error);
    if (cleanup_error)
    {
        job["cleanup_warning"] = "Temporary upload parts need manual cleanup";
    }
    store_.save_job(job);
}

} // namespace nvr::web
