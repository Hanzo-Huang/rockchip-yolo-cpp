#pragma once

#include "server/services/common.hpp"
#include <atomic>
#include <memory>
#include <thread>
#include <map>
#include <deque>

namespace nvr::web
{
struct Runner
{
    mutable std::mutex mutex;
    Json config, state;
    int command_fd = -1, output_fd = -1, preview_fd = -1;
    int pid = 0;
    std::atomic<bool> done{false};
    std::thread monitor;
    std::string preview_packet;
    uint64_t preview_serial = 0;
    std::string video_init;
    std::deque<std::pair<uint64_t, std::string>> video_gop;
    std::deque<std::pair<uint64_t, Json>> detections;
    uint64_t video_serial = 0, video_init_serial = 0, video_key_serial = 0, detection_serial = 0;
    size_t video_bytes = 0;
    std::chrono::steady_clock::time_point stop_deadline{}, heartbeat{};

    ~Runner();
    void launch(const fs::path &executable, const fs::path &directory);
    void observe();
    void command(const Json &command);
    Json snapshot() const;
};

class Supervisor
{
    fs::path executable_, root_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Runner>> runners_;

public:
    Supervisor(fs::path executable, fs::path root)
        : executable_(std::move(executable)), root_(std::move(root))
    {
    }

    ~Supervisor();

    // Worker lifecycle and channel state.
    Json start(const std::string &id, Json config);
    Json command(const std::string &id, const Json &message);
    Json state(const std::string &id) const;
    bool active(const std::string &id) const;
    void forget(const std::string &id);
    bool uses_asset(const std::string &id) const;

    // Diagnostic JPEG preview.
    void preview(const std::string &id, bool enabled, int fps);
    std::pair<uint64_t, std::string> packet(const std::string &id) const;

    // Original video and timestamped detections.
    void streaming(const std::string &id, bool enabled);
    std::vector<std::pair<uint64_t, std::string>> video(const std::string &id,
                                                        uint64_t after) const;
    std::vector<std::pair<uint64_t, Json>> detection(const std::string &id, uint64_t after) const;
};
} // namespace nvr::web
