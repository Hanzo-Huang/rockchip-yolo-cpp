#include "server/services/application.hpp"

#include <drogon/WebSocketController.h>
#include <set>

namespace nvr::web
{
using namespace drogon;

class LiveSocket : public WebSocketController<LiveSocket, false>
{
    struct Client
    {
        WebSocketConnectionPtr socket;
        std::string id, run;
        uint64_t cursor = 0;
    };

    Application &app_;
    std::mutex mutex_;
    std::map<const void *, Client> clients_;
    std::map<std::string, std::string> enabled_;

public:
    explicit LiveSocket(Application &app) : app_(app)
    {
    }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/video");

    WS_PATH_LIST_END
    void handleNewConnection(const HttpRequestPtr &request,
                             const WebSocketConnectionPtr &socket) override
    {
        auto id = request->getParameter("channel");
        if (id.empty() || id.size() > 80)
        {
            socket->shutdown(CloseCode::kViolation, "Invalid channel");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.emplace(socket.get(), Client{socket, id, ""});
    }

    void handleNewMessage(const WebSocketConnectionPtr &, std::string &&,
                          const WebSocketMessageType &) override
    {
    }

    void handleConnectionClosed(const WebSocketConnectionPtr &socket) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(socket.get());
    }

    void tick()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::set<std::string> requested;
        for (auto &pair : clients_)
        {
            if (pair.second.socket->connected())
            {
                requested.insert(pair.second.id);
            }
        }
        for (auto &pair : enabled_)
        {
            if (!requested.count(pair.first))
            {
                app_.supervisor.streaming(pair.first, false);
            }
        }
        for (auto &id : requested)
        {
            auto state = app_.supervisor.state(id);
            auto run = state.value("run_id", std::string());
            if (!enabled_.count(id) || enabled_[id] != run)
            {
                app_.supervisor.streaming(id, true);
                enabled_[id] = run;
            }
        }
        for (auto it = enabled_.begin(); it != enabled_.end();)
        {
            if (!requested.count(it->first))
            {
                it = enabled_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto &pair : clients_)
        {
            auto &client = pair.second;
            if (!client.socket->connected())
            {
                continue;
            }
            auto state = app_.supervisor.state(client.id);
            auto run = state.value("run_id", std::string());
            if (state.contains("stream_error") && !state["stream_error"].is_null())
            {
                client.socket->send(Json{{"error", state["stream_error"]}}.dump());
                continue;
            }
            if (client.run != run)
            {
                client.run = run;
                client.cursor = 0;
            }
            for (auto &packet : app_.supervisor.video(client.id, client.cursor))
            {
                client.socket->send(packet.second, WebSocketMessageType::Binary);
                client.cursor = packet.first;
            }
        }
    }
};

class DetectionSocket : public WebSocketController<DetectionSocket, false>
{
    struct Client
    {
        WebSocketConnectionPtr socket;
        std::string id, run;
        uint64_t cursor = 0;
    };

    Application &app_;
    std::mutex mutex_;
    std::map<const void *, Client> clients_;

public:
    explicit DetectionSocket(Application &app) : app_(app)
    {
    }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/detections");

    WS_PATH_LIST_END
    void handleNewConnection(const HttpRequestPtr &request,
                             const WebSocketConnectionPtr &socket) override
    {
        auto id = request->getParameter("channel");
        if (id.empty() || id.size() > 80)
        {
            socket->shutdown(CloseCode::kViolation, "Invalid channel");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.emplace(socket.get(), Client{socket, id, ""});
    }

    void handleNewMessage(const WebSocketConnectionPtr &, std::string &&,
                          const WebSocketMessageType &) override
    {
    }

    void handleConnectionClosed(const WebSocketConnectionPtr &socket) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(socket.get());
    }

    void tick()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &pair : clients_)
        {
            auto &client = pair.second;
            if (!client.socket->connected())
            {
                continue;
            }
            auto run = app_.supervisor.state(client.id).value("run_id", std::string());
            if (client.run != run)
            {
                client.run = run;
                client.cursor = 0;
            }
            for (auto &item : app_.supervisor.detection(client.id, client.cursor))
            {
                client.socket->send(item.second.dump());
                client.cursor = item.first;
            }
        }
    }
};

void register_live(Application &application)
{
    auto video = std::make_shared<LiveSocket>(application);
    auto detection = std::make_shared<DetectionSocket>(application);
    drogon::app().registerController(video);
    drogon::app().registerController(detection);
    drogon::app().registerBeginningAdvice(
        [video, detection]
        {
            drogon::app().getLoop()->runEvery(.005,
                                              [video]
                                              {
                                                  video->tick();
                                              });
            drogon::app().getLoop()->runEvery(.02,
                                              [detection]
                                              {
                                                  detection->tick();
                                              });
        });
}
} // namespace nvr::web
