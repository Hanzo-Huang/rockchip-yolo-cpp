#pragma once

#include "engine/detector.hpp"
#include "engine/frame_queue.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace nvr
{
// Each worker owns its RKNN context, DMA input, preprocessing and output storage.
// Results are published in dequeue order even when cores finish out of order.
class DetectorPool
{
    std::vector<std::unique_ptr<Detector>> detectors_;

public:
    Json description;
    DetectorPool(const std::string &model, const std::string &preprocess, bool verify,
                 float confidence, float iou, int workers = 0, const ModelOptions &options = {});
    void run(FrameQueue &queue, const std::function<bool(const Frame &)> &active,
             const std::function<void(const Frame &, Json)> &consume);
};
} // namespace nvr
