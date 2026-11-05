#pragma once

#include "engine/media.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nvr
{
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

    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    void write(const AVPacket *packet);
    void finish();
};
} // namespace nvr
