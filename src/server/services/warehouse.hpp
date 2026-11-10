#pragma once

#include "server/storage/store.hpp"
#include "supervisor.hpp"
#include <condition_variable>
#include <deque>
#include <thread>

namespace nvr::web
{
class Warehouse
{
    Store &store_;
    Supervisor &supervisor_;
    fs::path root_, bin_;
    std::mutex &mutation_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> pending_;
    bool stopping_ = false;
    std::thread validator_;

    void validate_loop();
    void validate(const std::string &id);

public:
    Warehouse(Store &, Supervisor &, fs::path root, fs::path bin, std::mutex &mutation);
    ~Warehouse();

    // Chunked upload lifecycle.
    Json begin(const Json &input);
    Json append(const std::string &id, size_t index, uint64_t offset, std::string_view bytes);
    Json complete(const std::string &id);
    void cancel(const std::string &id);

    // Existing assets.
    Json revalidate_model(const std::string &id);
    void remove(const std::string &id, const std::string &confirmation);
};

Json inspect_video(const fs::path &file);
std::string embedded_rknn_toolkit_version(const fs::path &file);
Json inspect_model(const fs::path &directory, const fs::path &bin);

void extract_model_zip(const fs::path &file, const fs::path &destination);
void bundle_model(const fs::path &directory);
} // namespace nvr::web
