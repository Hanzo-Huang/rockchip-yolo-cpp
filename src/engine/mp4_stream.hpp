#pragma once

#include "engine/media.hpp"
#include "engine/json.hpp"

#include <functional>
#include <string>
#include <vector>

namespace nvr
{
// Stream-copy compressed H.264 into short MSE fragments. No decode or encode.
class Mp4Stream
{
    AVFormatContext *format_ = nullptr;
    AVIOContext *io_ = nullptr;
    AVRational time_base_;
    int64_t origin_ = AV_NOPTS_VALUE, fragment_start_ = AV_NOPTS_VALUE;
    bool fragment_key_ = false;
    std::vector<uint8_t> bytes_;
    std::function<void(const Json &, const std::vector<uint8_t> &)> output_;
    std::string codec_;

    static int write(void *, const uint8_t *, int);

    // FFmpeg 7+ made this callback const; retain the older SDK signature too.
    static int write(void *opaque, uint8_t *data, int size)
    {
        return write(opaque, static_cast<const uint8_t *>(data), size);
    }

    void emit(bool init);

public:
    Mp4Stream(const AVCodecParameters *, AVRational,
              std::function<void(const Json &, const std::vector<uint8_t> &)>);
    ~Mp4Stream();

    Mp4Stream(const Mp4Stream &) = delete;
    Mp4Stream &operator=(const Mp4Stream &) = delete;

    void packet(const AVPacket *);
};
} // namespace nvr
