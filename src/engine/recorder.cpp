#include "engine/recorder.hpp"
#include "engine/common.hpp"

#include <iomanip>
#include <sstream>

namespace nvr
{
Recorder::Recorder(const std::filesystem::path &dir, const AVCodecParameters *params, AVRational tb,
                   double seconds, bool enabled)
    : directory_(dir), time_base_(tb), segment_seconds_(seconds), enabled_(enabled)
{
    require(seconds > 0, "segment_seconds must be positive");
    parameters_ = avcodec_parameters_alloc();
    require(parameters_, "allocate recording codec parameters");
    require(avcodec_parameters_copy(parameters_, params) >= 0, "copy recording codec parameters");
}

Recorder::~Recorder()
{
    try
    {
        finish();
    }
    catch (...)
    {
    }
    if (parameters_)
    {
        avcodec_parameters_free(&parameters_);
    }
}

void Recorder::open(const AVPacket *packet)
{
    std::ostringstream name;
    name << "segment-" << std::setw(6) << std::setfill('0') << segment_++ << ".mkv";
    final_ = directory_ / name.str();
    temporary_ = final_.string() + ".part";
    int rc = avformat_alloc_output_context2(&output_, nullptr, "matroska", temporary_.c_str());
    require(rc >= 0 && output_, "create Matroska recorder");
    AVStream *stream = avformat_new_stream(output_, nullptr);
    require(stream, "create recording stream");
    require(avcodec_parameters_copy(stream->codecpar, parameters_) >= 0, "copy stream parameters");
    stream->codecpar->codec_tag = 0;
    stream->time_base = time_base_;
    require(avio_open(&output_->pb, temporary_.c_str(), AVIO_FLAG_WRITE) >= 0,
            "open recording output");
    rc = avformat_write_header(output_, nullptr);
    require(rc >= 0, "recording header: " + av_error(rc));
    origin_ = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
    require(origin_ != AV_NOPTS_VALUE, "input has no recording timestamp");
    last_dts_ = AV_NOPTS_VALUE;
}

void Recorder::write(const AVPacket *packet)
{
    if (observe_packet)
    {
        observe_packet(packet);
    }
    if (!enabled_)
    {
        return;
    }
    const int64_t timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
    const bool key = packet->flags & AV_PKT_FLAG_KEY;
    // RTSP startup can have PTS but no DTS for reordered frames. Do not let the
    // muxer guess the first DTS: that can precede the next real DTS by 1 ms.
    // Wait for a fully timestamped keyframe; leave normal file packets unchanged.
    if (!output_ && (!key || packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE))
    {
        return;
    }
    require(timestamp != AV_NOPTS_VALUE, "video packet is missing PTS/DTS");
    if (output_ && key && double(timestamp - origin_) * av_q2d(time_base_) >= segment_seconds_)
    {
        finish();
    }
    if (!output_)
    {
        if (!key)
        {
            return;
        }
        open(packet);
    }
    require(std::filesystem::space(directory_).available > 128ULL * 1024 * 1024,
            "less than 128 MiB free; recording stopped");
    AVPacket *copy = av_packet_clone(packet);
    require(copy, "clone recording packet");
    if (copy->pts != AV_NOPTS_VALUE)
    {
        copy->pts -= origin_;
    }
    if (copy->dts != AV_NOPTS_VALUE)
    {
        copy->dts -= origin_;
    }
    av_packet_rescale_ts(copy, time_base_, output_->streams[0]->time_base);
    copy->stream_index = 0;
    copy->pos = -1;
    if (copy->dts != AV_NOPTS_VALUE && last_dts_ != AV_NOPTS_VALUE && copy->dts <= last_dts_)
    {
        av_packet_free(&copy);
        throw std::runtime_error("non-monotonic video DTS; reconnect required");
    }
    last_dts_ = copy->dts;
    int rc = av_interleaved_write_frame(output_, copy);
    av_packet_free(&copy);
    require(rc >= 0, "write recording packet: " + av_error(rc));
    ++packets;
    bytes += static_cast<uint64_t>(packet->size);
}

void Recorder::finish()
{
    if (!output_)
    {
        return;
    }
    int rc = av_write_trailer(output_);
    int close_rc = avio_closep(&output_->pb);
    avformat_free_context(output_);
    output_ = nullptr;
    require(rc >= 0 && close_rc >= 0, "finalize recording failed; .part retained");
    std::filesystem::rename(temporary_, final_);
    files.push_back(final_.filename().string());
}
} // namespace nvr
