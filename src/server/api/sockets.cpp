#include "server/services/application.hpp"

#include <drogon/WebSocketController.h>
#include <map>
#include <charconv>

namespace nvr::web
{
using namespace drogon;
using Clock = std::chrono::steady_clock;

struct Subscriber
{
    WebSocketConnectionPtr connection;
    std::string channel, run;
    int fps = 0; // Zero follows source cadence; positive values are diagnostic limits.
    uint64_t serial = 0;
    bool awaiting = false;
    Clock::time_point sent{}, next{};
};

class PreviewSocket : public WebSocketController<PreviewSocket, false>
{
    Application &application_;
    std::mutex mutex_;
    std::map<const void *, Subscriber> clients_;
    std::map<std::string, std::pair<std::string, int>> enabled_;

public:
    explicit PreviewSocket(Application &application) : application_(application)
    {
    }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/preview");

    WS_PATH_LIST_END
    void handleNewConnection(const HttpRequestPtr &request,
                             const WebSocketConnectionPtr &connection) override
    {
        auto id = request->getParameter("channel");
        if (id.empty() || id.size() > 80)
        {
            connection->shutdown(CloseCode::kViolation, "Invalid channel");
            return;
        }
        int fps = 0;
        const auto value = request->getParameter("fps");
        if (!value.empty())
        {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), fps);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || fps < 0 ||
                fps > 120)
            {
                connection->shutdown(CloseCode::kViolation, "Invalid preview rate");
                return;
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.emplace(connection.get(), Subscriber{connection, id, "", fps});
    }

    void handleNewMessage(const WebSocketConnectionPtr &connection, std::string &&message,
                          const WebSocketMessageType &type) override
    {
        if (type != WebSocketMessageType::Text || message != "ack")
        {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = clients_.find(connection.get());
        if (found != clients_.end())
        {
            found->second.awaiting = false;
        }
    }

    void handleConnectionClosed(const WebSocketConnectionPtr &connection) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(connection.get());
    }

    void tick()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, int> requested;
        for (auto &pair : clients_)
        {
            if (pair.second.connection->connected())
            {
                auto [it, inserted] = requested.emplace(pair.second.channel, pair.second.fps);
                if (!inserted)
                {
                    it->second = it->second == 0 || pair.second.fps == 0
                                     ? 0
                                     : std::max(it->second, pair.second.fps);
                }
            }
        }
        for (auto &pair : enabled_)
        {
            if (!requested.count(pair.first))
            {
                application_.supervisor.preview(pair.first, false, 0);
            }
        }
        for (auto &pair : requested)
        {
            auto state = application_.supervisor.state(pair.first);
            auto run = state.value("run_id", std::string());
            auto previous = enabled_.find(pair.first);
            if (previous == enabled_.end() || previous->second != std::make_pair(run, pair.second))
            {
                application_.supervisor.preview(pair.first, true, pair.second);
            }
            enabled_[pair.first] = {run, pair.second};
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
            if (!client.connection->connected())
            {
                continue;
            }
            if (client.awaiting)
            {
                if (Clock::now() - client.sent > std::chrono::seconds(5))
                {
                    client.connection->shutdown(CloseCode::kNormalClosure,
                                                "Preview acknowledgement timeout");
                }
                continue;
            }
            // The encoder already limits the fastest subscriber's cadence. Applying
            // a second independent clock would drop otherwise on-time frames.
            if (client.fps > 0 &&
                (requested[client.channel] == 0 || client.fps < requested[client.channel]) &&
                Clock::now() < client.next)
            {
                continue;
            }
            auto state = application_.supervisor.state(client.channel);
            auto run = state.value("run_id", std::string());
            if (run != client.run)
            {
                client.run = run;
                client.serial = 0;
            }
            auto packet = application_.supervisor.packet(client.channel);
            if (packet.second.empty() || packet.first == client.serial)
            {
                continue;
            }
            client.connection->send(packet.second, WebSocketMessageType::Binary);
            client.serial = packet.first;
            client.sent = Clock::now();
            client.awaiting = true;
            if (client.fps > 0)
            {
                if (client.next == Clock::time_point{})
                {
                    client.next = client.sent;
                }
                client.next += std::chrono::microseconds(1000000 / client.fps);
                if (client.next < client.sent)
                {
                    client.next = client.sent + std::chrono::microseconds(1000000 / client.fps);
                }
            }
        }
    }
};

class EventSocket : public WebSocketController<EventSocket, false>
{
    Application &application_;
    std::mutex mutex_;
    std::vector<WebSocketConnectionPtr> clients_;
    std::atomic<bool> pending_{false};

public:
    explicit EventSocket(Application &application) : application_(application)
    {
    }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/events");

    WS_PATH_LIST_END
    void handleNewConnection(const HttpRequestPtr &,
                             const WebSocketConnectionPtr &connection) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.push_back(connection);
    }

    void handleNewMessage(const WebSocketConnectionPtr &, std::string &&,
                          const WebSocketMessageType &) override
    {
    }

    void handleConnectionClosed(const WebSocketConnectionPtr &connection) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), connection), clients_.end());
    }

    void tick()
    {
        if (pending_.exchange(true))
        {
            return;
        }
        if (!application_.executor.submit(
                [this]
                {
                    try
                    {
                        auto value = application_.snapshot().dump();
                        std::lock_guard<std::mutex> lock(mutex_);
                        for (auto &client : clients_)
                        {
                            if (client->connected())
                            {
                                client->send(value);
                            }
                        }
                    }
                    catch (...)
                    {
                    }
                    pending_ = false;
                }))
        {
            pending_ = false;
        }
    }
};

void register_sockets(Application &application)
{
    auto previews = std::make_shared<PreviewSocket>(application);
    auto events = std::make_shared<EventSocket>(application);
    app().registerController(previews);
    app().registerController(events);
    app().registerBeginningAdvice(
        [previews, events]
        {
            // A 50 ms poll itself caps delivery at 20 FPS even with encoder limits removed.
            app().getLoop()->runEvery(.005,
                                      [previews]
                                      {
                                          previews->tick();
                                      });
            app().getLoop()->runEvery(1.0,
                                      [events]
                                      {
                                          events->tick();
                                      });
        });
}
} // namespace nvr::web
