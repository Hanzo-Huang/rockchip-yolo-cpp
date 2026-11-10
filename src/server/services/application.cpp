#include "server/services/application.hpp"

#include <cmath>
#include <set>

namespace nvr::web
{
// Background job execution.
Executor::Executor(size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        threads_.emplace_back(
            [this]
            {
                while (true)
                {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        ready_.wait(lock,
                                    [&]
                                    {
                                        return stopping_ || !tasks_.empty();
                                    });
                        if (stopping_ && tasks_.empty())
                        {
                            return;
                        }
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    try
                    {
                        task();
                    }
                    catch (...)
                    {
                    }
                }
            });
    }
}

Executor::~Executor()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto &t : threads_)
    {
        t.join();
    }
}

bool Executor::submit(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || tasks_.size() >= 32)
    {
        return false;
    }
    tasks_.push(std::move(task));
    ready_.notify_one();
    return true;
}

// Application state and API presentation.
Application::Application(fs::path directory, fs::path binaries)
    : root(std::move(directory)), bin(std::move(binaries)), store(root / "app.db"),
      supervisor(bin / "rk-nvr-worker", root), warehouse(store, supervisor, root, bin, mutation)
{
}

Json Application::public_asset(Json asset, bool detail)
{
    auto &info = asset["info"];
    if (asset["kind"] == "model" && !info.contains("toolkit_version"))
    {
        const auto version = embedded_rknn_toolkit_version(asset["path"].get<std::string>());
        if (!version.empty())
        {
            info["toolkit_version"] = version;
        }
    }
    asset.erase("path");
    asset["references"] = store.references(asset["id"]);
    if (info.contains("tensors") && info["tensors"].contains("outputs"))
    {
        std::set<std::string> types;
        for (const auto &tensor : info["tensors"].at("outputs"))
        {
            // Older inspections only recorded the logical tensor type.
            auto type = tensor.value("native_type", tensor.value("type", std::string()));
            if (type == "FLOAT16")
            {
                type = "FP16";
            }
            if (type == "FLOAT32")
            {
                type = "FP32";
            }
            if (!type.empty() && type != "UNKNOW")
            {
                types.insert(type);
            }
        }
        info["output_types"] = types;
    }
    if (!detail)
    {
        info.erase("tensors");
    }
    return asset;
}

Json Application::public_channel(Json channel)
{
    auto &config = channel["config"];
    if (config.contains("rtsp_url"))
    {
        config["rtsp_display"] = redact_url(config["rtsp_url"]);
        // Ordinary stream URLs are editable, without exposing stored credentials
        // through this unauthenticated LAN API.
        if (config["rtsp_display"] != config["rtsp_url"])
        {
            config.erase("rtsp_url");
        }
    }
    channel["runtime"] = supervisor.state(channel["id"]);
    return channel;
}

Json Application::snapshot()
{
    Json channels = Json::array(), videos = Json::array(), models = Json::array();
    for (auto &item : store.channels())
    {
        channels.push_back(public_channel(item));
    }
    for (auto &item : store.assets("video"))
    {
        videos.push_back(public_asset(item));
    }
    for (auto &item : store.assets("model"))
    {
        models.push_back(public_asset(item));
    }
    return {{"channels", channels},
            {"videos", videos},
            {"models", models},
            {"jobs", store.jobs()},
            {"system", system_metrics(root)},
            {"capabilities",
             {{"camera_formats", Json::array({"H264"})},
              {"max_active_workers", 8},
              {"preview", "Live H.264 stream-copy; original MP4 files; separate detections"},
              {"access", "LAN (unauthenticated)"}}}};
}

// Channel configuration and runtime actions.
Json Application::save_channel(const Json &input, const std::string &id)
{
    std::lock_guard<std::mutex> lock(mutation);
    bool editing = !id.empty();
    Json existing;
    if (editing)
    {
        existing = store.channel(id);
    }
    Json values = editing ? existing["config"] : Json::object();
    if (editing)
    {
        values["name"] = existing["name"];
    }
    values.update(input);
    Json channel = editing ? existing : Json{{"id", new_id()}, {"created_at", now_iso()}};
    channel["name"] = display_name(values);
    std::string type = values.value("source_type", std::string("file"));
    check(type == "file" || type == "rtsp" || type == "camera", "Invalid source type");
    Json config = {{"source_type", type}};
    if (type == "file")
    {
        std::string video_id =
            values.value("video_id", editing && !existing["video_id"].is_null()
                                         ? existing["video_id"].get<std::string>()
                                         : std::string());
        auto asset = store.asset(video_id);
        check(asset["kind"] == "video" && asset["status"] == "ready", "Select a compatible video");
        channel["video_id"] = video_id;
    }
    else
    {
        channel["video_id"] = nullptr;
        if (type == "rtsp")
        {
            std::string url = values.value("rtsp_url", std::string());
            if (url.empty() && editing)
            {
                url = existing["config"].value("rtsp_url", std::string());
            }
            check(url.size() <= 2048 &&
                      (url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0) &&
                      url.find_first_of("\r\n") == std::string::npos,
                  "Provide an RTSP URL");
            config["rtsp_url"] = url;
        }
        else
        {
            std::string camera = values.value("camera_id", std::string());
            bool usable = false;
            for (auto &device : discover_cameras())
            {
                if (device["id"] == camera && device["supported"] == true)
                {
                    usable = true;
                }
            }
            check(usable, "Select a supported detected camera");
            config["camera_id"] = camera;
        }
    }
    std::string model_id = values.value("model_id", editing && !existing["model_id"].is_null()
                                                        ? existing["model_id"].get<std::string>()
                                                        : std::string());
    auto model = store.asset(model_id);
    check(model["kind"] == "model" && model["status"] == "ready",
          "Select a runtime-compatible model");
    channel["model_id"] = model_id;
    std::string preprocessing = values.value("preprocess", std::string("rga"));
    check(preprocessing == "rga" || preprocessing == "cpu", "Invalid preprocessing mode");
    int every = values.value("detect_every", 1);
    check(every >= 1 && every <= 30, "Inference interval must be 1–30");
    int workers = values.value("inference_workers",
                               editing ? existing["config"].value("inference_workers", 0) : 0);
    check(workers == 0 || workers == 1, "Inference workers must be 0 (auto) or 1 (single context)");
    double confidence = values.value("confidence", 0.35), iou = values.value("nms_iou", 0.45);
    check(std::isfinite(confidence) && confidence > 0 && confidence < 1 && std::isfinite(iou) &&
              iou > 0 && iou < 1,
          "Invalid detection thresholds");
    config.update({{"preprocess", preprocessing},
                   {"detect_every", every},
                   {"inference_workers", workers},
                   {"confidence", confidence},
                   {"nms_iou", iou},
                   {"recording", values.value("recording", type != "file")}});
    if (editing && supervisor.active(id))
    {
        auto previous_config = existing["config"];
        previous_config["inference_workers"] = previous_config.value("inference_workers", 0);
        check(config == previous_config && channel["video_id"] == existing["video_id"] &&
                  channel["model_id"] == existing["model_id"],
              "Stop the channel before changing its source, model or processing settings", 409);
    }
    channel["config"] = config;
    store.save_channel(channel);
    return public_channel(channel);
}

Json Application::channel_command(const std::string &id, const Json &input)
{
    std::lock_guard<std::mutex> lock(mutation);
    auto channel = store.channel(id);
    std::string action = input.at("action");
    auto config = channel["config"];
    std::string type = config["source_type"];
    if (action == "start")
    {
        auto model = store.asset(channel["model_id"]);
        check(model["status"] == "ready", "Model is incompatible", 409);
        config["model"] = model["path"];
        config["model_id"] = model["id"];
        config["model_info"] = model["info"];
        if (type == "file")
        {
            auto video = store.asset(channel["video_id"]);
            config["uri"] = video["path"];
            config["video_id"] = video["id"];
        }
        else if (type == "rtsp")
        {
            config["uri"] = config["rtsp_url"];
        }
        else
        {
            bool usable = false;
            for (auto &device : discover_cameras())
            {
                if (device["id"] == config["camera_id"] && device["supported"] == true)
                {
                    config["uri"] = device["device"];
                    usable = true;
                }
            }
            check(usable, "Camera is disconnected or unsupported", 409);
        }
        return supervisor.start(id, config);
    }
    auto state = supervisor.state(id);
    std::string status = state.value("state", std::string());
    check(action == "pause" || action == "resume" || action == "stop" || action == "seek",
          "Unknown channel action");
    if (action == "pause")
    {
        check(status == "running", "Channel is not running", 409);
    }
    if (action == "resume")
    {
        check(status == "paused" || status == "analytics_paused", "Channel is not paused", 409);
    }
    Json message = {{"command", action}, {"request_id", new_id()}};
    if (action == "seek")
    {
        check(type == "file" && (status == "running" || status == "paused"),
              "This channel cannot seek now", 409);
        double seconds = input.at("seconds");
        double duration = state.value("duration", 0.0);
        check(std::isfinite(seconds) && seconds >= 0 && duration > 0 && seconds < duration,
              "Seek position is outside the video");
        message["seconds"] = seconds;
    }
    return supervisor.command(id, message);
}
} // namespace nvr::web
