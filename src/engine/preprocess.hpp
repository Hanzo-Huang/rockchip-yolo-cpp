#pragma once

#include "engine/dma_buffer.hpp"
#include "engine/image.hpp"
#include "engine/geometry.hpp"

#include <map>
#include <string>
#include <vector>
#include <im2d.hpp>

extern "C"
{
#include <libswscale/swscale.h>
}

namespace nvr
{
// One instance per processing thread. The destination must outlive this object.
// Clear source imports before replacing a source buffer pool whose FDs may be reused.
class Preprocessor
{
    std::map<int, rga_buffer_handle_t> sources_;
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

    Preprocessor(const Preprocessor &) = delete;
    Preprocessor &operator=(const Preprocessor &) = delete;

    Geometry process(const Nv12ImageView &image);
    void clear_sources();
};
} // namespace nvr
