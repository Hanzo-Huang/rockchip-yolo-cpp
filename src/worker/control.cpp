#include "worker/control.hpp"

#include <iostream>
#include <cmath>
#include <poll.h>
#include <unistd.h>

namespace nvr
{
void WorkerControl::update(const Json &value)
{
    std::lock_guard<std::mutex> lock(mutex);
    state.update(value);
}

void WorkerControl::publish()
{
    Json snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = state;
        const auto current_epoch = epoch.load(), decoded = state.value("decoded", uint64_t(0)),
                   inferred = state.value("inferred", uint64_t(0));
        const double seconds = milliseconds(rate_started) / 1000;
        if (rate_epoch != current_epoch || decoded < rate_decoded || inferred < rate_inferred)
        {
            rate_epoch = current_epoch;
            rate_started = Clock::now();
            rate_decoded = decoded;
            rate_inferred = inferred;
            recent_decode = recent_inference = 0;
        }
        else if (seconds >= 1)
        {
            recent_decode = double(decoded - rate_decoded) / seconds;
            recent_inference = double(inferred - rate_inferred) / seconds;
            rate_started = Clock::now();
            rate_decoded = decoded;
            rate_inferred = inferred;
        }
        snapshot["decode_fps"] = recent_decode;
        snapshot["inference_fps"] = recent_inference;
    }
    snapshot["type"] = "state";
    snapshot["epoch"] = epoch.load();
    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << snapshot.dump() << std::endl;
}

void WorkerControl::read_commands()
{
    std::string pending;
    auto heartbeat = Clock::now();
    while (!done)
    {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        int ready = poll(&input, 1, 100);
        if (ready > 0)
        {
            char buffer[4096];
            ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count <= 0)
            {
                cancel = true;
                stop = true;
                epoch_stop = true;
                break;
            }
            pending.append(buffer, size_t(count));
            if (pending.size() > 16384)
            {
                stop = true;
                epoch_stop = true;
                break;
            }
            for (size_t end; (end = pending.find('\n')) != std::string::npos;)
            {
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                try
                {
                    Json message = Json::parse(line);
                    std::string command = message.at("command");
                    if (command == "stop")
                    {
                        cancel = true;
                        stop = true;
                        epoch_stop = true;
                        update({{"state", "stopping"}});
                    }
                    else if (command == "pause")
                    {
                        paused = true;
                        ++clock_revision;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (queue)
                            {
                                queue->clear();
                            }
                            if (file && preview_queue)
                            {
                                preview_queue->clear();
                            }
                        }
                        update({{"state", file ? "paused" : "analytics_paused"}});
                    }
                    else if (command == "resume")
                    {
                        paused = false;
                        ++clock_revision;
                        update({{"state", "running"}});
                    }
                    else if (command == "seek")
                    {
                        require(file, "only files are seekable");
                        double target = message.at("seconds");
                        require(std::isfinite(target) && target >= 0, "invalid seek position");
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            seek_seconds = target;
                            seek_pending = true;
                        }
                        epoch_stop = true;
                        update({{"state", "seeking"}});
                    }
                    else if (command == "preview")
                    {
                        preview = message.value("enabled", false);
                        preview_fps = std::clamp(message.value("fps", 0), 0, 120);
                    }
                    else if (command == "stream")
                    {
                        streaming = message.value("enabled", false);
                        update({{"stream_error", nullptr}});
                    }
                    else
                    {
                        throw std::runtime_error("unknown worker command");
                    }
                    update({{"ack", message.value("request_id", std::string())}});
                    publish();
                }
                catch (const std::exception &)
                {
                    update({{"command_error", "invalid worker command"}});
                    publish();
                }
            }
        }
        if (milliseconds(heartbeat) > 500)
        {
            publish();
            heartbeat = Clock::now();
        }
    }
}
} // namespace nvr
