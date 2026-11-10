#include "server/services/warehouse.hpp"

#include "model/metadata.hpp"
#include "engine/task_model.hpp"
#include <fstream>
#include <cmath>
#include <regex>

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

std::string embedded_rknn_toolkit_version(const fs::path &file)
{
    // Known RKNN exports store this string in the metadata near the file header.
    // Bound the read so existing models can be inspected without loading them into RKNN.
    std::ifstream input(file, std::ios::binary);
    std::string header(4096, '\0');
    input.read(header.data(), header.size());
    header.resize(input.gcount());
    if (header.compare(0, 8, "RKNN\0\0\0\0", 8) != 0)
    {
        return {};
    }

    static const std::regex toolkit(
        R"(([0-9]+\.[0-9]+\.[0-9]+[A-Za-z0-9.+_-]{0,32})\(compiler version: [^\x00\r\n]{1,192}\)\))");
    for (std::sregex_iterator it(header.begin(), header.end(), toolkit), end; it != end; ++it)
    {
        const size_t start = it->position(), size = it->length();
        // The string has a little-endian length prefix and a trailing NUL.
        if (start >= 4 && size < 256 && start + size < header.size() &&
            static_cast<unsigned char>(header[start - 4]) == size &&
            header.compare(start - 3, 3, "\0\0\0", 3) == 0 && header[start + size] == '\0')
        {
            return (*it)[1].str();
        }
    }
    return {};
}

Json inspect_model(const fs::path &directory, const fs::path &bin)
{
    Json info = read_model_metadata(directory / "metadata.yaml");
    const int width = info.at("width"), height = info.at("height");
    const auto model = directory / "model.rknn";
    info["size_bytes"] = fs::file_size(model);
    const auto embedded_version = embedded_rknn_toolkit_version(model);
    if (!embedded_version.empty())
    {
        info["toolkit_version"] = embedded_version;
    }
    info["accuracy_verified"] = false;
    info["compatible"] = false;
    try
    {
        const auto output = capture({(bin / "rk-model-info").string(), model.string()}, 30);
        auto tensors = parse_program_json(output);
        static const std::regex toolkit(
            R"(RKNN Model Information,[^\r\n]*?toolkit version:[ \t]*)"
            R"(([0-9]+\.[0-9]+\.[0-9]+[A-Za-z0-9.+_-]{0,32})[ \t]*(?:\(|,|\r?\n|$))");
        std::smatch version;
        const auto logs_end = output.cbegin() + output.find('{');
        if (std::regex_search(output.cbegin(), logs_end, version, toolkit))
        {
            info["toolkit_version"] = version[1].str();
        }
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
        std::vector<TensorSpec> specs;
        for (const auto &tensor : tensors.at("outputs"))
        {
            const std::string type = tensor.at("type");
            check(type == "INT8" || type == "UINT8" || type == "FP16" || type == "FP32" ||
                      type == "FLOAT16" || type == "FLOAT32" || type == "INT32",
                  "Unsupported model output type", 422);
            if (tensor.value("quantization", std::string()) == "AFFINE")
            {
                double scale = tensor.at("scale");
                check(std::isfinite(scale) && scale > 0, "Invalid output quantization scale", 422);
            }
            specs.push_back(tensor_spec(tensor));
        }
        TaskPostprocessor contract(model_options(info), specs, width, height);
        info["postprocessor"] = contract.description();
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
