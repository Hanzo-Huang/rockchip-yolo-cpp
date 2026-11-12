#include "server/services/application.hpp"

#include <sstream>
#include <cmath>
#include <fstream>
#include <charconv>

namespace nvr::web
{
using namespace drogon;

HttpResponsePtr json_response(const Json &value, int status)
{
    auto response = HttpResponse::newHttpResponse();
    response->setStatusCode(HttpStatusCode(status));
    response->setContentTypeCode(CT_APPLICATION_JSON);
    response->setBody(value.dump());
    response->addHeader("Cache-Control", "no-store");
    return response;
}

namespace
{
std::vector<std::string> segments(const std::string &path)
{
    std::istringstream input(path);
    std::string piece;
    std::vector<std::string> result;
    while (std::getline(input, piece, '/'))
    {
        if (!piece.empty())
        {
            result.push_back(piece);
        }
    }
    return result;
}

Json body_json(const HttpRequestPtr &request, size_t limit = 65536)
{
    check(request->body().size() <= limit, "JSON body is too large", 413);
    auto value = Json::parse(request->body());
    check(value.is_object(), "Expected a JSON object");
    return value;
}

HttpResponsePtr file_response(const fs::path &path, const HttpRequestPtr &req,
                              const std::string &name = {})
{
    check(fs::is_regular_file(path), "File not found", 404);
    auto range = req->getHeader("range");
    size_t size = fs::file_size(path);
    HttpResponsePtr response;
    if (!range.empty())
    {
        auto invalid = [&]
        {
            auto result = json_response({{"error", "Requested range is not satisfiable"}}, 416);
            result->addHeader("Content-Range", "bytes */" + std::to_string(size));
            return result;
        };
        if (range.rfind("bytes=", 0) != 0 || range.find(',') != std::string::npos || size == 0)
        {
            return invalid();
        }
        auto dash = range.find('-', 6);
        if (dash == std::string::npos)
        {
            return invalid();
        }
        auto number = [](std::string_view text, size_t &output)
        {
            auto value = std::from_chars(text.data(), text.data() + text.size(), output);
            return !text.empty() && value.ec == std::errc{} &&
                   value.ptr == text.data() + text.size();
        };
        size_t first = 0, last = size - 1;
        auto left = std::string_view(range).substr(6, dash - 6),
             right = std::string_view(range).substr(dash + 1);
        if (left.empty())
        {
            size_t suffix = 0;
            if (!number(right, suffix) || suffix == 0)
            {
                return invalid();
            }
            first = size - std::min(size, suffix);
        }
        else
        {
            if (!number(left, first) || first >= size)
            {
                return invalid();
            }
            if (!right.empty() && !number(right, last))
            {
                return invalid();
            }
            last = std::min(last, size - 1);
            if (last < first)
            {
                return invalid();
            }
        }
        response = HttpResponse::newFileResponse(path.string(), first, last - first + 1, true, name,
                                                 CT_NONE, "", req);
        response->setStatusCode(k206PartialContent);
    }
    else
    {
        response = HttpResponse::newFileResponse(path.string(), name, CT_NONE, "", req);
    }
    response->addHeader("Accept-Ranges", "bytes");
    response->addHeader("Cache-Control", "private, no-cache");
    return response;
}
} // namespace

HttpResponsePtr Application::route(const HttpRequestPtr &request, const std::string &path)
{
    const auto parts = segments(path);
    const auto method = request->method();
    check(!parts.empty(), "API route not found", 404);
    const auto &resource = parts[0];
    if (resource == "state" && parts.size() == 1 && method == Get)
    {
        return json_response(snapshot());
    }
    if (resource == "cameras" && parts.size() == 1 && method == Get)
    {
        return json_response(discover_cameras());
    }
    if (resource == "jobs" && method == Get)
    {
        return json_response(parts.size() == 1 ? store.jobs() : store.job(parts[1]));
    }
    if (resource == "uploads")
    {
        if (parts.size() == 1 && method == Post)
        {
            return json_response(warehouse.begin(body_json(request)), 201);
        }
        check(parts.size() >= 2, "Upload not found", 404);
        const auto &id = parts[1];
        if (parts.size() == 2 && method == Get)
        {
            return json_response(store.job(id));
        }
        if (parts.size() == 2 && method == Delete)
        {
            warehouse.cancel(id);
            return json_response({{"ok", true}});
        }
        if (parts.size() == 3 && parts[2] == "complete" && method == Post)
        {
            return json_response(warehouse.complete(id), 202);
        }
        if (parts.size() == 4 && parts[2] == "files" && method == Put)
        {
            check(!parts[3].empty() &&
                      parts[3].find_first_not_of("0123456789") == std::string::npos,
                  "Invalid file index");
            auto offset = request->getHeader("x-upload-offset");
            check(!offset.empty() && offset.find_first_not_of("0123456789") == std::string::npos,
                  "Upload offset is required");
            return json_response(warehouse.append(id, size_t(std::stoull(parts[3])),
                                                  std::stoull(offset), request->body()));
        }
    }
    if (resource == "videos" || resource == "models")
    {
        const std::string kind = resource == "videos" ? "video" : "model";
        if (kind == "model" && parts.size() == 2 && parts[1] == "hugging-face" && method == Post)
        {
            return json_response(warehouse.import_hugging_face(body_json(request)), 202);
        }
        if (parts.size() == 1 && method == Get)
        {
            Json result = Json::array();
            for (auto &asset : store.assets(kind))
            {
                result.push_back(public_asset(asset));
            }
            return json_response(result);
        }
        check(parts.size() >= 2, "Asset not found", 404);
        auto asset = store.asset(parts[1]);
        check(asset["kind"] == kind, "Asset not found", 404);
        if (parts.size() == 2 && method == Get)
        {
            return json_response(public_asset(asset, true));
        }
        if (kind == "model" && parts.size() == 3 && parts[2] == "revalidate" && method == Post)
        {
            return json_response(public_asset(warehouse.revalidate_model(parts[1]), true));
        }
        if (parts.size() == 2 && method == Patch)
        {
            std::lock_guard<std::mutex> lock(mutation);
            store.rename_asset(parts[1], display_name(body_json(request)));
            return json_response(public_asset(store.asset(parts[1])));
        }
        if (parts.size() == 2 && method == Delete)
        {
            warehouse.remove(parts[1], body_json(request).value("confirm_name", std::string()));
            return json_response({{"ok", true}, {"recoverable", true}});
        }
        fs::path location = asset["path"].get<std::string>();
        if (parts.size() == 3 && parts[2] == "download" && (method == Get || method == Head))
        {
            std::string name = asset["name"];
            name += kind == "video" ? ".mp4" : ".zip";
            return file_response(kind == "video" ? location : location.parent_path() / "bundle.zip",
                                 request, name);
        }
        if (kind == "video" && parts.size() == 3 && parts[2] == "content" &&
            (method == Get || method == Head))
        {
            return file_response(location, request);
        }
        if (kind == "video" && parts.size() == 3 && parts[2] == "thumbnail" &&
            (method == Get || method == Head))
        {
            return file_response(location.parent_path() / "thumbnail.jpg", request);
        }
        if (kind == "model" && parts.size() == 3 && parts[2] == "metadata" && method == Get)
        {
            std::ifstream input(location.parent_path() / "metadata.yaml");
            std::string yaml((std::istreambuf_iterator<char>(input)), {});
            return json_response({{"yaml", yaml}, {"info", asset["info"]}});
        }
        if (kind == "video" && parts.size() == 3 && parts[2] == "preview" && method == Post)
        {
            std::lock_guard<std::mutex> lock(mutation);
            check(asset["status"] == "ready", "This codec cannot use hardware preview", 409);
            std::string id = "preview-" + parts[1];
            if (!supervisor.active(id))
            {
                supervisor.start(id, {{"source_type", "file"},
                                      {"uri", asset["path"]},
                                      {"video_id", asset["id"]},
                                      {"preview_only", true},
                                      {"recording", false}});
            }
            return json_response({{"id", id}, {"runtime", supervisor.state(id)}}, 202);
        }
    }
    if (resource == "channels")
    {
        if (parts.size() == 1 && method == Get)
        {
            Json list = Json::array();
            for (auto &channel : store.channels())
            {
                list.push_back(public_channel(channel));
            }
            return json_response(list);
        }
        if (parts.size() == 1 && method == Post)
        {
            return json_response(save_channel(body_json(request, 512 * 1024)), 201);
        }
        check(parts.size() >= 2, "Channel not found", 404);
        auto channel = store.channel(parts[1]);
        if (parts.size() == 2 && method == Get)
        {
            return json_response(public_channel(channel));
        }
        if (parts.size() == 2 && method == Patch)
        {
            return json_response(save_channel(body_json(request, 512 * 1024), parts[1]));
        }
        if (parts.size() == 2 && method == Delete)
        {
            std::lock_guard<std::mutex> lock(mutation);
            check(!supervisor.active(parts[1]), "Stop the channel before deleting it", 409);
            check(body_json(request).value("confirm_name", std::string()) == channel["name"],
                  "Confirmation name does not match", 409);
            supervisor.forget(parts[1]);
            store.remove_channel(parts[1]);
            return json_response({{"ok", true}, {"recordings_preserved", true}});
        }
        if (parts.size() == 3 && parts[2] == "actions" && method == Post)
        {
            return json_response(channel_command(parts[1], body_json(request)), 202);
        }
    }
    if (resource == "previews" && parts.size() >= 2 && parts[1].rfind("preview-", 0) == 0)
    {
        auto asset = store.asset(parts[1].substr(8));
        check(asset["kind"] == "video", "Preview not found", 404);
        if (parts.size() == 2 && method == Get)
        {
            return json_response(supervisor.state(parts[1]));
        }
        if (parts.size() == 2 && method == Delete)
        {
            if (supervisor.active(parts[1]))
            {
                supervisor.command(parts[1], {{"command", "stop"}});
            }
            return json_response({{"ok", true}}, 202);
        }
        if (parts.size() == 3 && parts[2] == "actions" && method == Post)
        {
            auto input = body_json(request);
            std::string action = input.at("action");
            check(action == "pause" || action == "resume" || action == "seek" || action == "stop",
                  "Unknown preview action");
            Json command = {{"command", action}};
            if (action == "seek")
            {
                double seconds = input.at("seconds");
                check(std::isfinite(seconds) && seconds >= 0 &&
                          seconds < asset["info"].value("duration", 0.0),
                      "Invalid preview seek");
                command["seconds"] = seconds;
            }
            return json_response(supervisor.command(parts[1], command), 202);
        }
    }
    throw Error(404, "API route not found");
}

void Application::register_routes()
{
    drogon::app().registerHandlerViaRegex(
        "/api/v1/(.*)",
        [this](const HttpRequestPtr &request,
               std::function<void(const HttpResponsePtr &)> &&callback, const std::string &path)
        {
            bool accepted = executor.submit(
                [this, request, callback, path]
                {
                    try
                    {
                        callback(route(request, path));
                    }
                    catch (const Error &error)
                    {
                        callback(json_response({{"error", error.what()}}, error.status));
                    }
                    catch (const Json::exception &)
                    {
                        callback(json_response({{"error", "Invalid request data"}}, 400));
                    }
                    catch (const std::exception &)
                    {
                        callback(json_response(
                            {{"error", "Operation failed; check board storage and permissions"}},
                            500));
                    }
                });
            if (!accepted)
            {
                callback(json_response({{"error", "Server is busy; try again shortly"}}, 503));
            }
        },
        {Get, Head, Post, Put, Patch, Delete});
}
} // namespace nvr::web
