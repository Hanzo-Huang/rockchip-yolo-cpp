#include "engine/decoder.hpp"
#include "model/metadata.hpp"
#include "engine/detector_pool.hpp"
#include "engine/recorder.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <thread>

#include <csignal>
#include <iostream>
#include <sys/resource.h>

namespace
{
volatile std::sig_atomic_t interrupted = 0;

void handle_signal(int)
{
    interrupted = 1;
}

std::mutex log_mutex;

void log(const nvr::Json &j)
{
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << j.dump() << std::endl;
}

double percentile(std::vector<double> v, double q)
{
    if (v.empty())
    {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(q * double(v.size() - 1))];
}

nvr::Json run_channel(const nvr::Json &config, const nvr::Json &channel,
                      const std::filesystem::path &directory, std::atomic<bool> &stop)
{
    using namespace nvr;
    const std::string id = channel.at("id"), uri = channel.at("uri");
    const std::string mode = config.value("preprocess", std::string("rga"));
    require(mode == "rga" || mode == "cpu", "preprocess must be rga or cpu");
    const bool lossless = config.value("lossless_analytics", false);
    const int every = config.value("detect_every", 1);
    require(every >= 1, "detect_every must be >=1");
    const double confidence = config.value("confidence", 0.35), iou = config.value("nms_iou", 0.45);
    require(confidence > 0 && confidence < 1 && iou > 0 && iou < 1,
            "thresholds must be between 0 and 1");
    std::filesystem::create_directories(directory);
    std::ofstream events(directory / "detections.jsonl");
    require(bool(events), "create detections log");
    Decoder decoder(uri, stop);
    // Destroy detector/imported source handles before destroying MPP's context.
    DetectorPool detector(config.at("model"), mode, config.value("verify_io", false),
                          float(confidence), float(iou), config.value("inference_workers", 0),
                          configured_model_options(config));
    Recorder recorder(directory, decoder.parameters(), decoder.time_base(),
                      config.value("segment_seconds", 60.0));
    FrameQueue queue(config.value("queue_capacity", size_t(6)));
    log({{"event", "channel_started"},
         {"channel", id},
         {"detector", detector.description},
         {"recording", "video stream-copy Matroska"}});
    std::exception_ptr inference_error;
    uint64_t inferred = 0, objects = 0;
    double preprocess_total = 0, inference_total = 0, post_total = 0;
    std::vector<double> latency;
    latency.reserve(10000);
    auto started = Clock::now();
    std::thread inference(
        [&]
        {
            try
            {
                detector.run(
                    queue,
                    [&](const Frame &)
                    {
                        return !stop;
                    },
                    [&](const Frame &, Json result)
                    {
                        result["channel"] = id;
                        ++inferred;
                        objects += result["detections"].size();
                        preprocess_total += result["preprocess_ms"].get<double>();
                        inference_total += result["inference_ms"].get<double>();
                        post_total += result["postprocess_ms"].get<double>();
                        if (latency.size() == 10000)
                        {
                            latency.erase(latency.begin(), latency.begin() + 5000);
                        }
                        latency.push_back(result["age_ms"].get<double>());
                        events << result.dump() << '\n';
                        require(bool(events), "write detection events");
                        if (inferred % 30 == 0)
                        {
                            log({{"event", "progress"},
                                 {"channel", id},
                                 {"inferred", inferred},
                                 {"age_ms", result["age_ms"]},
                                 {"queue_drops", queue.dropped.load()}});
                        }
                    });
                events.flush();
            }
            catch (...)
            {
                inference_error = std::current_exception();
                stop = true;
                queue.close();
            }
        });
    std::exception_ptr capture_error;
    try
    {
        decoder.run(recorder,
                    [&](FramePtr frame)
                    {
                        if ((frame->sequence - 1) % static_cast<uint64_t>(every) == 0)
                        {
                            queue.push(std::move(frame), lossless, stop);
                        }
                    });
    }
    catch (...)
    {
        capture_error = std::current_exception();
        stop = true;
    }
    queue.close();
    inference.join();
    queue.clear();
    recorder.finish();
    if (inference_error)
    {
        std::rethrow_exception(inference_error);
    }
    if (capture_error)
    {
        std::rethrow_exception(capture_error);
    }
    const double elapsed = milliseconds(started) / 1000.0;
    Json summary = {
        {"channel", id},
        {"decoded", decoder.frames()},
        {"invalid_frames", decoder.invalid_frames},
        {"inferred", inferred},
        {"objects", objects},
        {"queue_drops", queue.dropped.load()},
        {"elapsed_seconds", elapsed},
        {"decode_fps", double(decoder.frames()) / std::max(elapsed, 0.001)},
        {"analytics_fps", double(inferred) / std::max(elapsed, 0.001)},
        {"preprocess_avg_ms", preprocess_total / double(std::max<uint64_t>(inferred, 1))},
        {"inference_avg_ms", inference_total / double(std::max<uint64_t>(inferred, 1))},
        {"postprocess_avg_ms", post_total / double(std::max<uint64_t>(inferred, 1))},
        {"age_p50_ms", percentile(latency, 0.50)},
        {"age_p95_ms", percentile(latency, 0.95)},
        {"recorded_packets", recorder.packets},
        {"recorded_bytes", recorder.bytes},
        {"segments", recorder.files},
        {"output", directory.string()},
        {"preprocess", mode},
        {"clean_shutdown", true}};
    std::ofstream out(directory / "summary.json");
    out << summary.dump(2) << '\n';
    require(bool(out), "write summary");
    log({{"event", "channel_finished"}, {"summary", summary}});
    return summary;
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        nvr::require(argc == 2, "Usage: rk-nvr config.json");
        std::ifstream input(argv[1]);
        nvr::require(bool(input), "open configuration");
        nvr::Json config;
        nvr::Json summaries = nvr::Json::array();
        input >> config;
        const auto &channels = config.at("channels");
        nvr::require(channels.is_array() && !channels.empty() && channels.size() <= 8,
                     "provide 1..8 channels");
        std::set<std::string> names;
        for (const auto &c : channels)
        {
            std::string id = c.at("id");
            nvr::require(
                !id.empty() &&
                    id.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
                        std::string::npos,
                "invalid channel id");
            nvr::require(names.insert(id).second, "duplicate channel id");
        }
        const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        auto directory = std::filesystem::path(config.value("output", std::string("recordings"))) /
                         ("session-" + std::to_string(stamp));
        nvr::require(std::filesystem::create_directories(directory),
                     "session output already exists");
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::atomic<bool> stop{false}, done{false};
        std::atomic<int> failures{0};
        std::mutex summaries_mutex;
        avformat_network_init();
        auto start = nvr::Clock::now();
        double limit = config.value("max_seconds", 0.0);
        std::thread monitor(
            [&]
            {
                while (!done)
                {
                    if (interrupted || (limit > 0 && nvr::milliseconds(start) >= limit * 1000))
                    {
                        stop = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
        std::vector<std::thread> workers;
        for (const auto &c : channels)
        {
            workers.emplace_back(
                [&, c]
                {
                    try
                    {
                        auto result =
                            run_channel(config, c, directory / c.at("id").get<std::string>(), stop);
                        std::lock_guard<std::mutex> lock(summaries_mutex);
                        summaries.push_back(std::move(result));
                    }
                    catch (const std::exception &e)
                    {
                        ++failures;
                        stop = true;
                        log({{"event", "error"}, {"channel", c.at("id")}, {"message", e.what()}});
                    }
                });
        }
        for (auto &worker : workers)
        {
            worker.join();
        }
        done = true;
        monitor.join();
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        double wall = nvr::milliseconds(start) / 1000;
        double cpu = double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                     double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
        nvr::Json report = {{"event", "session_finished"},
                            {"ok", failures == 0},
                            {"wall_seconds", wall},
                            {"process_cpu_percent", 100 * cpu / std::max(wall, 0.001)},
                            {"max_rss_kib", usage.ru_maxrss},
                            {"channels", summaries}};
        std::ofstream file(directory / "session.json");
        file << report.dump(2) << '\n';
        log(report);
        return failures ? 1 : 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "rk-nvr: " << e.what() << '\n';
        return 1;
    }
}
