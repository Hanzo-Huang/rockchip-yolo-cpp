#include "engine/detector_pool.hpp"

#include <fstream>
#include <thread>

#include <sched.h>

namespace
{
void prefer_performance_cpus()
{
    // RK3576 and RK3588 place their performance CPUs at IDs 4..7. Keep
    // inference on those CPUs without overriding a caller's restricted CPU set.
    cpu_set_t allowed, performance;
    CPU_ZERO(&performance);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    {
        return;
    }
    for (int cpu = 4; cpu < 8; ++cpu)
    {
        if (CPU_ISSET(cpu, &allowed))
        {
            CPU_SET(cpu, &performance);
        }
    }
    if (CPU_COUNT(&performance) > 0)
    {
        // Affinity is a best-effort optimization, never a prerequisite to inference.
        (void)sched_setaffinity(0, sizeof(performance), &performance);
    }
}
} // namespace

namespace nvr
{
DetectorPool::DetectorPool(const std::string &model, const std::string &preprocess, bool verify,
                           float confidence, float iou, int workers)
{
    std::ifstream compatible("/proc/device-tree/compatible", std::ios::binary);
    std::string chip((std::istreambuf_iterator<char>(compatible)), {});
    const int cores = chip.find("rockchip,rk3588") != std::string::npos   ? 3
                      : chip.find("rockchip,rk3576") != std::string::npos ? 2
                                                                          : 1;
    require(workers == 0 || workers == 1, "inference_workers must be 0 (auto) or 1 (single context)");
    if (workers == 0)
    {
        workers = cores;
    }
    Json masks = Json::array();
    for (int i = 0; i < workers; ++i)
    {
        auto mask = workers == 1 ? RKNN_NPU_CORE_ALL : static_cast<rknn_core_mask>(1 << i);
        detectors_.push_back(
            std::make_unique<Detector>(model, preprocess, verify, confidence, iou, mask));
        masks.push_back(detectors_.back()->description.at("core_mask"));
    }
    description = detectors_.front()->description;
    description["inference_workers"] = workers;
    description["core_masks"] = masks;
    description["prefer_performance_cpus"] = cores > 1;
    description["core_mask"] = workers == 1 ? "all" : "per-worker";
}

void DetectorPool::run(FrameQueue &queue, const std::function<bool(const Frame &)> &active,
                       const std::function<void(const Frame &, Json)> &consume)
{
    std::mutex mutex, dequeue_mutex;
    std::condition_variable ready;
    uint64_t issued = 0, published = 0;
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::vector<std::thread> threads;
    auto fail = [&]
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!error)
        {
            error = std::current_exception();
        }
        failed = true;
        queue.close();
        ready.notify_all();
    };
    try
    {
        for (auto &detector : detectors_)
        {
            threads.emplace_back(
                [&, worker = detector.get()]
                {
                    try
                    {
                        if (description.at("prefer_performance_cpus").get<bool>())
                        {
                            prefer_performance_cpus();
                        }
                        while (true)
                        {
                            FramePtr frame;
                            uint64_t ticket;
                            {
                                // Assign tickets in source order, including across idle workers.
                                std::lock_guard<std::mutex> lock(dequeue_mutex);
                                if (failed)
                                {
                                    break;
                                }
                                frame = queue.pop();
                                if (!frame)
                                {
                                    break;
                                }
                                ticket = issued++;
                            }
                            Json result;
                            if (active(*frame))
                            {
                                result = worker->infer(*frame);
                            }
                            std::unique_lock<std::mutex> lock(mutex);
                            ready.wait(lock,
                                       [&]
                                       {
                                           return failed || ticket == published;
                                       });
                            if (failed)
                            {
                                break;
                            }
                            if (!result.is_null() && active(*frame))
                            {
                                consume(*frame, std::move(result));
                            }
                            ++published;
                            ready.notify_all();
                        }
                    }
                    catch (...)
                    {
                        fail();
                    }
                });
        }
    }
    catch (...)
    {
        fail();
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    if (error)
    {
        std::rethrow_exception(error);
    }
}
} // namespace nvr
