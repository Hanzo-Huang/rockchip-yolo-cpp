#pragma once

#include "engine/pipeline.hpp"

namespace nvr
{
struct WorkerControl
{
    std::atomic<bool> stop{false}, epoch_stop{false}, paused{false}, preview{false}, done{false},
        cancel{false};
    std::atomic<int> preview_fps{0};
    std::atomic<bool> streaming{false};
    std::atomic<uint64_t> epoch{1}, clock_revision{0};

    std::mutex mutex, output_mutex;
    Json state = {{"state", "starting"}, {"position", 0.0}, {"duration", 0.0}};
    double seek_seconds = 0;
    bool seek_pending = false, file = true;

    Clock::time_point rate_started = Clock::now();
    uint64_t rate_epoch = 0, rate_decoded = 0, rate_inferred = 0;
    double recent_decode = 0, recent_inference = 0;

    std::shared_ptr<FrameQueue> queue, preview_queue;

    void update(const Json &value);
    void publish();
    void read_commands();
};

int run_worker(const Json &config);
} // namespace nvr
