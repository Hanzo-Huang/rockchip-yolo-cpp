#include "server/services/warehouse.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <memory>

namespace nvr::web
{
void extract_model_zip(const fs::path &file, const fs::path &destination)
{
    auto *raw = archive_read_new();
    std::unique_ptr<archive, decltype(&archive_read_free)> input(raw, archive_read_free);
    archive_read_support_format_zip(raw);
    check(archive_read_open_filename(raw, file.c_str(), 65536) == ARCHIVE_OK, "Cannot open ZIP",
          422);
    archive_entry *entry = nullptr;
    int entries = 0, files = 0;
    bool yaml = false, model = false;
    uint64_t total = 0;
    // Leave room for the downloadable bundle built from the extracted files.
    const auto storage_limit = fs::space(destination).available / 2;
    int status;
    while ((status = archive_read_next_header(raw, &entry)) == ARCHIVE_OK)
    {
        check(++entries <= 8, "ZIP contains too many entries", 422);
        const char *filename = archive_entry_pathname(entry);
        check(filename, "ZIP entry has no name", 422);
        fs::path name(filename);
        check(!name.is_absolute() && std::string(filename).find('\\') == std::string::npos,
              "Unsafe ZIP path", 422);
        size_t depth = 0;
        for (auto &component : name)
        {
            check(component != ".." && component != ".", "Unsafe ZIP path", 422);
            if (!component.empty())
            {
                ++depth;
            }
        }
        check(depth <= 2, "Put the two model files at ZIP root or in one folder", 422);
        check(!archive_entry_symlink(entry) && !archive_entry_hardlink(entry) &&
                  archive_entry_is_encrypted(entry) != 1,
              "ZIP links/encrypted entries are not allowed", 422);
        if (archive_entry_filetype(entry) == AE_IFDIR)
        {
            continue;
        }
        check(archive_entry_filetype(entry) == AE_IFREG, "ZIP must contain regular files", 422);
        check(++files <= 2, "ZIP must contain exactly two model files", 422);
        std::string base = name.filename().string();
        bool is_yaml = base == "metadata.yaml", is_model = name.extension() == ".rknn";
        check(is_yaml || is_model, "ZIP may contain only metadata.yaml and one .rknn file", 422);
        check(!(is_yaml ? yaml : model), "Duplicate model ZIP entry", 422);
        if (is_yaml)
        {
            yaml = true;
        }
        else
        {
            model = true;
        }
        auto target = destination / (is_yaml ? "metadata.yaml" : "model.rknn");
        std::ofstream output(target, std::ios::binary);
        check(bool(output), "Create extracted file", 500);
        char buffer[65536];
        la_ssize_t count;
        uint64_t size = 0;
        while ((count = archive_read_data(raw, buffer, sizeof(buffer))) > 0)
        {
            size += uint64_t(count);
            total += uint64_t(count);
            check(total <= 256ULL * 1024 * 1024 && (!is_yaml || size <= 65536),
                  "ZIP exceeds extraction limit", 422);
            check(total <= storage_limit, "Insufficient storage to extract the model ZIP", 507);
            output.write(buffer, count);
            check(bool(output), "Write extracted file", 507);
        }
        check(count == 0, "ZIP data is corrupt", 422);
        check(size > 0, "ZIP contains an empty file", 422);
    }
    check(status == ARCHIVE_EOF && files == 2 && yaml && model,
          "ZIP needs metadata.yaml and one .rknn file", 422);
}

void bundle_model(const fs::path &directory)
{
    auto *raw = archive_write_new();
    std::unique_ptr<archive, decltype(&archive_write_free)> output(raw, archive_write_free);
    archive_write_set_format_zip(raw);
    archive_write_set_options(raw, "zip:compression=store");
    check(archive_write_open_filename(raw, (directory / "bundle.zip").c_str()) == ARCHIVE_OK,
          "Create model download bundle", 500);
    for (const char *filename : {"metadata.yaml", "model.rknn"})
    {
        auto path = directory / filename;
        auto *entry = archive_entry_new();
        archive_entry_set_pathname(entry, filename);
        archive_entry_set_size(entry, la_int64_t(fs::file_size(path)));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        int status = archive_write_header(raw, entry);
        archive_entry_free(entry);
        check(status == ARCHIVE_OK, "Create ZIP header", 500);
        std::ifstream input(path, std::ios::binary);
        char buffer[65536];
        while (input)
        {
            input.read(buffer, sizeof(buffer));
            auto size = input.gcount();
            if (size > 0)
            {
                check(archive_write_data(raw, buffer, size) == size, "Write ZIP data", 507);
            }
        }
    }
    check(archive_write_close(raw) == ARCHIVE_OK, "Finalize model bundle", 507);
}
} // namespace nvr::web
