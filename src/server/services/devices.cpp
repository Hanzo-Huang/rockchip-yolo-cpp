#include "server/services/common.hpp"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <sstream>

namespace nvr::web
{
Json discover_cameras()
{
    Json devices = Json::array();
    for (const auto &item : fs::directory_iterator("/dev"))
    {
        std::string name = item.path().filename().string();
        if (name.rfind("video", 0) != 0 || name.size() <= 5)
        {
            continue;
        }
        if (name.find_first_not_of("0123456789", 5) != std::string::npos)
        {
            continue;
        }
        int fd = open(item.path().c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
        {
            continue;
        }
        v4l2_capability cap{};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0)
        {
            close(fd);
            continue;
        }
        uint32_t flags =
            (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(flags & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)))
        {
            close(fd);
            continue;
        }
        Json formats = Json::array();
        bool h264 = false;
        for (auto type : {V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE})
        {
            v4l2_fmtdesc format{};
            format.type = type;
            for (format.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &format) == 0; ++format.index)
            {
                uint32_t code = format.pixelformat;
                char fourcc[5] = {char(code), char(code >> 8), char(code >> 16), char(code >> 24),
                                  0};
                formats.push_back(fourcc);
                if (code == V4L2_PIX_FMT_H264)
                {
                    h264 = true;
                }
            }
        }
        close(fd);
        devices.push_back(
            {{"id", name},
             {"name", reinterpret_cast<const char *>(cap.card)},
             {"device", item.path().string()},
             {"formats", formats},
             {"supported", h264},
             {"reason",
              h264 ? "H.264 capture supported; physical-device validation required"
                   : "Only H.264 V4L2 capture is enabled; MJPEG/raw/CSI ingest needs device "
                     "validation"}});
    }
    return devices;
}

Json system_metrics(const fs::path &root)
{
    static std::mutex mutex;
    static uint64_t previous_total = 0, previous_idle = 0;
    static Json cached;
    static auto updated = std::chrono::steady_clock::time_point{};
    std::lock_guard<std::mutex> lock(mutex);
    if (!cached.is_null() && std::chrono::steady_clock::now() - updated < std::chrono::seconds(1))
    {
        return cached;
    }
    updated = std::chrono::steady_clock::now();
    std::ifstream stat("/proc/stat");
    std::string label, line;
    std::getline(stat, line);
    std::istringstream cpu(line);
    cpu >> label;
    uint64_t values[10]{};
    for (auto &value : values)
    {
        cpu >> value;
    }
    uint64_t total = 0;
    for (int i = 0; i < 8; ++i)
    {
        total += values[i];
    }
    uint64_t idle = values[3] + values[4];
    Json percent = nullptr;
    if (previous_total && total > previous_total)
    {
        percent = 100.0 * (1.0 - double(idle - previous_idle) / double(total - previous_total));
    }
    previous_total = total;
    previous_idle = idle;
    std::ifstream mem("/proc/meminfo");
    uint64_t memory_total = 0, available = 0;
    while (std::getline(mem, line))
    {
        std::istringstream fields(line);
        uint64_t value = 0;
        fields >> label >> value;
        if (label == "MemTotal:")
        {
            memory_total = value * 1024;
        }
        if (label == "MemAvailable:")
        {
            available = value * 1024;
        }
    }
    Json temperature = nullptr;
    for (const auto &zone : fs::directory_iterator("/sys/class/thermal"))
    {
        if (zone.path().filename().string().rfind("thermal_zone", 0) == 0)
        {
            std::ifstream input(zone.path() / "temp");
            double value = 0;
            if (input >> value)
            {
                temperature = value / 1000;
                break;
            }
        }
    }
    Json npu = nullptr;
    std::ifstream load("/sys/kernel/debug/rknpu/load");
    if (load)
    {
        std::getline(load, line);
        npu = line;
    }
    auto space = fs::space(root);
    cached = {{"cpu_percent", percent},
              {"memory_total", memory_total},
              {"memory_used", memory_total - available},
              {"storage_free", space.available},
              {"storage_total", space.capacity},
              {"temperature_c", temperature},
              {"npu_load", npu}};
    return cached;
}
} // namespace nvr::web
