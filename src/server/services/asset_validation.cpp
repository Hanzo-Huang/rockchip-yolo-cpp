#include "server/services/warehouse.hpp"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <cmath>

namespace nvr::web
{
Json inspect_video(const fs::path &file)
{
    auto probe = parse_program_json(
        capture({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_streams",
                 "-show_format", "-of", "json", file.string()}));
    check(probe.contains("streams") && !probe["streams"].empty(), "MP4 contains no video stream",
          422);
    const auto &stream = probe["streams"][0];
    const auto &format = probe["format"];
    check(format.value("format_name", std::string()).find("mp4") != std::string::npos,
          "File is not an MP4 container", 422);
    std::string codec = stream.value("codec_name", std::string()),
                pixel = stream.value("pix_fmt", std::string());
    bool compatible =
        (codec == "h264" || codec == "hevc") && (pixel == "yuv420p" || pixel == "yuvj420p");
    return {{"codec", codec},
            {"pixel_format", pixel},
            {"width", stream.value("width", 0)},
            {"height", stream.value("height", 0)},
            {"duration", std::stod(format.value("duration", std::string("0")))},
            {"size_bytes", fs::file_size(file)},
            {"frame_rate", stream.value("avg_frame_rate", std::string())},
            {"compatible", compatible},
            {"reason", compatible ? "8-bit H.264/H.265 input"
                                  : "Engine accepts 8-bit H.264/H.265 video only"}};
}

Json inspect_model(const fs::path &directory, const fs::path &bin)
{
    auto metadata = directory / "metadata.yaml";
    check(fs::file_size(metadata) <= 65536, "metadata.yaml exceeds 64 KiB", 422);
    YAML::Node document = YAML::LoadFile(metadata.string());
    check(document.IsMap(), "Metadata must be a YAML mapping", 422);
    check(document["task"] && document["task"].as<std::string>() == "detect",
          "Only task: detect is supported", 422);
    check(document["batch"] && document["batch"].as<int>() == 1, "Only batch: 1 is supported", 422);
    check(!document["channels"] || document["channels"].as<int>() == 3,
          "Model must have three RGB input channels", 422);
    auto image = document["imgsz"];
    check(image && image.IsSequence() && image.size() == 2, "imgsz must be [height,width]", 422);
    int height = image[0].as<int>(), width = image[1].as<int>();
    check(width > 0 && width <= 4096 && height > 0 && height <= 4096,
          "Invalid model image dimensions", 422);
    auto names = document["names"];
    check(names && (names.IsMap() || names.IsSequence()) && names.size() > 0 &&
              names.size() <= 1020,
          "Metadata needs class names", 422);
    Json labels = Json::array();
    for (size_t i = 0; i < names.size(); ++i)
    {
        auto node = names.IsMap() ? names[int(i)] : names[i];
        check(node && node.IsScalar(), "Class IDs must be consecutive starting at zero", 422);
        auto name = node.as<std::string>();
        check(!name.empty() && name.size() <= 128, "Invalid class name", 422);
        labels.push_back(name);
    }
    Json info = {{"width", width},
                 {"height", height},
                 {"task", "detect"},
                 {"classes", labels.size()},
                 {"names", labels},
                 {"size_bytes", fs::file_size(directory / "model.rknn")},
                 {"accuracy_verified", false},
                 {"compatible", false}};
    for (const auto *field : {"description", "author", "version", "license", "date", "docs"})
    {
        if (document[field] && document[field].IsScalar())
        {
            auto value = document[field].as<std::string>();
            check(value.size() <= 2048, "Metadata field is too long", 422);
            info[field] = value;
        }
    }
    try
    {
        auto tensors = parse_program_json(
            capture({(bin / "rk-model-info").string(), (directory / "model.rknn").string()}, 30));
        info["tensors"] = tensors;
        check(tensors["inputs"].size() == 1, "Model must have one input", 422);
        const auto &input = tensors["inputs"][0];
        auto dims = input["dims"];
        bool nhwc = input["format"] == "NHWC";
        check(nhwc || input["format"] == "NCHW", "Input must be NHWC or NCHW", 422);
        check(dims.size() == 4 && dims[0] == 1 && dims[nhwc ? 1 : 2] == height &&
                  dims[nhwc ? 2 : 3] == width && dims[nhwc ? 3 : 1] == 3,
              "Metadata dimensions differ from RKNN input", 422);
        check(width % 16 == 0 && height % 2 == 0,
              "RKNN image dimensions violate hardware alignment", 422);
        const auto &outputs = tensors["outputs"];
        size_t classes = labels.size();
        check(!document["end2end"] || !document["end2end"].as<bool>() || outputs.size() == 2,
              "Post-NMS end-to-end model outputs are unsupported", 422);
        if (outputs.size() == 1)
        {
            auto shape = outputs[0]["dims"];
            check(shape.size() == 3 && shape[0] == 1, "Unsupported detection output rank", 422);
            bool first = shape[1].get<int>() < shape[2].get<int>();
            check(shape[first ? 1 : 2].get<size_t>() == classes + 4 &&
                      shape[first ? 2 : 1].get<int>() > int(classes + 4),
                  "Class count/output layout mismatch", 422);
            info["postprocessor"] = "decoded YOLO";
        }
        else if (outputs.size() == 2)
        {
            int boxes = -1;
            for (size_t i = 0; i < 2; ++i)
            {
                const auto &out = outputs[i];
                auto shape = out["dims"];
                check(shape.size() == 3 && shape[0] == 1 && shape[1].get<int>() > 0 &&
                          shape[2].get<int>() > 0,
                      "YOLO26 outputs need batch-1 rank-3 tensors", 422);
                std::string type = out["type"];
                check(type == "INT8" || type == "UINT8" || type == "FP16" || type == "FP32" ||
                          type == "FLOAT16" || type == "FLOAT32",
                      "Unsupported YOLO26 output type", 422);
                if (type == "INT8" || type == "UINT8")
                {
                    double scale = out.at("scale");
                    check(out["quantization"] == "AFFINE" && std::isfinite(scale) && scale > 0,
                          "Invalid YOLO26 output quantization", 422);
                }
                if (shape[1] == 4 || shape[2] == 4)
                {
                    if (boxes < 0 ||
                        out.value("name", std::string()).find("Mul_2") != std::string::npos)
                    {
                        boxes = int(i);
                    }
                }
            }
            check(boxes >= 0, "YOLO26 requires an xyxy boxes tensor", 422);
            auto b = outputs[size_t(boxes)]["dims"], s = outputs[size_t(1 - boxes)]["dims"];
            int proposals = b[b[1] == 4 ? 2 : 1];
            check((s[1] == classes && s[2] == proposals) || (s[2] == classes && s[1] == proposals),
                  "YOLO26 score/box shape or metadata class count mismatch", 422);
            info["postprocessor"] = "YOLO26 scores/xyxy";
            info["nms"] = false;
        }
        else
        {
            check(outputs.size() == 9,
                  "Supported outputs: decoded YOLO (1), YOLO26 scores/xyxy (2), INT8 DFL (9)", 422);
            for (size_t i = 0; i < 9; ++i)
            {
                const auto &output = outputs[i];
                auto shape = output["dims"];
                check(shape.size() == 4 && shape[0] == 1 && output["format"] == "NCHW" &&
                          output["type"] == "INT8" && output["quantization"] == "AFFINE",
                      "DFL outputs must be affine INT8 NCHW", 422);
                double scale = output.at("scale");
                check(std::isfinite(scale) && scale > 0, "Invalid DFL quantization scale", 422);
                check(shape[1].get<size_t>() == (i % 3 == 0   ? 64
                                                 : i % 3 == 1 ? classes
                                                              : 1),
                      "DFL output channel count mismatch", 422);
                auto base = outputs[i - i % 3]["dims"];
                int gh = shape[2], gw = shape[3];
                check(gh > 0 && gw > 0 && shape[2] == base[2] && shape[3] == base[3] &&
                          width % gw == 0 && height % gh == 0 && width / gw == height / gh,
                      "DFL grid/stride mismatch", 422);
            }
            info["postprocessor"] = "INT8 DFL";
        }
        info["tensor_type"] = input["type"];
        info["compatible"] = true;
        info["reason"] = "Runtime-loaded; tensor contract validated. Accuracy is unverified.";
    }
    catch (const std::exception &error)
    {
        info["reason"] = std::string(error.what()).substr(0, 500);
    }
    return info;
}
} // namespace nvr::web
