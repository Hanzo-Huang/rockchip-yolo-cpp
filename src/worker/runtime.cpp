#include "worker/control.hpp"
#include "model/metadata.hpp"
#include "engine/decoder.hpp"
#include "engine/detector_pool.hpp"
#include "engine/recorder.hpp"

#include <algorithm>
#include <fstream>
#include <thread>
#include "engine/jpeg_preview.hpp"
#include "engine/mp4_stream.hpp"

#include <poll.h>
#include <iostream>
#include <arpa/inet.h>
#include <csignal>
#include <cmath>
#include <cstring>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
volatile std::sig_atomic_t signalled = 0;

void signal_stop(int)
{
    signalled = 1;
}

void send_preview(int fd, const nvr::Json &metadata, const std::vector<uint8_t> &jpeg)
{
    std::string header = metadata.dump();
    if (header.size() + jpeg.size() + 4 > 1048576)
    {
        return;
    }
    uint32_t length = htonl(uint32_t(header.size()));
    std::vector<uint8_t> payload(4 + header.size() + jpeg.size());
    std::memcpy(payload.data(), &length, 4);
    std::memcpy(payload.data() + 4, header.data(), header.size());
    std::memcpy(payload.data() + 4 + header.size(), jpeg.data(), jpeg.size());
    // SOCK_SEQPACKET preserves the frame boundary; back-pressure drops previews.
    send(fd, payload.data(), payload.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
}
} // namespace

namespace nvr
{
int run_worker(const Json &config)
{
    WorkerControl control;
    control.file = config.value("source_type", std::string("file")) == "file";
    control.preview = config.contains("thumbnail_file");
    control.update({{"run_id", config.at("run_id")}, {"channel", config.at("id")}});
    std::signal(SIGTERM, signal_stop);
    std::signal(SIGINT, signal_stop);
    std::signal(SIGPIPE, SIG_IGN);
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    // Validation thumbnails are one-shot jobs without an interactive stdin.
    std::thread commands(
        [&]
        {
            if (!config.contains("thumbnail_file"))
            {
                control.read_commands();
            }
        });
    std::thread signals(
        [&]
        {
            while (!control.done)
            {
                if (signalled)
                {
                    control.cancel = true;
                    control.stop = true;
                }
                if (control.stop)
                {
                    control.epoch_stop = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });

    int exit_code = 0;
    try
    {
        double target = 0;
        while (!control.stop)
        {
            control.epoch_stop = false;
            if (control.stop)
            {
                control.epoch_stop = true;
                break;
            }
            const uint64_t epoch = control.epoch.load();

            // Each seek starts a fresh decoder epoch and discards old frames.
            Decoder decoder(config.at("uri"), control.epoch_stop,
                            config.value("source_type", std::string("file")) == "camera");
            if (target > 0)
            {
                decoder.seek(target);
            }
            double duration = decoder.duration_seconds();
            require(!control.file || target <= duration || duration == 0, "seek exceeds duration");
            auto directory = std::filesystem::path(config.at("output").get<std::string>()) /
                             ("epoch-" + std::to_string(epoch));
            std::filesystem::create_directories(directory);

            std::unique_ptr<DetectorPool> detector;
            if (!config.value("preview_only", false))
            {
                detector = std::make_unique<DetectorPool>(
                    config.at("model"), config.value("preprocess", std::string("rga")), false,
                    config.value("confidence", 0.35f), config.value("nms_iou", 0.45f),
                    config.value("inference_workers", 0), configured_model_options(config));
            }
            // Declare after decoder: release imported handles and frames before MPP.
            std::unique_ptr<JpegPreview> preview;
            // Reordered live video releases several frames together. A one-frame
            // queue throws away work even when the NPU can finish before the next
            // burst. Keep only a small burst; full queues still drop the oldest frame.
            auto queue = std::make_shared<FrameQueue>(control.file ? 1 : 8);
            // Live decoders can release several reordered pictures in one burst.
            // Retain a small bounded jitter buffer, not an ever-growing backlog.
            auto preview_queue = std::make_shared<FrameQueue>(control.file ? 1 : 8);
            {
                std::lock_guard<std::mutex> lock(control.mutex);
                control.queue = queue;
                control.preview_queue = preview_queue;
            }

            // Original compressed video feeds recording and browser playback.
            Recorder recorder(directory, decoder.parameters(), decoder.time_base(),
                              config.value("segment_seconds", 60.0),
                              config.value("recording", !control.file));
            std::unique_ptr<Mp4Stream> stream;
            recorder.observe_packet = [&](const AVPacket *packet)
            {
                if (!control.streaming)
                {
                    stream.reset();
                    return;
                }
                try
                {
                    if (!stream)
                    {
                        stream = std::make_unique<Mp4Stream>(
                            decoder.parameters(), decoder.time_base(),
                            [&](const Json &info, const std::vector<uint8_t> &data)
                            {
                                for (size_t offset = 0; offset < data.size(); offset += 32768)
                                {
                                    Json meta = info;
                                    meta["run_id"] = config.at("run_id");
                                    meta["epoch"] = epoch;
                                    meta["first"] = offset == 0;
                                    const auto header = meta.dump();
                                    const auto size = std::min(size_t(32768), data.size() - offset);
                                    std::vector<uint8_t> message(4 + header.size() + size);
                                    uint32_t length = htonl(uint32_t(header.size()));
                                    std::memcpy(message.data(), &length, 4);
                                    std::memcpy(message.data() + 4, header.data(), header.size());
                                    std::memcpy(message.data() + 4 + header.size(),
                                                data.data() + offset, size);
                                    auto deadline = Clock::now() + std::chrono::milliseconds(500);
                                    while (send(config.value("preview_fd", 3), message.data(),
                                                message.size(), MSG_DONTWAIT | MSG_NOSIGNAL) !=
                                           ssize_t(message.size()))
                                    {
                                        require(!control.stop && Clock::now() < deadline,
                                                "Compressed video transport stalled");
                                        pollfd fd{config.value("preview_fd", 3), POLLOUT, 0};
                                        poll(&fd, 1, 5);
                                    }
                                }
                            });
                    }
                    stream->packet(packet);
                }
                catch (const std::exception &error)
                {
                    control.streaming = false;
                    stream.reset();
                    control.update({{"stream_error", error.what()}});
                    control.publish();
                }
            };
            control.update(
                {{"state",
                  control.paused ? (control.file ? "paused" : "analytics_paused") : "running"},
                 {"duration", duration},
                 {"position", target},
                 {"source_start_us", int64_t(decoder.start_seconds() * 1e6)},
                 {"preview_error", nullptr}});
            if (detector)
            {
                control.update({{"detector", detector->description}});
            }
            control.publish();
            std::atomic<uint64_t> decoded{0}, inferred{0};
            std::exception_ptr inference_error;
            std::mutex detection_mutex;
            std::condition_variable detection_ready;
            Json latest_detection = Json::object();
            std::deque<Json> detection_history;
            const double source_start = control.file ? decoder.start_seconds() : 0;
            const double source_fps = decoder.frame_rate();
            const auto position_of = [&](const Frame &frame)
            {
                return std::max(0.0, frame.pts_us == AV_NOPTS_VALUE
                                         ? double(frame.sequence - 1) / source_fps
                                         : double(frame.pts_us) / 1e6 - source_start);
            };
            auto started = Clock::now();

            // Inference consumes its own bounded queue independently of playback.
            std::thread inference(
                [&]
                {
                    try
                    {
                        std::ofstream events(directory / "detections.jsonl");
                        require(bool(events), "open detection log");
                        if (detector)
                        {
                            detector->run(
                                *queue,
                                [&](const Frame &frame)
                                {
                                    return !control.epoch_stop && !control.stop &&
                                           (!control.paused ||
                                            (control.file && frame.preview_step));
                                },
                                [&](const Frame &frame, Json result)
                                {
                                    ++inferred;
                                    result["run_id"] = config.at("run_id");
                                    result["epoch"] = epoch;
                                    {
                                        std::lock_guard<std::mutex> lock(control.output_mutex);
                                        auto event = result;
                                        event["type"] = "detections";
                                        std::cout << event.dump() << std::endl;
                                    }
                                    events << result.dump() << '\n';
                                    require(bool(events), "write detection log");
                                    {
                                        std::lock_guard<std::mutex> lock(detection_mutex);
                                        latest_detection = result;
                                        detection_history.push_back(result);
                                        if (detection_history.size() > 32)
                                        {
                                            detection_history.pop_front();
                                        }
                                    }
                                    detection_ready.notify_all();
                                    if (control.epoch_stop ||
                                        (control.file && control.paused && !frame.preview_step))
                                    {
                                        return;
                                    }
                                    double elapsed = std::max(0.001, milliseconds(started) / 1000);
                                    control.update(
                                        {{"decoded", decoded.load()},
                                         {"inferred", inferred.load()},
                                         {"queue_drops", queue->dropped.load()},
                                         {"decode_fps", double(decoded) / elapsed},
                                         {"inference_fps", double(inferred) / elapsed},
                                         {"preprocess_ms", result.value("preprocess_ms", 0.0)},
                                         {"inference_ms", result.value("inference_ms", 0.0)},
                                         {"rknn_run_ms", result.value("rknn_run_ms", 0.0)},
                                         {"output_get_ms", result.value("output_get_ms", 0.0)},
                                         {"postprocess_ms", result.value("postprocess_ms", 0.0)},
                                         {"age_ms", result.value("age_ms", 0.0)},
                                         {"objects", result["detections"].size()},
                                         {"task", result.value("task", std::string("detect"))},
                                         {"top_class", result.contains("classification") &&
                                                               !result["classification"].empty()
                                                           ? result["classification"][0]["class_id"]
                                                           : Json(nullptr)},
                                         {"top_confidence",
                                          result.contains("classification") &&
                                                  !result["classification"].empty()
                                              ? result["classification"][0]["confidence"]
                                              : Json(0)}});
                                });
                        }
                        events.flush();
                    }
                    catch (...)
                    {
                        inference_error = std::current_exception();
                        control.epoch_stop = true;
                        control.stop = true;
                    }
                });

            // Preview never waits for inference during playback. Analytics keeps only
            // the latest frame; live preview has a small bounded timestamp jitter buffer.
            std::thread rendering(
                [&]
                {
                    auto next_preview = Clock::now();
                    int64_t live_base_pts = AV_NOPTS_VALUE;
                    auto live_base_clock = Clock::now();
                    while (auto frame = preview_queue->pop())
                    {
                        if (control.epoch_stop || control.stop)
                        {
                            break;
                        }
                        if (!control.preview ||
                            (control.file && control.paused && !frame->preview_step))
                        {
                            continue;
                        }
                        const int preview_fps = control.preview_fps.load();
                        if (preview_fps > 0 && !frame->preview_step && Clock::now() < next_preview)
                        {
                            continue;
                        }
                        if (!control.file && preview_fps == 0 && frame->pts_us != AV_NOPTS_VALUE)
                        {
                            auto now = Clock::now();
                            if (live_base_pts == AV_NOPTS_VALUE || frame->pts_us < live_base_pts)
                            {
                                live_base_pts = frame->pts_us;
                                live_base_clock = now + std::chrono::milliseconds(200);
                            }
                            auto due = live_base_clock +
                                       std::chrono::microseconds(frame->pts_us - live_base_pts);
                            // Timestamp jumps/stalls rebase the short buffer. Never catch up by
                            // displaying a large burst, and never retain unbounded live latency.
                            if (due > now + std::chrono::milliseconds(500))
                            {
                                live_base_pts = frame->pts_us;
                                live_base_clock = now + std::chrono::milliseconds(200);
                                due = live_base_clock;
                            }
                            if (due < now - std::chrono::milliseconds(100))
                            {
                                if (milliseconds(frame->decoded_at) > 400)
                                {
                                    ++preview_queue->dropped;
                                    continue;
                                }
                                live_base_pts = frame->pts_us;
                                live_base_clock = now + std::chrono::milliseconds(200);
                                due = live_base_clock;
                            }
                            while (Clock::now() < due && !control.stop && !control.epoch_stop &&
                                   control.preview)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            }
                            if (control.stop || control.epoch_stop || !control.preview)
                            {
                                continue;
                            }
                        }
                        Json result;
                        {
                            std::unique_lock<std::mutex> lock(detection_mutex);
                            // Only a paused seek's one-shot frame waits for its own detections.
                            if (frame->preview_step && detector)
                            {
                                detection_ready.wait_for(
                                    lock, std::chrono::seconds(2),
                                    [&]
                                    {
                                        return control.epoch_stop || control.stop ||
                                               latest_detection.value("frame", uint64_t(0)) ==
                                                   frame->sequence;
                                    });
                            }
                            result = latest_detection;
                            if (!control.file && frame->pts_us != AV_NOPTS_VALUE)
                            {
                                result = Json::object();
                                for (auto it = detection_history.rbegin();
                                     it != detection_history.rend(); ++it)
                                {
                                    const auto pts = it->value("pts_us", int64_t(AV_NOPTS_VALUE));
                                    if (pts != AV_NOPTS_VALUE && pts <= frame->pts_us)
                                    {
                                        result = *it;
                                        break;
                                    }
                                }
                            }
                        }
                        if (control.epoch_stop || control.stop ||
                            (control.file && control.paused && !frame->preview_step))
                        {
                            continue;
                        }
                        const auto detection_pts = result.value("pts_us", int64_t(AV_NOPTS_VALUE));
                        const double age =
                            detection_pts == AV_NOPTS_VALUE || frame->pts_us == AV_NOPTS_VALUE
                                ? -1
                                : double(frame->pts_us - detection_pts) / 1000;
                        result["detection_frame"] = result.value("frame", uint64_t(0));
                        result["detection_pts_us"] = detection_pts;
                        result["detection_age_ms"] = age;
                        if (age < 0 ||
                            age >
                                std::clamp(result.value("inference_ms", 0.0) * 3, 200.0, 1500.0) ||
                            result.value("source_width", 0) !=
                                int(mpp_frame_get_width(frame->value)) ||
                            result.value("source_height", 0) !=
                                int(mpp_frame_get_height(frame->value)))
                        {
                            result["detections"] = Json::array();
                            result.erase("classification");
                            result.erase("dense");
                        }
                        result["frame"] = frame->sequence;
                        result["pts_us"] = frame->pts_us;
                        result["source_width"] = mpp_frame_get_width(frame->value);
                        result["source_height"] = mpp_frame_get_height(frame->value);
                        try
                        {
                            if (!preview)
                            {
                                preview = std::make_unique<JpegPreview>();
                            }
                            auto bytes = preview->encode(*frame);
                            if (config.contains("thumbnail_file"))
                            {
                                std::ofstream image(config.at("thumbnail_file").get<std::string>(),
                                                    std::ios::binary);
                                image.write(reinterpret_cast<const char *>(bytes.data()),
                                            std::streamsize(bytes.size()));
                                require(bool(image), "write hardware thumbnail");
                                control.stop = true;
                                control.epoch_stop = true;
                            }
                            result["run_id"] = config.at("run_id");
                            result["epoch"] = epoch;
                            result["channel"] = config.at("id");
                            result["position"] = position_of(*frame);
                            result["preview_age_ms"] = milliseconds(frame->decoded_at);
                            send_preview(config.value("preview_fd", 3), result, bytes);
                            if (preview_fps > 0)
                            {
                                auto interval = std::chrono::microseconds(1000000 / preview_fps);
                                next_preview += interval;
                                if (next_preview < Clock::now())
                                {
                                    next_preview = Clock::now() + interval;
                                }
                            }
                        }
                        catch (const std::exception &error)
                        {
                            control.update({{"preview_error", error.what()}});
                            control.preview = false;
                            preview.reset();
                            control.publish();
                        }
                    }
                });

            // Capture follows the source clock and supplies both consumer queues.
            std::exception_ptr decode_error;
            try
            {
                bool step = control.paused;
                uint64_t clock_revision = control.clock_revision.load();
                int64_t base_pts = AV_NOPTS_VALUE;
                auto base_clock = Clock::now();
                const int every = config.value("detect_every", 1);
                decoder.run(
                    recorder,
                    [&](FramePtr frame)
                    {
                        ++decoded;
                        if (control.epoch_stop || control.stop)
                        {
                            return;
                        }
                        double position = position_of(*frame);
                        if (control.file && position + 0.0001 < target)
                        {
                            return;
                        }
                        if (control.file)
                        {
                            while (control.paused && !step && !control.epoch_stop && !control.stop)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                            if (control.epoch_stop || control.stop)
                            {
                                return;
                            }
                            if (base_pts == AV_NOPTS_VALUE ||
                                clock_revision != control.clock_revision.load())
                            {
                                base_pts = static_cast<int64_t>(position * 1e6);
                                base_clock = Clock::now();
                                clock_revision = control.clock_revision.load();
                            }
                            if (!step)
                            {
                                auto due = base_clock +
                                           std::chrono::microseconds(
                                               static_cast<int64_t>(position * 1e6) - base_pts);
                                while (Clock::now() < due && !control.epoch_stop && !control.stop &&
                                       !control.paused)
                                {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                }
                                if (control.paused)
                                {
                                    return;
                                }
                            }
                            frame->preview_step = step;
                            step = false;
                        }
                        if (control.epoch_stop || control.stop)
                        {
                            return;
                        }
                        control.update(
                            {{"position", position},
                             {"decoded", decoded.load()},
                             {"decode_fps",
                              double(decoded) / std::max(0.001, milliseconds(started) / 1000)},
                             {"queue_drops", queue->dropped.load()},
                             {"preview_drops", preview_queue->dropped.load()}});
                        if (control.preview)
                        {
                            preview_queue->push(frame, false, control.epoch_stop);
                        }
                        bool active = !control.paused || control.file;
                        if (detector && active &&
                            (frame->preview_step || (frame->sequence - 1) % uint64_t(every) == 0))
                        {
                            // Offline inference must retain every selected frame, including slow
                            // dense tasks.
                            queue->push(std::move(frame), control.file, control.epoch_stop);
                        }
                    });
                // Keep a paused last-frame seek paused even when demux has reached EOF.
                while (control.file && control.paused && !control.epoch_stop && !control.stop)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            catch (...)
            {
                decode_error = std::current_exception();
                control.epoch_stop = true;
            }

            // Join consumers before releasing frames, imported buffers and MPP.
            queue->close();
            preview_queue->close();
            inference.join();
            rendering.join();
            queue->clear();
            preview_queue->clear();
            preview.reset();
            detector.reset();
            recorder.finish();
            {
                std::lock_guard<std::mutex> lock(control.mutex);
                control.queue.reset();
                control.preview_queue.reset();
            }
            if (inference_error)
            {
                std::rethrow_exception(inference_error);
            }
            bool seek = false;
            {
                std::lock_guard<std::mutex> lock(control.mutex);
                seek = control.seek_pending;
                if (seek)
                {
                    target = control.seek_seconds;
                    control.seek_pending = false;
                }
            }
            if (seek && !control.stop)
            {
                ++control.epoch;
                continue;
            }
            if (decode_error && !control.stop)
            {
                std::rethrow_exception(decode_error);
            }
            control.update({{"state", control.stop ? "stopped" : "ended"},
                            {"recorded_packets", recorder.packets},
                            {"invalid_frames", decoder.invalid_frames}});
            control.publish();
            break;
        }
    }
    catch (const std::exception &error)
    {
        if (control.cancel)
        {
            control.update({{"state", "stopped"}});
        }
        else
        {
            control.update({{"state", "error"}, {"error", error.what()}});
            exit_code = 1;
        }
        control.publish();
    }
    control.stop = true;
    control.epoch_stop = true;
    control.done = true;
    commands.join();
    signals.join();
    return exit_code;
}
} // namespace nvr
