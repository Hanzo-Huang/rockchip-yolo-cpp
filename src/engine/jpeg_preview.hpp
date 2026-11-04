#pragma once

#include "engine/pipeline.hpp"

namespace nvr
{
// Owned by one inference thread. Only compressed JPEG bytes leave the worker.
class JpegPreview
{
    MppCtx context_ = nullptr;
    MppApi *api_ = nullptr;
    MppEncCfg cfg_ = nullptr;
    MppBuffer input_buffer_ = nullptr;
    std::unique_ptr<DmaBuffer> image_;
    rga_buffer_handle_t destination_ = 0;
    std::map<int, rga_buffer_handle_t> sources_;
    int width_ = 0, height_ = 0, hs_ = 0, vs_ = 0;

    void initialize(int width, int height);

public:
    ~JpegPreview();

    std::vector<uint8_t> encode(const Frame &frame);
};
} // namespace nvr
