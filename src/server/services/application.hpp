#pragma once

#include "warehouse.hpp"
#include <drogon/drogon.h>
#include <queue>

namespace nvr::web
{
class Executor
{
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;

public:
    explicit Executor(size_t count);
    ~Executor();

    bool submit(std::function<void()> task);
};

class Application
{
public:
    fs::path root, bin;
    Store store;
    Supervisor supervisor;
    std::mutex mutation;
    Warehouse warehouse;
    Executor executor{3};

    Application(fs::path directory, fs::path binaries);

    // Read the current application state for API responses.
    Json snapshot();
    Json public_channel(Json channel);
    Json public_asset(Json asset, bool detail = false);

    // Create/edit channels and dispatch runtime actions.
    Json save_channel(const Json &input, const std::string &id = {});
    Json channel_command(const std::string &id, const Json &input);

    // HTTP routing.
    drogon::HttpResponsePtr route(const drogon::HttpRequestPtr &request, const std::string &path);
    void register_routes();
};

drogon::HttpResponsePtr json_response(const Json &value, int status = 200);

void register_sockets(Application &application);
void register_live(Application &application);
} // namespace nvr::web
