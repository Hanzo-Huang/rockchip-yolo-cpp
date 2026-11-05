#pragma once

#include "engine/frame.hpp"
#include "engine/media.hpp"

#include <atomic>
#include <functional>

extern "C"
{
#include <libavcodec/bsf.h>
}

namespace nvr
{
class Recorder;

// FFmpeg input and MPP hardware decoding.
class Decoder
{
    AVFormatContext *format_ = nullptr;
    AVBSFContext *bsf_ = nullptr;
    MppCtx context_ = nullptr;
    MppApi *api_ = nullptr;
    MppBufferGroup group_ = nullptr;
    int stream_ = -1;
    std::atomic<bool> &stop_;
    std::atomic<int64_t> deadline_ns_{0};
    uint64_t sequence_ = 0;
    Clock::time_point progress_ = Clock::now();

    static int interrupt(void *opaque);
    void deadline();

    bool drain(const std::function<void(FramePtr)> &consume, bool &eos);
    void submit(AVPacket *packet, bool end_of_stream, const std::function<void(FramePtr)> &consume,
                bool &received_eos);

public:
    uint64_t invalid_frames = 0;

    Decoder(const std::string &uri, std::atomic<bool> &stop, bool camera = false);
    ~Decoder();

    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    const AVCodecParameters *parameters() const
    {
        return format_->streams[stream_]->codecpar;
    }

    AVRational time_base() const
    {
        return format_->streams[stream_]->time_base;
    }

    uint64_t frames() const
    {
        return sequence_;
    }

    double duration_seconds() const;
    double start_seconds() const;
    double frame_rate() const;

    void seek(double seconds);
    // Packet views are borrowed for the callback. Decoded frames transfer shared ownership.
    void run(const std::function<void(FramePtr)> &consume,
             const std::function<void(const AVPacket *)> &observe_packet = {});

    // Convenience overload for existing recording pipelines.
    void run(Recorder &recorder, const std::function<void(FramePtr)> &consume);
};
} // namespace nvr
