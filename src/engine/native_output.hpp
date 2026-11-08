#pragma once

#include "engine/tensor.hpp"
#include <rknn_api.h>

namespace nvr
{
// Owns a bound native output buffer. Its RKNN context must outlive this object.
// Unsupported layouts leave it unbound so callers can use rknn_outputs_get instead.
class NativeOutput
{
    rknn_context context_;
    rknn_tensor_mem *memory_ = nullptr;
    TensorView view_;

public:
    NativeOutput(rknn_context context, const rknn_tensor_attr &logical);
    ~NativeOutput();
    NativeOutput(const NativeOutput &) = delete;
    NativeOutput &operator=(const NativeOutput &) = delete;

    bool bound() const
    {
        return memory_ != nullptr;
    }

    size_t bytes() const
    {
        return memory_ ? memory_->size : 0;
    }

    void *data() const
    {
        return memory_ ? memory_->virt_addr : nullptr;
    }

    TensorView view(const void *storage = nullptr) const;
    void sync(rknn_mem_sync_mode mode = RKNN_MEMORY_SYNC_FROM_DEVICE) const;
};
} // namespace nvr
