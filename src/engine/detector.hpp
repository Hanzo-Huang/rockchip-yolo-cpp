#pragma once

#include "engine/frame.hpp"
#include "engine/json.hpp"
#include "engine/preprocess.hpp"
#include "engine/postprocess.hpp"

#include <memory>
#include <string>
#include <vector>
#include <rknn_api.h>

namespace nvr
{
// RKNN inference and detection postprocessing.
class Detector
{
    rknn_context context_ = 0;
    rknn_tensor_attr input_attr_{}, output_attr_{};
    rknn_tensor_mem *tensor_ = nullptr;
    std::unique_ptr<DmaBuffer> input_;
    std::unique_ptr<Preprocessor> preprocessor_;
    std::unique_ptr<YoloPostprocessor> postprocessor_;
    std::vector<float> output_;
    int width_ = 0, height_ = 0, stride_ = 0;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<std::vector<int8_t>> quant_outputs_;
    bool dfl_ = false;
    bool split_ = false, scores_first_ = true, boxes_first_ = true;
    int scores_index_ = -1, boxes_index_ = -1;
    int features_ = 0, proposals_ = 0;
    bool features_first_ = true, verify_, verified_ = false;
    std::string mode_;
    double last_run_ms_ = 0, last_output_ms_ = 0;

    void run_output(bool copy_input);

public:
    Json description;

    Detector(const std::string &model, const std::string &preprocess, bool verify, float confidence,
             float iou, rknn_core_mask core_mask = RKNN_NPU_CORE_ALL);
    ~Detector();

    Detector(const Detector &) = delete;
    Detector &operator=(const Detector &) = delete;

    Json infer(const Frame &frame);
};
} // namespace nvr
