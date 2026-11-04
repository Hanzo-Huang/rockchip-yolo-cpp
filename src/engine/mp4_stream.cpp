#include "engine/mp4_stream.hpp"

#include <cstdio>

namespace nvr
{
int Mp4Stream::write(void *opaque, const uint8_t *data, int size)
{
    auto &self = *static_cast<Mp4Stream *>(opaque);
    if (self.bytes_.size() + size > 8 * 1024 * 1024)
    {
        return AVERROR(ENOBUFS);
    }
    self.bytes_.insert(self.bytes_.end(), data, data + size);
    return size;
}

Mp4Stream::Mp4Stream(const AVCodecParameters *parameters, AVRational tb,
                     std::function<void(const Json &, const std::vector<uint8_t> &)> output)
    : time_base_(tb), output_(std::move(output))
{
    require(parameters->codec_id == AV_CODEC_ID_H264, "Continuous browser playback requires H.264");
    require(avformat_alloc_output_context2(&format_, nullptr, "mp4", nullptr) >= 0 && format_,
            "Create MP4 stream");
    auto *stream = avformat_new_stream(format_, nullptr);
    require(stream && avcodec_parameters_copy(stream->codecpar, parameters) >= 0,
            "Copy MP4 stream parameters");
    stream->codecpar->codec_tag = 0;
    stream->time_base = tb;
    auto *buffer = static_cast<uint8_t *>(av_malloc(32768));
    io_ = avio_alloc_context(buffer, 32768, 1, this, nullptr, write, nullptr);
    require(io_, "Allocate MP4 stream IO");
    format_->pb = io_;
    format_->flags |= AVFMT_FLAG_CUSTOM_IO;
}

Mp4Stream::~Mp4Stream()
{
    if (format_)
    {
        avformat_free_context(format_);
    }
    if (io_)
    {
        av_freep(&io_->buffer);
        avio_context_free(&io_);
    }
}

void Mp4Stream::emit(bool init)
{
    avio_flush(io_);
    require(io_->error >= 0, "MP4 stream output failed");
    if (bytes_.empty())
    {
        return;
    }
    if (init)
    {
        const std::vector<uint8_t> marker = {'a', 'v', 'c', 'C'};
        auto found = std::search(bytes_.begin(), bytes_.end(), marker.begin(), marker.end());
        require(found != bytes_.end() && bytes_.end() - found >= 8,
                "MP4 has no AVC decoder configuration");
        char value[32];
        std::snprintf(value, sizeof(value), "avc1.%02x%02x%02x", found[5], found[6], found[7]);
        codec_ = value;
    }
    output_({{"kind", "video"},
             {"init", init},
             {"key", fragment_key_},
             {"origin_us", av_rescale_q(origin_, time_base_, AVRational{1, 1000000})},
             {"mime", "video/mp4; codecs=\"" + codec_ + "\""}},
            bytes_);
    bytes_.clear();
}

void Mp4Stream::packet(const AVPacket *packet)
{
    if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE)
    {
        return;
    }
    const bool key = packet->flags & AV_PKT_FLAG_KEY;
    if (origin_ == AV_NOPTS_VALUE)
    {
        if (!key)
        {
            return;
        }
        origin_ = packet->dts;
        AVDictionary *options = nullptr;
        av_dict_set(&options, "movflags", "empty_moov+default_base_moof+frag_custom", 0);
        int rc = avformat_write_header(format_, &options);
        av_dict_free(&options);
        require(rc >= 0, "MP4 stream header: " + av_error(rc));
        emit(true);
    }
    // Keep reordered B pictures in complete keyframe groups. Cutting arbitrary
    // 100 ms fragments can overlap presentation times across MSE append groups.
    if (fragment_start_ != AV_NOPTS_VALUE && key)
    {
        require(av_write_frame(format_, nullptr) >= 0, "Flush MP4 fragment");
        emit(false);
        fragment_start_ = AV_NOPTS_VALUE;
    }
    if (fragment_start_ == AV_NOPTS_VALUE)
    {
        fragment_start_ = packet->dts;
        fragment_key_ = key;
    }
    AVPacket *copy = av_packet_clone(packet);
    require(copy, "Clone compressed stream packet");
    copy->pts -= origin_;
    copy->dts -= origin_;
    av_packet_rescale_ts(copy, time_base_, format_->streams[0]->time_base);
    copy->stream_index = 0;
    copy->pos = -1;
    int rc = av_write_frame(format_, copy);
    av_packet_free(&copy);
    require(rc >= 0, "Write MP4 stream: " + av_error(rc));
}
} // namespace nvr
