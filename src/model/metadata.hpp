#pragma once

#include "engine/task_json.hpp"

#include <filesystem>

namespace nvr
{
Json read_model_metadata(const std::filesystem::path &path);
ModelOptions configured_model_options(const Json &config);
} // namespace nvr
