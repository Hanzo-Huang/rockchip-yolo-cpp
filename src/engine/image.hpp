#pragma once

#include <cstddef>
#include <cstdint>

namespace nvr
{
// Borrowed NV12 storage. The caller keeps the DMA-BUF and mapping alive through
// processing. Strides describe the allocated luma plane, including padding.
struct Nv12ImageView
{
    int width = 0, height = 0;
    int horizontal_stride = 0, vertical_stride = 0;
    int fd = -1;
    const uint8_t *data = nullptr;
    size_t size = 0;
};
} // namespace nvr
