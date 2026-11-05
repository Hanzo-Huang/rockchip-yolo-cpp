#include "engine/preprocess.hpp"
#include "engine/common.hpp"

#include <cstring>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

namespace nvr
{
// Model input resizing and color conversion.
Preprocessor::Preprocessor(DmaBuffer &input, int w, int h, int stride, const std::string &mode)
    : width_(w), height_(h), stride_(stride), input_(input), mode_(mode)
{
    require(mode_ == "rga" || mode_ == "cpu", "preprocess must be rga or cpu");
    require(w > 0 && h > 0 && w % 16 == 0 && h % 2 == 0 && stride >= w && stride % 16 == 0,
            "invalid RGB destination dimensions or stride");
    require(input.fd >= 0 && input.data && input.size >= size_t(stride) * size_t(h) * 3,
            "invalid RGB destination DMA buffer");
    if (mode_ == "rga")
    {
        destination_ = importbuffer_fd(input_.fd, static_cast<int>(input_.size));
        require(destination_ != 0, "RGA import RKNN input DMA-BUF");
    }
}

Preprocessor::~Preprocessor()
{
    if (destination_)
    {
        releasebuffer_handle(destination_);
    }
    clear_sources();
    if (sws_)
    {
        sws_freeContext(sws_);
    }
}

void Preprocessor::clear_sources()
{
    for (auto &item : sources_)
    {
        releasebuffer_handle(item.second);
    }
    sources_.clear();
}

Geometry Preprocessor::process(const Nv12ImageView &image)
{
    const int hs = image.horizontal_stride, vs = image.vertical_stride;
    require(image.width > 0 && image.height > 0 && hs >= image.width && vs >= image.height,
            "invalid NV12 image strides");
    require(image.size >= size_t(hs) * size_t(vs) * 3 / 2, "invalid NV12 image buffer size");
    const auto g = letterbox_geometry(image.width, image.height, width_, height_);

    if (mode_ == "rga")
    {
        int fd = image.fd;
        require(fd >= 0, "RGA preprocessing needs a DMA-BUF file descriptor");
        auto found = sources_.find(fd);
        if (found == sources_.end())
        {
            require(sources_.size() < 96,
                    "source buffer cache limit exceeded (reconnect required)");
            auto handle = importbuffer_fd(fd, static_cast<int>(image.size));
            // The import owns its DMA-BUF attachment. The caller retains the source
            // during processing; no decoder-specific buffer reference is stored.
            require(handle != 0, "RGA import NV12 DMA-BUF");
            found = sources_.emplace(fd, handle).first;
        }
        auto dst =
            wrapbuffer_handle_t(destination_, width_, height_, stride_, height_, RK_FORMAT_RGB_888);
        if (!painted_ || previous_.rw != g.rw || previous_.rh != g.rh || previous_.px != g.px ||
            previous_.py != g.py)
        {
            // RK3588 color fill needs RGA2 and below-4-GiB memory. Initialize the
            // constant background once instead, so ordinary DMA heaps also work.
            // Every image is still resized/converted by RGA directly into this buffer.
            dma_buf_sync sync{};
            sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
            require(ioctl(input_.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0,
                    "sync model input for letterbox initialization");
            std::memset(input_.data, 114, input_.size);
            sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
            require(ioctl(input_.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0,
                    "finish letterbox initialization");
            painted_ = true;
            previous_ = g;
        }
        auto src =
            wrapbuffer_handle_t(found->second, g.width, g.height, hs, vs, RK_FORMAT_YCbCr_420_SP);
        // A single synchronous hardware job resizes, converts NV12->RGB and writes
        // directly into RKNN-bound DMA memory. No full-frame CPU pixel copy.
        auto status = improcess(src, dst, {}, im_rect{0, 0, g.width, g.height},
                                im_rect{g.px, g.py, g.rw, g.rh}, {}, -1, nullptr, nullptr, IM_SYNC);
        require(status == IM_STATUS_SUCCESS,
                std::string("RGA preprocess: ") + imStrError_t(status));
    }
    else
    {
        int fd = image.fd;
        dma_buf_sync sync{};
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
        if (fd >= 0)
        {
            require(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "sync image for CPU read");
        }
        const auto *base = image.data;
        require(base, "CPU preprocessing needs mapped NV12 storage");
        // swscale writes much faster into cacheable memory than an uncached DMA
        // mapping. Use a reusable scratch tensor, then one linear upload. This
        // avoids repeatedly writing scaled pixels into uncached memory.
        if (cpu_output_.size() != input_.size)
        {
            cpu_output_.resize(input_.size);
        }
        std::memset(cpu_output_.data(), 114, cpu_output_.size());
        sws_ = sws_getCachedContext(sws_, g.width, g.height, AV_PIX_FMT_NV12, g.rw, g.rh,
                                    AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        require(sws_, "create CPU NV12/RGB scaler");
        const uint8_t *planes[4] = {base, base + size_t(hs) * size_t(vs), nullptr, nullptr};
        int lines[4] = {hs, hs, 0, 0};
        uint8_t *out[4] = {cpu_output_.data() + (g.py * stride_ + g.px) * 3, nullptr, nullptr,
                           nullptr};
        int out_lines[4] = {stride_ * 3, 0, 0, 0};
        int rows = sws_scale(sws_, planes, lines, 0, g.height, out, out_lines);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        if (fd >= 0)
        {
            require(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "end CPU image read");
        }
        require(rows == g.rh, "CPU scaler incomplete frame");
        std::memcpy(input_.data, cpu_output_.data(), input_.size);
    }
    return g;
}
} // namespace nvr
