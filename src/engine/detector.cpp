#include "engine/pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace nvr
{
Detector::Detector(const std::string &model, const std::string &mode, bool verify, float threshold,
                   float iou)
    : verify_(verify), threshold_(threshold), iou_(iou), mode_(mode)
{
    try
    {
        std::ifstream file(model, std::ios::binary);
        require(bool(file), "open RKNN model");
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
        require(!bytes.empty(), "empty RKNN model");
        require(rknn_init(&context_, bytes.data(), static_cast<uint32_t>(bytes.size()), 0,
                          nullptr) == RKNN_SUCC,
                "initialize RKNN model");
        require(rknn_set_core_mask(context_, RKNN_NPU_CORE_ALL) == 0, "enable available NPU cores");

        // Validate the model input contract before allocating hardware buffers.
        rknn_input_output_num io{};
        require(rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) == 0,
                "query RKNN I/O");
        require(io.n_input == 1 && (io.n_output == 1 || io.n_output == 2 || io.n_output == 9),
                "use decoded YOLO, two-output YOLO26, or nine-output INT8 DFL export");
        require(rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attr_, sizeof(input_attr_)) == 0,
                "query RKNN input");
        require(rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &output_attr_, sizeof(output_attr_)) ==
                    0,
                "query RKNN output");
        require(input_attr_.n_dims == 4 && input_attr_.dims[0] == 1,
                "model input must be batch-1 image");
        bool nhwc = input_attr_.fmt == RKNN_TENSOR_NHWC;
        height_ = static_cast<int>(input_attr_.dims[nhwc ? 1 : 2]);
        width_ = static_cast<int>(input_attr_.dims[nhwc ? 2 : 3]);
        require(input_attr_.dims[nhwc ? 3 : 1] == 3 && width_ % 16 == 0 && height_ % 2 == 0,
                "input needs RGB, width aligned to 16 and even height");

        // Select the output decoder and allocate its reusable result storage.
        dfl_ = io.n_output == 9;
        split_ = io.n_output == 2;
        if (dfl_)
        {
            output_attrs_.resize(9);
            quant_outputs_.resize(9);
            proposals_ = 0;
            for (uint32_t i = 0; i < 9; ++i)
            {
                auto &a = output_attrs_[i];
                a.index = i;
                require(rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a)) == 0,
                        "query DFL output");
                require(a.n_dims == 4 && a.dims[0] == 1 && a.fmt == RKNN_TENSOR_NCHW &&
                            a.type == RKNN_TENSOR_INT8 &&
                            a.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && a.scale > 0,
                        "DFL requires affine INT8 NCHW outputs");
                quant_outputs_[i].resize(a.n_elems);
                if (i % 3 == 0)
                {
                    require(a.dims[1] == 64 && a.dims[2] > 0 && a.dims[3] > 0,
                            "DFL needs 16 bins per side");
                    require(width_ % int(a.dims[3]) == 0 && height_ % int(a.dims[2]) == 0 &&
                                width_ / int(a.dims[3]) == height_ / int(a.dims[2]),
                            "DFL stride mismatch");
                    proposals_ += int(a.dims[2] * a.dims[3]);
                }
                else
                {
                    const auto &b = output_attrs_[i - i % 3];
                    require(a.dims[2] == b.dims[2] && a.dims[3] == b.dims[3], "DFL grid mismatch");
                    if (i % 3 == 1)
                    {
                        if (i == 1)
                        {
                            features_ = int(a.dims[1]) + 4;
                        }
                        require(a.dims[1] == uint32_t(features_ - 4) && features_ > 4 &&
                                    features_ <= 1024,
                                "DFL class mismatch");
                    }
                    else
                    {
                        require(a.dims[1] == 1, "DFL confidence sum must have one channel");
                    }
                }
            }
        }
        else if (split_)
        {
            output_attrs_.resize(2);
            for (uint32_t i = 0; i < 2; ++i)
            {
                auto &a = output_attrs_[i];
                a.index = i;
                require(rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a)) == 0,
                        "query YOLO26 output");
                require(a.n_dims == 3 && a.dims[0] == 1 && a.dims[1] > 0 && a.dims[2] > 0,
                        "YOLO26 outputs need batch-1 rank-3 tensors");
                if (a.dims[1] == 4 || a.dims[2] == 4)
                {
                    if (boxes_index_ < 0 || std::string(a.name).find("Mul_2") != std::string::npos)
                    {
                        boxes_index_ = int(i);
                    }
                }
            }
            require(boxes_index_ >= 0, "YOLO26 requires a four-coordinate boxes output");
            scores_index_ = 1 - boxes_index_;
            const auto &b = output_attrs_[size_t(boxes_index_)];
            const auto &s = output_attrs_[size_t(scores_index_)];
            boxes_first_ = b.dims[1] == 4;
            proposals_ = int(b.dims[boxes_first_ ? 2 : 1]);
            scores_first_ = s.dims[2] == uint32_t(proposals_);
            require(s.dims[scores_first_ ? 2 : 1] == uint32_t(proposals_),
                    "YOLO26 score/box proposal count mismatch");
            features_ = int(s.dims[scores_first_ ? 1 : 2]) + 4;
            require(features_ > 4 && features_ <= 1024 && proposals_ > 0,
                    "unsupported YOLO26 class count");
            require(s.n_elems == uint32_t(features_ - 4) * uint32_t(proposals_) &&
                        b.n_elems == 4 * uint32_t(proposals_),
                    "YOLO26 tensor size mismatch");
            output_.resize(size_t(features_) * size_t(proposals_));
        }
        else
        {
            require(output_attr_.n_dims == 3 && output_attr_.dims[0] == 1,
                    "unsupported YOLO output rank/batch");
            features_first_ = output_attr_.dims[1] < output_attr_.dims[2];
            features_ = static_cast<int>(output_attr_.dims[features_first_ ? 1 : 2]);
            proposals_ = static_cast<int>(output_attr_.dims[features_first_ ? 2 : 1]);
            require(features_ > 4 && features_ <= 1024 && proposals_ > features_,
                    "unsupported YOLO output layout");
        }

        // Share one RGB DMA buffer between RGA preprocessing and RKNN input.
        stride_ = input_attr_.w_stride ? static_cast<int>(input_attr_.w_stride) : width_;
        require(stride_ >= width_ && stride_ % 16 == 0, "unsupported RKNN width stride");
        input_ = std::make_unique<DmaBuffer>(
            std::max<size_t>(input_attr_.size_with_stride, size_t(stride_) * height_ * 3));
        tensor_ = rknn_create_mem_from_fd(context_, input_->fd, input_->data,
                                          static_cast<uint32_t>(input_->size), 0);
        require(tensor_, "import RGA destination DMA-BUF into RKNN");
        input_attr_.type = RKNN_TENSOR_UINT8;
        input_attr_.fmt = RKNN_TENSOR_NHWC;
        input_attr_.pass_through = 0;
        input_attr_.h_stride = static_cast<uint32_t>(height_);
        require(rknn_set_io_mem(context_, tensor_, &input_attr_) == 0,
                "bind external RGB input to RKNN");
        if (!dfl_ && !split_)
        {
            output_.resize(output_attr_.n_elems);
        }
        preprocessor_ = std::make_unique<Preprocessor>(*input_, width_, height_, stride_, mode_);
        rknn_sdk_version version{};
        require(rknn_query(context_, RKNN_QUERY_SDK_VERSION, &version, sizeof(version)) == 0,
                "query RKNN version");
        description = {{"runtime", version.api_version},
                       {"driver", version.drv_version},
                       {"input_width", width_},
                       {"input_height", height_},
                       {"input_stride", stride_},
                       {"classes", features_ - 4},
                       {"proposals", proposals_},
                       {"preprocess", mode_},
                       {"output_type", get_type_string(output_attr_.type)},
                       {"postprocessor", dfl_     ? "INT8 DFL (9 outputs)"
                                         : split_ ? "YOLO26 scores/xyxy (2 outputs; no NMS)"
                                                  : "decoded YOLO (1 output)"},
                       {"core_mask", "all"},
                       {"input_api", "rknn_create_mem_from_fd + rknn_set_io_mem"}};
    }
    catch (...)
    {
        preprocessor_.reset();
        if (tensor_)
        {
            rknn_destroy_mem(context_, tensor_);
        }
        tensor_ = nullptr;
        input_.reset();
        if (context_)
        {
            rknn_destroy(context_);
        }
        context_ = 0;
        throw;
    }
}

Detector::~Detector()
{
    preprocessor_.reset();
    if (tensor_)
    {
        rknn_destroy_mem(context_, tensor_);
    }
    input_.reset();
    if (context_)
    {
        rknn_destroy(context_);
    }
}

void Detector::run_output(bool copy_input)
{
    std::vector<uint8_t> packed;
    if (copy_input)
    {
        packed.resize(size_t(width_) * height_ * 3);
        for (int row = 0; row < height_; ++row)
        {
            std::memcpy(packed.data() + size_t(row) * width_ * 3,
                        static_cast<uint8_t *>(input_->data) + size_t(row) * stride_ * 3,
                        size_t(width_) * 3);
        }
        rknn_input in{};
        in.buf = packed.data();
        in.size = static_cast<uint32_t>(packed.size());
        in.type = RKNN_TENSOR_UINT8;
        in.fmt = RKNN_TENSOR_NHWC;
        require(rknn_inputs_set(context_, 1, &in) == 0, "set reference RKNN input");
    }
    auto started = Clock::now();
    require(rknn_run(context_, nullptr) == 0, "RKNN inference");
    last_run_ms_ = milliseconds(started);
    started = Clock::now();
    if (dfl_)
    {
        std::vector<rknn_output> outputs(9);
        for (uint32_t i = 0; i < 9; ++i)
        {
            auto &out = outputs[i];
            out.index = i;
            out.want_float = 0;
            out.is_prealloc = 1;
            out.buf = quant_outputs_[i].data();
            out.size = uint32_t(quant_outputs_[i].size());
        }
        require(rknn_outputs_get(context_, 9, outputs.data(), nullptr) == 0,
                "get quantized RKNN outputs");
        require(rknn_outputs_release(context_, 9, outputs.data()) == 0,
                "release quantized RKNN outputs");
    }
    else if (split_)
    {
        rknn_output outputs[2]{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            auto &out = outputs[i];
            out.index = i;
            out.want_float = 1;
            out.is_prealloc = 1;
            out.buf = output_.data() +
                      (int(i) == scores_index_ ? 0 : size_t(features_ - 4) * size_t(proposals_));
            out.size = static_cast<uint32_t>(output_attrs_[i].n_elems * sizeof(float));
        }
        require(rknn_outputs_get(context_, 2, outputs, nullptr) == 0,
                "get YOLO26 scores and boxes");
        require(rknn_outputs_release(context_, 2, outputs) == 0, "release YOLO26 outputs");
    }
    else
    {
        rknn_output out{};
        out.want_float = 1;
        out.is_prealloc = 1;
        out.buf = output_.data();
        out.size = static_cast<uint32_t>(output_.size() * sizeof(float));
        require(rknn_outputs_get(context_, 1, &out, nullptr) == 0, "get RKNN output");
        require(rknn_outputs_release(context_, 1, &out) == 0, "release RKNN output");
    }
    last_output_ms_ = milliseconds(started);
}

Json Detector::infer(const Frame &f)
{
    auto pre = Clock::now();
    auto geometry = preprocessor_->process(f);
    double pre_ms = milliseconds(pre);
    auto start = Clock::now();
    run_output(false);
    double infer_ms = milliseconds(start);
    double run_ms = last_run_ms_, output_ms = last_output_ms_;
    Json verification = nullptr;
    if (verify_ && !verified_)
    {
        std::vector<float> external = output_;
        auto quant_external = quant_outputs_;
        run_output(true);
        float max_error = 0;
        if (dfl_)
        {
            require(quant_external == quant_outputs_,
                    "external DMA quantized outputs disagree with reference input");
        }
        else
        {
            for (size_t i = 0; i < external.size(); ++i)
            {
                require(std::isfinite(external[i]) && std::isfinite(output_[i]),
                        "nonfinite model output");
                max_error = std::max(max_error, std::abs(external[i] - output_[i]));
            }
        }
        require(rknn_set_io_mem(context_, tensor_, &input_attr_) == 0,
                "rebind external input after verification");
        require(max_error < 0.01f, "external DMA input disagrees with rknn_inputs_set: max error " +
                                       std::to_string(max_error));
        output_ = std::move(external);
        quant_outputs_ = std::move(quant_external);
        verified_ = true;
        verification = {{"passed", true}, {"max_abs_error", max_error}};
    }
    auto post = Clock::now();
    Json detections = decode(geometry);
    double post_ms = milliseconds(post);
    return Json{{"frame", f.sequence},
                {"pts_us", f.pts_us},
                {"source_width", geometry.width},
                {"source_height", geometry.height},
                {"preprocess_ms", pre_ms},
                {"inference_ms", infer_ms},
                {"rknn_run_ms", run_ms},
                {"output_get_ms", output_ms},
                {"postprocess_ms", post_ms},
                {"age_ms", milliseconds(f.decoded_at)},
                {"io_verification", verification},
                {"detections", detections}};
}

Json Detector::decode(const Geometry &g)
{
    struct Box
    {
        float x, y, w, h, score;
        int cls;
    };

    std::vector<Box> candidates;
    auto value = [&](int p, int f)
    {
        return output_[features_first_ ? size_t(f) * proposals_ + p : size_t(p) * features_ + f];
    };
    if (dfl_)
    {
        for (int branch = 0; branch < 3; ++branch)
        {
            const auto &reg = output_attrs_[size_t(branch) * 3];
            const auto &scores = output_attrs_[size_t(branch) * 3 + 1];
            const auto &r = quant_outputs_[size_t(branch) * 3];
            const auto &s = quant_outputs_[size_t(branch) * 3 + 1];
            const auto &sums = output_attrs_[size_t(branch) * 3 + 2];
            const auto &sums_data = quant_outputs_[size_t(branch) * 3 + 2];
            int gh = int(reg.dims[2]), gw = int(reg.dims[3]), cells = gh * gw, stride = width_ / gw;
            for (int p = 0; p < cells; ++p)
            {
                // The model-zoo sum head bounds the maximum class probability. Leave
                // one quantization step of margin to avoid filtering borderline hits.
                if (float(int(sums_data[size_t(p)]) - sums.zp) * sums.scale + sums.scale <
                    threshold_)
                {
                    continue;
                }
                int best = -128, cls = -1;
                for (int c = 0; c < features_ - 4; ++c)
                {
                    int score = s[size_t(c) * cells + p];
                    if (score > best)
                    {
                        best = score;
                        cls = c;
                    }
                }
                float probability = float(best - scores.zp) * scores.scale;
                if (cls < 0 || probability < threshold_)
                {
                    continue;
                }
                float distances[4]{};
                for (int side = 0; side < 4; ++side)
                {
                    float logits[16], max_logit = -1e30f;
                    for (int bin = 0; bin < 16; ++bin)
                    {
                        float x =
                            float(int(r[size_t(side * 16 + bin) * cells + p]) - reg.zp) * reg.scale;
                        logits[bin] = x;
                        max_logit = std::max(max_logit, x);
                    }
                    float total = 0, weighted = 0;
                    for (int bin = 0; bin < 16; ++bin)
                    {
                        float weight = std::exp(logits[bin] - max_logit);
                        total += weight;
                        weighted += float(bin) * weight;
                    }
                    distances[side] = weighted / total * float(stride);
                }
                float cx = (float(p % gw) + 0.5f) * float(stride),
                      cy = (float(p / gw) + 0.5f) * float(stride);
                candidates.push_back({cx - distances[0], cy - distances[1],
                                      distances[0] + distances[2], distances[1] + distances[3],
                                      probability, cls});
            }
        }
    }
    else if (split_)
    {
        // Rockchip's truncated one2one head already supplies sigmoid probabilities
        // and pixel-space xyxy boxes. Select TopK; do not apply a second NMS.
        const auto *boxes = output_.data() + size_t(features_ - 4) * size_t(proposals_);
        for (int p = 0; p < proposals_; ++p)
        {
            float best = threshold_;
            int cls = -1;
            for (int c = 0; c < features_ - 4; ++c)
            {
                float score = output_[scores_first_ ? size_t(c) * proposals_ + p
                                                    : size_t(p) * (features_ - 4) + c];
                if (std::isfinite(score) && score > best)
                {
                    best = score;
                    cls = c;
                }
            }
            if (cls < 0)
            {
                continue;
            }
            auto corner = [&](int c)
            {
                return boxes[boxes_first_ ? size_t(c) * proposals_ + p : size_t(p) * 4 + c];
            };
            float x = corner(0), y = corner(1), w = corner(2) - x, h = corner(3) - y;
            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h) &&
                w > 0 && h > 0)
            {
                candidates.push_back({x, y, w, h, best, cls});
            }
        }
    }
    else
    {
        for (int p = 0; p < proposals_; ++p)
        {
            float best = threshold_;
            int cls = -1;
            for (int c = 4; c < features_; ++c)
            {
                float score = value(p, c);
                if (std::isfinite(score) && score > best)
                {
                    best = score;
                    cls = c - 4;
                }
            }
            if (cls < 0)
            {
                continue;
            }
            float w = value(p, 2), h = value(p, 3), x = value(p, 0) - w / 2,
                  y = value(p, 1) - h / 2;
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) ||
                w <= 0 || h <= 0)
            {
                continue;
            }
            candidates.push_back({x, y, w, h, best, cls});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Box &a, const Box &b)
              {
                  return a.score > b.score;
              });
    if (candidates.size() > 1000)
    {
        candidates.resize(1000);
    }
    std::vector<Box> kept;
    Json result = Json::array();
    for (const auto &a : candidates)
    {
        bool suppressed = false;
        if (!split_)
        {
            for (const auto &b : kept)
            {
                if (a.cls != b.cls)
                {
                    continue;
                }
                float iw = std::max(0.f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
                float ih = std::max(0.f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
                float inter = iw * ih, uni = a.w * a.h + b.w * b.h - inter;
                if (uni > 0 && inter / uni > iou_)
                {
                    suppressed = true;
                    break;
                }
            }
        }
        if (suppressed)
        {
            continue;
        }
        kept.push_back(a);
        // Use the actual even resized dimensions to undo the RGA letterbox.
        float sx = float(g.width) / float(g.rw), sy = float(g.height) / float(g.rh);
        float x1 = std::clamp((a.x - float(g.px)) * sx, 0.f, float(g.width));
        float y1 = std::clamp((a.y - float(g.py)) * sy, 0.f, float(g.height));
        float x2 = std::clamp((a.x + a.w - float(g.px)) * sx, 0.f, float(g.width));
        float y2 = std::clamp((a.y + a.h - float(g.py)) * sy, 0.f, float(g.height));
        if (x2 > x1 && y2 > y1)
        {
            result.push_back(
                {{"class_id", a.cls}, {"confidence", a.score}, {"box", {x1, y1, x2, y2}}});
        }
        if (kept.size() >= 300)
        {
            break;
        }
    }
    return result;
}
} // namespace nvr
