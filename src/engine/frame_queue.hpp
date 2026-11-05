#pragma once

#include "engine/frame.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace nvr
{
// One producer and multiple consumers. Live input drops the oldest queued frame;
// lossless input waits for capacity. Closing wakes both producers and consumers.
class FrameQueue
{
    std::mutex mutex_;
    std::condition_variable ready_, space_;
    std::deque<FramePtr> queue_;
    size_t capacity_;
    bool closed_ = false;

public:
    std::atomic<uint64_t> dropped{0};

    explicit FrameQueue(size_t capacity);

    void push(FramePtr frame, bool lossless, const std::atomic<bool> &stop);
    FramePtr pop();
    void close();
    void clear();
};
} // namespace nvr
