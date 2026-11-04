#include "server/services/warehouse.hpp"

#include <fstream>
#include <algorithm>
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
        if (job["files"].size() == 1)
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
