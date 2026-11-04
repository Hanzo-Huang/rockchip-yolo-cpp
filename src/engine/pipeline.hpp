#pragma once

#include <atomic>
#include <algorithm>
#include <map>
#include <set>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <rk_mpi.h>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libswscale/swscale.h>
}

#include <im2d.hpp>
#include <rknn_api.h>

namespace nvr
{
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

inline double milliseconds(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

inline void require(bool ok, const std::string &message)
{
    if (!ok)
    {
        throw std::runtime_error(message);
    }
}

inline std::string av_error(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

// Decoded frame ownership.
struct Frame
{
    MppFrame value = nullptr;
    uint64_t sequence = 0;
    int64_t pts_us = AV_NOPTS_VALUE;
    Clock::time_point decoded_at = Clock::now();
    bool preview_step = false;

    explicit Frame(MppFrame f) : value(f)
    {
    }

    ~Frame()
    {
        if (value)
        {
            mpp_frame_deinit(&value);
        }
    }

    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;
};

using FramePtr = std::shared_ptr<Frame>;

// Exactly one decoder producer and one inference consumer per channel. A live
// stream replaces queued frames; offline verification applies back-pressure.
class FrameQueue
{
    std::mutex mutex_;
    std::condition_variable ready_, space_;
    std::deque<FramePtr> queue_;
    size_t capacity_;
    bool closed_ = false;

public:
    std::atomic<uint64_t> dropped{0};

    explicit FrameQueue(size_t n) : capacity_(n)
    {
        require(n > 0 && n <= 8, "queue_capacity must be 1..8");
    }

    void push(FramePtr f, bool lossless, const std::atomic<bool> &stop)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (lossless && queue_.size() >= capacity_ && !closed_ && !stop)
        {
            space_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (closed_ || stop)
        {
            return;
        }
        if (queue_.size() >= capacity_)
        {
            queue_.pop_front();
            ++dropped;
        }
        queue_.push_back(std::move(f));
        ready_.notify_one();
    }

    FramePtr pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock,
                    [&]
                    {
                        return closed_ || !queue_.empty();
                    });
        if (queue_.empty())
        {
            return {};
        }
        auto result = std::move(queue_.front());
        queue_.pop_front();
        space_.notify_one();
        return result;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
        space_.notify_all();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
};

// Compressed packet recording.
class Recorder
{
    std::filesystem::path directory_, temporary_, final_;
    AVFormatContext *output_ = nullptr;
    AVCodecParameters *parameters_ = nullptr;
    AVRational time_base_{};
    int segment_ = 0;
    double segment_seconds_;
    int64_t origin_ = AV_NOPTS_VALUE, last_dts_ = AV_NOPTS_VALUE;
    bool enabled_ = true;

    void open(const AVPacket *packet);

public:
    std::function<void(const AVPacket *)> observe_packet;
    uint64_t packets = 0, bytes = 0;
    std::vector<std::string> files;

    Recorder(const std::filesystem::path &directory, const AVCodecParameters *parameters,
             AVRational time_base, double segment_seconds, bool enabled = true);
    ~Recorder();

    void write(const AVPacket *packet);
    void finish();
};

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
    void run(Recorder &recorder, const std::function<void(FramePtr)> &consume);
};

// Shared model input memory and preprocessing.
class DmaBuffer
{
public:
    int fd = -1;
    void *data = nullptr;
    size_t size = 0;

    explicit DmaBuffer(size_t bytes);
    ~DmaBuffer();
    DmaBuffer(const DmaBuffer &) = delete;
};

struct Geometry
{
    int width = 0, height = 0, rw = 0, rh = 0, px = 0, py = 0;
    float scale = 1;
};

class Preprocessor
{
    struct Source
    {
        rga_buffer_handle_t handle = 0;
        MppBuffer buffer = nullptr;
    };

    std::map<int, Source> sources_;
    rga_buffer_handle_t destination_ = 0;
    SwsContext *sws_ = nullptr;
    int width_, height_, stride_;
    DmaBuffer &input_;
    std::string mode_;
    bool painted_ = false;
    Geometry previous_;
    std::vector<uint8_t> cpu_output_;

public:
    Preprocessor(DmaBuffer &input, int width, int height, int stride, const std::string &mode);
    ~Preprocessor();

    Geometry process(const Frame &frame);
};

// RKNN inference and detection postprocessing.
class Detector
{
    rknn_context context_ = 0;
    rknn_tensor_attr input_attr_{}, output_attr_{};
    rknn_tensor_mem *tensor_ = nullptr;
    std::unique_ptr<DmaBuffer> input_;
    std::unique_ptr<Preprocessor> preprocessor_;
    std::vector<float> output_;
    int width_ = 0, height_ = 0, stride_ = 0;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<std::vector<int8_t>> quant_outputs_;
    bool dfl_ = false;
    bool split_ = false, scores_first_ = true, boxes_first_ = true;
    int scores_index_ = -1, boxes_index_ = -1;
    int features_ = 0, proposals_ = 0;
    bool features_first_ = true, verify_, verified_ = false;
    float threshold_, iou_;
    std::string mode_;
    double last_run_ms_ = 0, last_output_ms_ = 0;

    void run_output(bool copy_input);
    Json decode(const Geometry &geometry);

public:
    Json description;

    Detector(const std::string &model, const std::string &preprocess, bool verify, float confidence,
             float iou);
    ~Detector();

    Json infer(const Frame &frame);
};
} // namespace nvr
