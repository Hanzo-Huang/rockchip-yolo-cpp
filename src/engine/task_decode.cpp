#include "engine/task_model.hpp"
#include "engine/common.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace nvr
{
namespace
{
struct Candidate
{
    int class_id = -1, proposal = 0, branch = -1, cell = 0;
    float score = 0, x = 0, y = 0, width = 0, height = 0, angle = 0;
    std::vector<float> extra;
};

float overlap(const Candidate &a, const Candidate &b, bool rotated)
{
    if (rotated)
    {
        // Probabilistic IoU uses the covariance of each rotated rectangle.
        auto covariance = [](const Candidate &box)
        {
            float w = box.width * box.width / 12, h = box.height * box.height / 12;
            float c = std::cos(box.angle), s = std::sin(box.angle);
            return std::array<float, 3>{w * c * c + h * s * s, w * s * s + h * c * c,
                                        (w - h) * c * s};
        };
        auto x = covariance(a), y = covariance(b);
        float ax = x[0] + y[0], ay = x[1] + y[1], ac = x[2] + y[2];
        float dx = a.x - b.x, dy = a.y - b.y, denominator = ax * ay - ac * ac + 1e-7f;
        float distance = (ax * dy * dy + ay * dx * dx - 2 * ac * dx * dy) / (4 * denominator);
        float determinant =
            std::max(0.f, x[0] * x[1] - x[2] * x[2]) * std::max(0.f, y[0] * y[1] - y[2] * y[2]);
        distance += .5f * std::log(denominator / (4 * std::sqrt(determinant) + 1e-7f) + 1e-7f);
        return 1 - std::sqrt(std::max(0.f, 1 - std::exp(-std::clamp(distance, 1e-7f, 100.f))));
    }
    float intersection = std::max(0.f, std::min(a.x + a.width / 2, b.x + b.width / 2) -
                                           std::max(a.x - a.width / 2, b.x - b.width / 2)) *
                         std::max(0.f, std::min(a.y + a.height / 2, b.y + b.height / 2) -
                                           std::max(a.y - a.height / 2, b.y - b.height / 2));
    float total = a.width * a.height + b.width * b.height - intersection;
    return total > 0 ? intersection / total : 0;
}

float matrix_value(const TensorSpec &spec, const std::vector<float> &data, int channels,
                   int proposals, int feature, int proposal)
{
    const auto &d = spec.dimensions;
    bool major = d.size() == 4 ? spec.channels_first : d[1] == channels;
    return data[major ? size_t(feature) * proposals + proposal
                      : size_t(proposal) * channels + feature];
}
} // namespace

TaskResult TaskPostprocessor::decode(const Geometry &g,
                                     const std::vector<std::vector<float>> &outputs,
                                     float confidence, float nms_iou) const
{
    require(outputs.size() == specs_.size(), "Model output count changed");
    for (size_t i = 0; i < outputs.size(); ++i)
    {
        require(outputs[i].size() == specs_[i].elements(), "Model output size mismatch");
    }
    require(g.width > 0 && g.height > 0 && g.rw > 0 && g.rh > 0, "Invalid result geometry");
    require(confidence > 0 && confidence < 1 && nms_iou > 0 && nms_iou < 1,
            "Thresholds must be between zero and one");
    const float source_width = float(g.width), source_height = float(g.height);
    const float resize_width = float(g.rw), resize_height = float(g.rh);
    const float padding_x = float(g.px), padding_y = float(g.py);
    const float model_width = float(input_width_), model_height = float(input_height_);
    TaskResult result;
    auto source_point = [&](float x, float y)
    {
        return std::array<float, 2>{(x - padding_x) * source_width / resize_width,
                                    (y - padding_y) * source_height / resize_height};
    };
    if (layout_ == Layout::Classification)
    {
        auto probabilities = outputs[0];
        require(std::all_of(probabilities.begin(), probabilities.end(),
                            [](float x)
                            {
                                return std::isfinite(x);
                            }),
                "Non-finite classification output");
        if (std::any_of(probabilities.begin(), probabilities.end(),
                        [](float x)
                        {
                            return x < 0 || x > 1;
                        }))
        {
            float maximum = *std::max_element(probabilities.begin(), probabilities.end()), sum = 0;
            for (float &value : probabilities)
            {
                value = std::exp(value - maximum);
                sum += value;
            }
            for (float &value : probabilities)
            {
                value /= sum;
            }
        }
        std::vector<int> order(probabilities.size());
        std::iota(order.begin(), order.end(), 0);
        size_t count = std::min(size_t(5), order.size());
        std::partial_sort(order.begin(), order.begin() + count, order.end(),
                          [&](int a, int b)
                          {
                              return probabilities[size_t(a)] > probabilities[size_t(b)];
                          });
        for (size_t i = 0; i < count; ++i)
        {
            result.classification.push_back({order[i], probabilities[size_t(order[i])]});
        }
        return result;
    }
    if (layout_ == Layout::Dense)
    {
        return decode_dense(g, {outputs[0].data(), outputs[0].size()});
    }

    // Generate candidates in model coordinates; task attachments retain proposal identity.
    const int extra_count = options_.task == ModelTask::Segment ? mask_channels_
                            : options_.task == ModelTask::Pose
                                ? options_.keypoints * options_.keypoint_dimensions
                            : options_.task == ModelTask::Obb ? 1
                                                              : 0;
    std::vector<Candidate> candidates;
    auto accept = [&](Candidate candidate, bool xyxy)
    {
        if (!std::isfinite(candidate.score) || !(candidate.score >= confidence) ||
            candidate.class_id < 0 || candidate.class_id >= options_.classes)
        {
            return;
        }
        if (options_.normalized && layout_ != Layout::Dfl && layout_ != Layout::Objectness)
        {
            candidate.x *= model_width;
            candidate.width *= model_width;
            candidate.y *= model_height;
            candidate.height *= model_height;
        }
        if (xyxy)
        {
            candidate.width -= candidate.x;
            candidate.height -= candidate.y;
            candidate.x += candidate.width / 2;
            candidate.y += candidate.height / 2;
        }
        if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
            !std::isfinite(candidate.width) || !std::isfinite(candidate.height) ||
            candidate.width <= 0 || candidate.height <= 0)
        {
            return;
        }
        if (options_.task == ModelTask::Obb)
        {
            candidate.angle = candidate.extra[0];
            if (!std::isfinite(candidate.angle))
            {
                return;
            }
        }
        candidates.push_back(std::move(candidate));
    };
    if (layout_ == Layout::Decoded || layout_ == Layout::EndToEnd)
    {
        const auto &data = outputs[size_t(prediction_)];
        auto value = [&](int p, int f)
        {
            return data[feature_major_ ? size_t(f) * proposals_ + p : size_t(p) * features_ + f];
        };
        for (int p = 0; p < proposals_; ++p)
        {
            Candidate c;
            c.proposal = p;
            if (layout_ == Layout::EndToEnd)
            {
                float label = value(p, 5);
                if (!std::isfinite(label) || label != std::round(label) || label < 0 ||
                    label >= float(options_.classes))
                {
                    continue;
                }
                c.class_id = int(label);
                c.score = value(p, 4);
            }
            else
            {
                for (int cls = 0; cls < options_.classes; ++cls)
                {
                    float score = value(p, 4 + cls);
                    if (std::isfinite(score) && score > c.score)
                    {
                        c.score = score;
                        c.class_id = cls;
                    }
                }
            }
            if (c.score < confidence)
            {
                continue;
            }
            c.x = value(p, 0);
            c.y = value(p, 1);
            c.width = value(p, 2);
            c.height = value(p, 3);
            int base = layout_ == Layout::EndToEnd ? 6 : 4 + options_.classes;
            for (int j = 0; j < extra_count; ++j)
            {
                c.extra.push_back(value(p, base + j));
            }
            accept(std::move(c), layout_ == Layout::EndToEnd && options_.task != ModelTask::Obb);
        }
    }
    else if (layout_ == Layout::Split)
    {
        for (int p = 0; p < proposals_; ++p)
        {
            Candidate c;
            c.proposal = p;
            for (int cls = 0; cls < options_.classes; ++cls)
            {
                float score = matrix_value(specs_[size_t(scores_)], outputs[size_t(scores_)],
                                           options_.classes, proposals_, cls, p);
                if (std::isfinite(score) && score > c.score)
                {
                    c.score = score;
                    c.class_id = cls;
                }
            }
            if (c.score < confidence)
            {
                continue;
            }
            auto value = [&](int f)
            {
                return matrix_value(specs_[size_t(boxes_)], outputs[size_t(boxes_)], 4, proposals_,
                                    f, p);
            };
            c.x = value(0);
            c.y = value(1);
            c.width = value(2);
            c.height = value(3);
            for (int j = 0; j < extra_count; ++j)
            {
                c.extra.push_back(matrix_value(specs_[size_t(auxiliary_)],
                                               outputs[size_t(auxiliary_)], extra_count, proposals_,
                                               j, p));
            }
            accept(std::move(c), options_.task != ModelTask::Obb);
        }
    }
    else if (layout_ == Layout::Objectness)
    {
        const bool anchored = options_.spatial_head == SpatialHead::Anchored;
        const int anchor_count = anchored ? 3 : 1;
        for (size_t branch = 0; branch < regressions_.size(); ++branch)
        {
            const size_t index = size_t(regressions_[branch]);
            const auto &grid = specs_[index].dimensions;
            const int width = grid[3], cells = width * grid[2];
            const float stride = float(input_width_ / width);
            const auto &output = outputs[index];
            for (int anchor = 0; anchor < anchor_count; ++anchor)
            {
                const int base = anchor * (5 + options_.classes);
                for (int cell = 0; cell < cells; ++cell)
                {
                    auto value = [&](int channel)
                    {
                        return output[size_t(base + channel) * cells + cell];
                    };
                    const float objectness = value(4);
                    if (!std::isfinite(objectness) || objectness < confidence)
                    {
                        continue;
                    }
                    Candidate candidate;
                    for (int category = 0; category < options_.classes; ++category)
                    {
                        const float score = objectness * value(5 + category);
                        if (std::isfinite(score) && score > candidate.score)
                        {
                            candidate.score = score;
                            candidate.class_id = category;
                        }
                    }
                    if (candidate.score < confidence)
                    {
                        continue;
                    }
                    const float column = float(cell % width), row = float(cell / width);
                    if (anchored)
                    {
                        const auto &anchors = options_.anchors[branch];
                        const float w = 2 * value(2), h = 2 * value(3);
                        candidate.x = (2 * value(0) - .5f + column) * stride;
                        candidate.y = (2 * value(1) - .5f + row) * stride;
                        candidate.width = w * w * anchors[size_t(2 * anchor)];
                        candidate.height = h * h * anchors[size_t(2 * anchor + 1)];
                    }
                    else
                    {
                        candidate.x = (value(0) + column) * stride;
                        candidate.y = (value(1) + row) * stride;
                        candidate.width = std::exp(value(2)) * stride;
                        candidate.height = std::exp(value(3)) * stride;
                    }
                    if (extra_count)
                    {
                        const auto &masks = outputs[size_t(extras_[branch])];
                        for (int coefficient = 0; coefficient < extra_count; ++coefficient)
                        {
                            candidate.extra.push_back(
                                masks[size_t(anchor * extra_count + coefficient) * cells + cell]);
                        }
                    }
                    accept(std::move(candidate), false);
                }
            }
        }
    }
    else
    {
        int offset = 0;
        for (size_t branch = 0; branch < regressions_.size(); ++branch)
        {
            int reg = regressions_[branch], cls = class_scores_[branch];
            const auto &d = specs_[size_t(reg)].dimensions;
            int width = d[3], height = d[2], cells = width * height;
            // Rockchip pose and OBB pack raw class logits after the DFL bins.
            const bool packed_logits = reg == cls;
            const float score_floor =
                packed_logits ? std::log(confidence / (1 - confidence)) : confidence;
            int channels = d[1] - (packed_logits ? options_.classes : 0), bins = channels / 4,
                stride = input_width_ / width;
            for (int p = 0; p < cells; ++p)
            {
                Candidate c;
                c.proposal = offset + p;
                c.branch = int(branch);
                c.cell = p;
                c.score = -std::numeric_limits<float>::infinity();
                for (int category = 0; category < options_.classes; ++category)
                {
                    float score =
                        outputs[size_t(cls)]
                               [size_t(category + (packed_logits ? channels : 0)) * cells + p];
                    if (std::isfinite(score) && score > c.score)
                    {
                        c.score = score;
                        c.class_id = category;
                    }
                }
                if (c.score < score_floor)
                {
                    continue;
                }
                if (packed_logits)
                {
                    c.score = 1 / (1 + std::exp(-c.score));
                }
                float distances[4]{};
                for (int side = 0; side < 4; ++side)
                {
                    if (bins == 1)
                    {
                        distances[side] =
                            outputs[size_t(reg)][size_t(side) * cells + p] * float(stride);
                    }
                    else
                    {
                        float maximum = -std::numeric_limits<float>::infinity(), sum = 0,
                              weighted = 0;
                        for (int bin = 0; bin < bins; ++bin)
                        {
                            maximum = std::max(
                                maximum,
                                outputs[size_t(reg)][size_t(side * bins + bin) * cells + p]);
                        }
                        for (int bin = 0; bin < bins; ++bin)
                        {
                            float weight = std::exp(
                                outputs[size_t(reg)][size_t(side * bins + bin) * cells + p] -
                                maximum);
                            sum += weight;
                            weighted += weight * float(bin);
                        }
                        distances[side] = weighted / sum * float(stride);
                    }
                }
                for (int j = 0; j < extra_count; ++j)
                {
                    int index = extras_.empty() ? auxiliary_ : extras_[branch];
                    c.extra.push_back(matrix_value(
                        specs_[size_t(index)], outputs[size_t(index)], extra_count,
                        extras_.empty() ? proposals_ : cells, j, extras_.empty() ? offset + p : p));
                }
                float x = (distances[2] - distances[0]) / 2, y = (distances[3] - distances[1]) / 2;
                if (options_.task == ModelTask::Obb)
                {
                    // Model-zoo OBB exposes the angle sigmoid before its affine transform.
                    c.extra[0] = (c.extra[0] - .25f) * 3.1415927410125732f;
                    float cosine = std::cos(c.extra[0]), sine = std::sin(c.extra[0]);
                    float rotated_x = x * cosine - y * sine;
                    y = x * sine + y * cosine;
                    x = rotated_x;
                }
                c.x = (float(p % width) + .5f) * float(stride) + x;
                c.y = (float(p / width) + .5f) * float(stride) + y;
                c.width = distances[0] + distances[2];
                c.height = distances[1] + distances[3];
                // Raw spatial heads are already in pixels, independent of decoded INT8
                // normalization.
                accept(std::move(c), false);
            }
            offset += cells;
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b)
                     {
                         return a.score > b.score;
                     });
    if (candidates.size() > 1000)
    {
        candidates.resize(1000);
    }
    std::vector<Candidate> kept;
    const bool nms = !options_.end_to_end && layout_ != Layout::EndToEnd && layout_ != Layout::Split;
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        const auto &candidate = candidates[index];
        bool suppressed = false;
        if (nms && options_.task == ModelTask::Obb)
        {
            // Ultralytics rotated Fast-NMS compares every higher-scoring proposal,
            // including proposals that were themselves suppressed.
            for (size_t previous = 0; previous < index; ++previous)
            {
                if (candidate.class_id == candidates[previous].class_id &&
                    overlap(candidate, candidates[previous], true) > nms_iou)
                {
                    suppressed = true;
                    break;
                }
            }
        }
        else if (nms)
        {
            for (const auto &previous : kept)
            {
                if (candidate.class_id == previous.class_id &&
                    overlap(candidate, previous, false) > nms_iou)
                {
                    suppressed = true;
                    break;
                }
            }
        }
        if (!suppressed)
        {
            kept.push_back(candidate);
        }
        if (kept.size() == 300)
        {
            break;
        }
    }
    for (const auto &candidate : kept)
    {
        TaskDetection item;
        item.detection.class_id = candidate.class_id;
        item.detection.confidence = candidate.score;
        auto a =
            source_point(candidate.x - candidate.width / 2, candidate.y - candidate.height / 2);
        auto b =
            source_point(candidate.x + candidate.width / 2, candidate.y + candidate.height / 2);
        if (options_.task == ModelTask::Obb)
        {
            float cosine = std::cos(candidate.angle), sine = std::sin(candidate.angle);
            a = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
            b = {-a[0], -a[1]};
            for (auto corner :
                 std::array<std::array<float, 2>, 4>{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}})
            {
                float x = corner[0] * candidate.width / 2, y = corner[1] * candidate.height / 2;
                auto point = source_point(candidate.x + x * cosine - y * sine,
                                          candidate.y + x * sine + y * cosine);
                item.polygon.push_back(point);
                a[0] = std::min(a[0], point[0]);
                a[1] = std::min(a[1], point[1]);
                b[0] = std::max(b[0], point[0]);
                b[1] = std::max(b[1], point[1]);
            }
        }
        item.detection.box = {std::clamp(a[0], 0.f, float(source_width)),
                              std::clamp(a[1], 0.f, float(source_height)),
                              std::clamp(b[0], 0.f, float(source_width)),
                              std::clamp(b[1], 0.f, float(source_height))};
        if (item.detection.box[2] <= item.detection.box[0] ||
            item.detection.box[3] <= item.detection.box[1])
        {
            continue;
        }
        if (options_.task == ModelTask::Pose)
        {
            for (int k = 0; k < options_.keypoints; ++k)
            {
                size_t index = size_t(k * options_.keypoint_dimensions);
                float x = candidate.extra[index], y = candidate.extra[index + 1];
                if (options_.normalized && layout_ != Layout::Dfl)
                {
                    x *= model_width;
                    y *= model_height;
                }
                float visibility =
                    options_.keypoint_dimensions == 3 ? candidate.extra[index + 2] : 1;
                auto point = source_point(x, y);
                if (!std::isfinite(point[0]) || !std::isfinite(point[1]) ||
                    !std::isfinite(visibility))
                {
                    visibility = 0;
                }
                item.keypoints.push_back({std::isfinite(point[0]) ? point[0] : 0,
                                          std::isfinite(point[1]) ? point[1] : 0,
                                          std::clamp(visibility, 0.f, 1.f)});
            }
        }
        if (options_.task == ModelTask::Segment)
        {
            const auto &spec = specs_[size_t(prototypes_)];
            const auto &d = spec.dimensions;
            int height = d[spec.channels_first ? 2 : 1], width = d[spec.channels_first ? 3 : 2];
            ResultRaster mask;
            mask.box = item.detection.box;
            const float left = mask.box[0] * resize_width / source_width + padding_x;
            const float top = mask.box[1] * resize_height / source_height + padding_y;
            const float box_width = (mask.box[2] - mask.box[0]) * resize_width / source_width;
            const float box_height = (mask.box[3] - mask.box[1]) * resize_height / source_height;
            int limit =
                std::max(1, int(std::sqrt(32768.0 / double(std::max(size_t(1), kept.size())))));
            mask.width = std::clamp(int(std::ceil(box_width)), 1, limit);
            mask.height = std::clamp(int(std::ceil(box_height)), 1, limit);

            const auto &prototypes = outputs[size_t(prototypes_)];
            auto dot = [&](int px, int py)
            {
                const size_t pixel = size_t(py) * width + px;
                float logit = 0;
                for (int c = 0; c < mask_channels_; ++c)
                {
                    size_t index = spec.channels_first ? size_t(c) * width * height + pixel
                                                       : pixel * mask_channels_ + c;
                    logit += candidate.extra[size_t(c)] * prototypes[index];
                }
                return logit;
            };
            auto proto_x = [&](float x)
            {
                return std::clamp(x * float(width) / model_width - .5f, 0.f, float(width - 1));
            };
            auto proto_y = [&](float y)
            {
                return std::clamp(y * float(height) / model_height - .5f, 0.f, float(height - 1));
            };
            const int x0 = int(proto_x(left)), y0 = int(proto_y(top));
            const int cols = int(std::ceil(proto_x(left + box_width))) - x0 + 1;
            const int rows = int(std::ceil(proto_y(top + box_height))) - y0 + 1;
            std::vector<float> logits;
            // Reuse the cropped prototype logits when the preview samples them densely.
            if (size_t(cols) * rows <= size_t(mask.width) * mask.height)
            {
                logits.reserve(size_t(cols) * rows);
                for (int py = y0; py < y0 + rows; ++py)
                {
                    for (int px = x0; px < x0 + cols; ++px)
                    {
                        logits.push_back(dot(px, py));
                    }
                }
            }
            auto logit_at = [&](int px, int py)
            {
                return logits.empty() ? dot(px, py) : logits[size_t(py - y0) * cols + px - x0];
            };
            const float step_x = box_width / float(mask.width);
            const float step_y = box_height / float(mask.height);
            // Interpolate the signed logits before thresholding so contours can cross
            // between prototype cells. The raster still covers only the detection box.
            for (int y = 0; y < mask.height; ++y)
            {
                const float fy = proto_y(top + (float(y) + .5f) * step_y);
                const int py = int(fy), next_y = std::min(py + 1, y0 + rows - 1);
                const float dy = fy - float(py);
                for (int x = 0; x < mask.width; ++x)
                {
                    const float fx = proto_x(left + (float(x) + .5f) * step_x);
                    const int px = int(fx), next_x = std::min(px + 1, x0 + cols - 1);
                    const float dx = fx - float(px);
                    const float a = logit_at(px, py), b = logit_at(next_x, py);
                    const float c = logit_at(px, next_y), d = logit_at(next_x, next_y);
                    const float upper = a + (b - a) * dx, lower = c + (d - c) * dx;
                    const float logit = upper + (lower - upper) * dy;
                    append_run(mask, std::isfinite(logit) && logit > 0 ? 1 : 0);
                }
            }
            item.mask = std::move(mask);
        }
        result.detections.push_back(std::move(item));
    }
    return result;
}
} // namespace nvr
