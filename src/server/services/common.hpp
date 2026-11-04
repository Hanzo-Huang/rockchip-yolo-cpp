#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <stdexcept>
#include <functional>
#include <chrono>

namespace nvr::web
{
using Json = nlohmann::json;
namespace fs = std::filesystem;

struct Error : std::runtime_error
{
    int status;

    Error(int code, const std::string &message) : std::runtime_error(message), status(code)
    {
    }
};

inline void check(bool value, const std::string &message, int status = 400)
{
    if (!value)
    {
        throw Error(status, message);
    }
}

std::string new_id();
std::string display_name(const Json &body);
void private_write(const fs::path &path, const std::string &content);
Json read_json(const fs::path &path);
std::string capture(const std::vector<std::string> &args, int timeout_seconds = 20,
                    size_t limit = 1024 * 1024);
Json parse_program_json(const std::string &output);
std::string redact_url(const std::string &value);
std::string now_iso();
Json discover_cameras();
Json system_metrics(const fs::path &root);
} // namespace nvr::web
