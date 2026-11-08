#include "server/services/supervisor.hpp"

#include <arpa/inet.h>
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <cstring>
extern char **environ;

namespace nvr::web
{
using Clock = std::chrono::steady_clock;

// One worker process and its IPC channels.
Runner::~Runner()
{
    if (!done && pid > 0)
    {
        kill(pid, SIGTERM);
    }
    if (monitor.joinable())
    {
        monitor.join();
    }
    for (int fd : {command_fd, output_fd, preview_fd})
    {
        if (fd >= 0)
        {
            close(fd);
        }
    }
}

void Runner::launch(const fs::path &executable, const fs::path &directory)
{
    fs::create_directories(directory);
    private_write(directory / "config.json", config.dump());
    int commands[2], output[2], preview[2];
    check(pipe2(commands, O_CLOEXEC) == 0, "Create worker command pipe", 500);
    check(pipe2(output, O_CLOEXEC) == 0, "Create worker output pipe", 500);
    check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, preview) == 0,
          "Create preview connection", 500);
    int size = 1024 * 1024;
    setsockopt(preview[0], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    setsockopt(preview[1], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, commands[0], 0);
    posix_spawn_file_actions_adddup2(&actions, output[1], 1);
    posix_spawn_file_actions_adddup2(&actions, preview[1], 3);
    posix_spawn_file_actions_addopen(&actions, 2, (directory / "worker.log").c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0600);
    for (int fd : {commands[0], commands[1], output[0], output[1], preview[0], preview[1]})
    {
        if (fd > 3)
        {
            posix_spawn_file_actions_addclose(&actions, fd);
        }
    }
    std::string binary = executable.string(), path = (directory / "config.json").string();
    char *argv[] = {binary.data(), path.data(), nullptr};
    pid_t child = 0;
    int result = posix_spawn(&child, binary.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(commands[0]);
    close(output[1]);
    close(preview[1]);
    if (result != 0)
    {
        close(commands[1]);
        close(output[0]);
        close(preview[0]);
        throw Error(500, "Cannot launch channel worker");
    }
    pid = int(child);
    command_fd = commands[1];
    output_fd = output[0];
    preview_fd = preview[0];
    for (int fd : {command_fd, output_fd, preview_fd})
    {
        fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    state = {
        {"state", "starting"}, {"run_id", config.at("run_id")}, {"position", 0.0}, {"pid", pid}};
    heartbeat = Clock::now();
    monitor = std::thread(
        [this]
        {
            observe();
        });
}

void Runner::command(const Json &message)
{
    std::lock_guard<std::mutex> lock(mutex);
    check(!done, "Channel is not active", 409);
    std::string line = message.dump() + "\n";
    check(write(command_fd, line.data(), line.size()) == ssize_t(line.size()),
          "Worker command could not be delivered", 503);
    const std::string action = message.at("command");
    if (action == "stop")
    {
        state["state"] = "stopping";
        stop_deadline = Clock::now() + std::chrono::seconds(10);
    }
    if (action == "seek")
    {
        state["state"] = "seeking";
        preview_packet.clear();
        ++preview_serial;
    }
}

Json Runner::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex);
    auto result = state;
    // A worker publishes its result before destroying MPP/RKNN. Do not advertise
    // it as stopped until waitpid confirms all hardware resources are released.
    if (!done && (result.value("state", std::string()) == "stopped" ||
                  result.value("state", std::string()) == "ended"))
    {
        result["state"] = "stopping";
    }
    return result;
}

void Runner::observe()
{
    std::string pending;
    std::vector<char> packet(1048577);
    int status = 0;
    while (!done)
    {
        pollfd fds[] = {{output_fd, POLLIN, 0}, {preview_fd, POLLIN, 0}};
        poll(fds, 2, 50);
        char bytes[8192];
        ssize_t count;
        while ((count = read(output_fd, bytes, sizeof(bytes))) > 0)
        {
            pending.append(bytes, size_t(count));
            if (pending.size() > 16777216)
            {
                pending.clear();
                break;
            }
            for (size_t end; (end = pending.find('\n')) != std::string::npos;)
            {
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                try
                {
                    auto event = Json::parse(line);
                    if (event.value("type", std::string()) == "state")
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.update(event);
                        heartbeat = Clock::now();
                    }
                    else if (event.value("type", std::string()) == "detections")
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        detections.emplace_back(++detection_serial, std::move(event));
                        if (detections.size() > 120)
                        {
                            detections.pop_front();
                        }
                    }
                }
                catch (const std::exception &)
                {
                } // Vendor banner lines are not IPC events.
            }
        }
        while ((count = recv(preview_fd, packet.data(), packet.size(), MSG_DONTWAIT)) > 0)
        {
            if (count < 6 || count > 1048576)
            {
                continue;
            }
            uint32_t length;
            std::memcpy(&length, packet.data(), 4);
            length = ntohl(length);
            if (length > uint32_t(count - 4))
            {
                continue;
            }
            try
            {
                auto meta = Json::parse(packet.data() + 4, packet.data() + 4 + length);
                std::lock_guard<std::mutex> lock(mutex);
                if (meta["run_id"] != config["run_id"] ||
                    state.value("state", std::string()) == "seeking" ||
                    meta.value("epoch", 0) != state.value("epoch", 0))
                {
                    continue;
                }
                if (meta.value("kind", std::string()) == "video")
                {
                    if (meta.value("init", false))
                    {
                        video_init.assign(packet.data(), size_t(count));
                        video_init_serial = ++video_serial;
                        video_gop.clear();
                        video_bytes = 0;
                        video_key_serial = 0;
                    }
                    else
                    {
                        if (meta.value("key", false) && meta.value("first", false))
                        {
                            video_key_serial = video_serial + 1;
                        }
                        video_gop.emplace_back(++video_serial,
                                               std::string(packet.data(), size_t(count)));
                        video_bytes += size_t(count);
                        // Keep recent outgoing chunks across keyframes: clearing at each
                        // keyframe can erase unsent tail fragments for existing viewers.
                        while (video_bytes > 16 * 1024 * 1024 && !video_gop.empty())
                        {
                            video_bytes -= video_gop.front().second.size();
                            video_gop.pop_front();
                        }
                    }
                    continue;
                }
                preview_packet.assign(packet.data(), size_t(count));
                ++preview_serial;
            }
            catch (const std::exception &)
            {
            }
        }
        if (waitpid(pid, &status, WNOHANG) == pid)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (state.value("state", std::string()) != "error")
            {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                {
                    if (state["state"] != "ended")
                    {
                        state["state"] = "stopped";
                    }
                }
                else
                {
                    state["state"] = "error";
                    state["error"] = "Channel worker exited unexpectedly";
                }
            }
            done = true;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            bool expired = stop_deadline != Clock::time_point{} && Clock::now() > stop_deadline;
            if (expired || Clock::now() - heartbeat > std::chrono::seconds(25))
            {
                kill(pid, SIGKILL);
                state["state"] = "error";
                state["error"] =
                    expired ? "Graceful-stop deadline exceeded" : "Worker heartbeat timeout";
            }
        }
    }
}

// Worker lifecycle and channel lookup.
Supervisor::~Supervisor()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &item : runners_)
    {
        if (!item.second->done)
        {
            try
            {
                item.second->command({{"command", "stop"}});
            }
            catch (...)
            {
            }
        }
    }
    runners_.clear();
}

Json Supervisor::start(const std::string &id, Json config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = runners_.find(id);
    check(found == runners_.end() || found->second->done, "Channel is already active", 409);
    size_t active_count = 0;
    for (auto &item : runners_)
    {
        if (!item.second->done)
        {
            ++active_count;
            check(config.value("source_type", std::string()) != "camera" ||
                      item.second->config.value("uri", std::string()) !=
                          config.value("uri", std::string()),
                  "Camera is already in use", 409);
        }
    }
    check(active_count < 8, "Maximum of eight active workers reached", 409);
    config["id"] = id;
    config["run_id"] = new_id();
    config["output"] = (root_ / "recordings" / id / config["run_id"].get<std::string>()).string();
    auto runner = std::make_shared<Runner>();
    runner->config = std::move(config);
    runner->launch(executable_, root_ / "runs" / id / runner->config["run_id"].get<std::string>());
    runners_[id] = runner;
    return runner->snapshot();
}

Json Supervisor::command(const std::string &id, const Json &message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = runners_.find(id);
    check(found != runners_.end(), "Channel is stopped", 409);
    found->second->command(message);
    return found->second->snapshot();
}

Json Supervisor::state(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = runners_.find(id);
    return found == runners_.end() ? Json{{"state", "stopped"}, {"position", 0.0}}
                                   : found->second->snapshot();
}

bool Supervisor::active(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = runners_.find(id);
    return found != runners_.end() && !found->second->done;
}

void Supervisor::forget(const std::string &id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = runners_.find(id);
    if (found != runners_.end())
    {
        check(found->second->done, "Worker is still active", 409);
        runners_.erase(found);
    }
}

bool Supervisor::uses_asset(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &item : runners_)
    {
        if (!item.second->done && (item.second->config.value("video_id", std::string()) == id ||
                                   item.second->config.value("model_id", std::string()) == id))
        {
            return true;
        }
    }
    return false;
}

// Browser preview, video and detection subscriptions.
void Supervisor::preview(const std::string &id, bool enabled, int fps)
{
    try
    {
        command(id, {{"command", "preview"}, {"enabled", enabled}, {"fps", fps}});
    }
    catch (const Error &)
    {
    }
}

std::pair<uint64_t, std::string> Supervisor::packet(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = runners_.find(id);
    if (found == runners_.end())
    {
        return {};
    }
    std::lock_guard<std::mutex> inner(found->second->mutex);
    return {found->second->preview_serial, found->second->preview_packet};
}

void Supervisor::streaming(const std::string &id, bool enabled)
{
    try
    {
        command(id, {{"command", "stream"}, {"enabled", enabled}});
    }
    catch (const Error &)
    {
    }
    if (enabled)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = runners_.find(id);
    if (found == runners_.end())
    {
        return;
    }
    std::lock_guard<std::mutex> inner(found->second->mutex);
    auto &runner = *found->second;
    // A reconnect must wait for fresh muxer initialization, not briefly replay
    // the last viewer's cached GOP before the timestamp origin changes.
    runner.video_init.clear();
    runner.video_gop.clear();
    runner.video_bytes = 0;
    runner.video_key_serial = 0;
}

std::vector<std::pair<uint64_t, std::string>> Supervisor::video(const std::string &id,
                                                                uint64_t after) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = runners_.find(id);
    if (found == runners_.end())
    {
        return {};
    }
    std::lock_guard<std::mutex> inner(found->second->mutex);
    auto &runner = *found->second;
    std::vector<std::pair<uint64_t, std::string>> result;
    if (runner.video_gop.empty())
    {
        return result;
    }
    bool joining = after < runner.video_init_serial || after < runner.video_gop.front().first - 1;
    if (joining)
    {
        if (runner.video_init.empty() || runner.video_key_serial < runner.video_gop.front().first)
        {
            return result;
        }
        result.emplace_back(runner.video_init_serial, runner.video_init);
    }
    for (auto &item : runner.video_gop)
    {
        if (item.first > after && (!joining || item.first >= runner.video_key_serial))
        {
            result.push_back(item);
        }
    }
    return result;
}

std::vector<std::pair<uint64_t, Json>> Supervisor::detection(const std::string &id,
                                                             uint64_t after) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = runners_.find(id);
    if (found == runners_.end())
    {
        return {};
    }
    std::lock_guard<std::mutex> inner(found->second->mutex);
    std::vector<std::pair<uint64_t, Json>> result;
    for (auto &item : found->second->detections)
    {
        if (item.first > after)
        {
            result.push_back(item);
        }
    }
    return result;
}
} // namespace nvr::web
