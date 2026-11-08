#include "model/metadata.hpp"
#include "engine/common.hpp"

#include <yaml-cpp/yaml.h>

namespace nvr
{
Json read_model_metadata(const std::filesystem::path &path)
{
    require(std::filesystem::file_size(path) <= 65536, "metadata.yaml exceeds 64 KiB");
    auto document = YAML::LoadFile(path.string());
    require(document.IsMap(), "Metadata must be a mapping");
    require(document["task"] && document["task"].IsScalar(), "Metadata must specify its task");
    const std::string task = document["task"].as<std::string>();
    model_task(task);
    require(document["batch"] && document["batch"].as<int>() == 1, "Only batch: 1 is supported");
    require(!document["channels"] || document["channels"].as<int>() == 3,
            "Model requires RGB input");
    auto image = document["imgsz"];
    require(image && image.IsSequence() && image.size() == 2, "imgsz must be [height,width]");
    int height = image[0].as<int>(), width = image[1].as<int>();
    require(width > 0 && width <= 4096 && height > 0 && height <= 4096 && width % 16 == 0 &&
                height % 2 == 0,
            "Invalid model dimensions or hardware alignment");
    auto names = document["names"];
    require(names && (names.IsSequence() || names.IsMap()) && names.size() > 0 &&
                names.size() <= 65535,
            "Metadata requires class names");
    Json labels = Json::array();
    for (size_t i = 0; i < names.size(); ++i)
    {
        auto node = names.IsMap() ? names[int(i)] : names[i];
        require(node && node.IsScalar(), "Class IDs must start at zero and be consecutive");
        auto name = node.as<std::string>();
        require(!name.empty() && name.size() <= 128, "Invalid class name");
        labels.push_back(name);
    }
    Json info = {{"task", task},
                 {"width", width},
                 {"height", height},
                 {"classes", labels.size()},
                 {"names", labels},
                 {"end2end", document["end2end"] ? document["end2end"].as<bool>() : false}};
    if (document["kpt_shape"])
    {
        auto shape = document["kpt_shape"];
        require(shape.IsSequence() && shape.size() == 2, "kpt_shape must have two dimensions");
        int count = shape[0].as<int>(), dimensions = shape[1].as<int>();
        require(count > 0 && count <= 512 && (dimensions == 2 || dimensions == 3),
                "Invalid keypoint shape");
        info["kpt_shape"] = {count, dimensions};
    }
    else if (task == "pose")
    {
        throw std::runtime_error("Pose metadata requires kpt_shape");
    }
    for (const auto *field :
         {"description", "author", "version", "license", "date", "docs", "head", "coordinates",
          "preprocess", "dense_resize", "precision", "head_precision", "decode_precision",
          "target_platform"})
    {
        if (document[field] && document[field].IsScalar())
        {
            auto value = document[field].as<std::string>();
            require(value.size() <= 2048, "Metadata field is too long");
            info[field] = value;
        }
    }
    if (!info.contains("coordinates"))
    {
        auto args = document["args"];
        // Ultralytics INT8 RKNN exports normalize coordinates to preserve score precision.
        bool normalized = args && args["quantize"] && args["quantize"].as<std::string>() == "8";
        info["coordinates"] = normalized ? "normalized" : "pixels";
    }
    require(info["coordinates"] == "pixels" || info["coordinates"] == "normalized",
            "Invalid coordinate units");
    if (info.contains("preprocess"))
    {
        require(info["preprocess"] == "letterbox" || info["preprocess"] == "center_crop",
                "Invalid preprocessing policy");
    }
    if (info.contains("dense_resize"))
    {
        require(info["dense_resize"] == "none" || info["dense_resize"] == "half_pixel" ||
                    info["dense_resize"] == "align_corners",
                "Invalid dense interpolation policy");
    }
    return info;
}

ModelOptions configured_model_options(const Json &config)
{
    if (config.contains("model_info"))
    {
        return model_options(config.at("model_info"));
    }
    auto directory = std::filesystem::path(config.at("model").get<std::string>()).parent_path();
    auto sidecar = directory / "metadata.yaml";
    if (std::filesystem::is_regular_file(sidecar))
    {
        return model_options(read_model_metadata(sidecar));
    }
    return model_options(config);
}
} // namespace nvr
