#include "engine/pipeline.hpp"

#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>

namespace nvr
{
// Shared DMA buffer ownership.
DmaBuffer::DmaBuffer(size_t requested) : size((requested + 4095) & ~size_t(4095))
{
    int heap = open("/dev/dma_heap/system-uncached", O_RDWR | O_CLOEXEC);
    require(heap >= 0, "open system-uncached DMA heap (check render group permissions)");
    dma_heap_allocation_data request{};
    request.len = size;
    request.fd_flags = O_RDWR | O_CLOEXEC;
    int rc = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &request);
    close(heap);
    require(rc == 0, "allocate uncached DMA-BUF");
    fd = static_cast<int>(request.fd);
    data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
    {
        close(fd);
        fd = -1;
        throw std::runtime_error("map DMA-BUF");
    }
}

DmaBuffer::~DmaBuffer()
{
    if (data && data != MAP_FAILED)
    {
        munmap(data, size);
    }
    if (fd >= 0)
    {
        close(fd);
    }
}

// Model input resizing and color conversion.
Preprocessor::Preprocessor(DmaBuffer &input, int w, int h, int stride, const std::string &mode)
    : width_(w), height_(h), stride_(stride), input_(input), mode_(mode)
{
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
    for (auto &item : sources_)
    {
        releasebuffer_handle(item.second.handle);
    }
    if (sws_)
    {
        sws_freeContext(sws_);
    }
}

Geometry Preprocessor::process(const Frame &f)
{
    auto raw = f.value;
    Geometry g;
    g.width = static_cast<int>(mpp_frame_get_width(raw));
    g.height = static_cast<int>(mpp_frame_get_height(raw));
    int hs = static_cast<int>(mpp_frame_get_hor_stride(raw)),
        vs = static_cast<int>(mpp_frame_get_ver_stride(raw));
    require(mpp_frame_get_fmt(raw) == MPP_FMT_YUV420SP, "expected linear NV12 from MPP");
    require(g.width > 0 && g.height > 0 && hs >= g.width && vs >= g.height,
            "invalid MPP frame strides");

    g.scale = std::min(float(width_) / float(g.width), float(height_) / float(g.height));
    // Even letterbox rectangles also satisfy NV12 chroma alignment requirements.
    g.rw = std::max(2, int(std::round(float(g.width) * g.scale)) & ~1);
    g.rh = std::max(2, int(std::round(float(g.height) * g.scale)) & ~1);
    g.px = ((width_ - g.rw) / 2) & ~1;
    g.py = ((height_ - g.rh) / 2) & ~1;

    MppBuffer buffer = mpp_frame_get_buffer(raw);
    require(buffer && mpp_buffer_get_size(buffer) >= size_t(hs) * size_t(vs) * 3 / 2,
            "invalid MPP frame buffer");

    if (mode_ == "rga")
    {
        int fd = mpp_buffer_get_fd(buffer);
        auto found = sources_.find(fd);
        if (found == sources_.end())
        {
            require(sources_.size() < 96,
                    "source buffer cache limit exceeded (reconnect required)");
            auto handle = importbuffer_fd(fd, static_cast<int>(mpp_buffer_get_size(buffer)));
            // RGA's import owns its DMA-BUF attachment. Do not retain an MppBuffer
            // reference here: that would prevent recycling the decoder's frame pool.
            // The Frame owns the MppFrame throughout synchronous preprocessing.
            require(handle != 0, "RGA import MPP DMA-BUF");
            found = sources_.emplace(fd, Source{handle, buffer}).first;
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
        auto src = wrapbuffer_handle_t(found->second.handle, g.width, g.height, hs, vs,
                                       RK_FORMAT_YCbCr_420_SP);
        // A single synchronous hardware job resizes, converts NV12->RGB and writes
        // directly into RKNN-bound DMA memory. No full-frame CPU pixel copy.
        auto status = improcess(src, dst, {}, im_rect{0, 0, g.width, g.height},
                                im_rect{g.px, g.py, g.rw, g.rh}, {}, -1, nullptr, nullptr, IM_SYNC);
        require(status == IM_STATUS_SUCCESS,
                std::string("RGA preprocess: ") + imStrError_t(status));
    }
    else
    {
        int fd = mpp_buffer_get_fd(buffer);
        dma_buf_sync sync{};
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
        require(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "sync MPP frame for CPU read");
        const auto *base = static_cast<const uint8_t *>(mpp_buffer_get_ptr(buffer));
        require(base, "map MPP frame for CPU baseline");
        // swscale writes much faster into cacheable memory than an uncached DMA
        // mapping. Use a reusable scratch tensor, then one linear upload. This
        // makes the CPU comparison an optimized baseline, not a straw man.
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
        require(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "end CPU frame read");
        require(rows == g.rh, "CPU scaler incomplete frame");
        std::memcpy(input_.data, cpu_output_.data(), input_.size);
    }
    return g;
}
} // namespace nvr
