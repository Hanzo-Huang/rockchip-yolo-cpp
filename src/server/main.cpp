#include "server/services/application.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sys/stat.h>
#include <arpa/inet.h>

namespace
{
volatile std::sig_atomic_t stopping = 0;

void stop(int)
{
    stopping = 1;
}
} // namespace

int main(int argc, char **argv)
{
    using namespace nvr::web;
    try
    {
        // The launcher supplies paths. Direct execution may use NVR_HOME or cwd.
        const char *home = std::getenv("NVR_HOME");
        auto project = home ? fs::absolute(home) : fs::current_path();
        auto bin = fs::canonical(fs::path(argv[0])).parent_path();
        fs::path data = project / ".local/state", web = project / ".local/build/web";
        int port = 8080;
        std::string bind = "0.0.0.0";
        for (int i = 1; i < argc; ++i)
        {
            std::string option = argv[i];
            check(i + 1 < argc, "Expected value after " + option);
            if (option == "--data")
            {
                data = fs::absolute(argv[++i]);
            }
            else if (option == "--web")
            {
                web = fs::absolute(argv[++i]);
            }
            else if (option == "--port")
            {
                port = std::stoi(argv[++i]);
            }
            else if (option == "--bind")
            {
                bind = argv[++i];
            }
            else
            {
                throw Error(400, "Unknown option: " + option);
            }
        }
        check(port >= 1 && port <= 65535, "Port must be 1–65535");
        check(fs::exists(web / "index.html"), "Run ./nvr build first");
        in_addr address{};
        check(inet_pton(AF_INET, bind.c_str(), &address) == 1,
              "Bind address must be an IPv4 address");
        umask(0077);
        fs::create_directories(data);
        fs::permissions(data, fs::perms::owner_all);
        Application application(data, bin);
        auto &server = drogon::app();
        server.addListener(bind, uint16_t(port))
            .setThreadNum(2)
            .setDocumentRoot(web.string())
            .setUploadPath((data / "staging/http").string())
            .setClientMaxBodySize(5 * 1024 * 1024)
            .setClientMaxMemoryBodySize(5 * 1024 * 1024)
            .setClientMaxWebSocketMessageSize(1024)
            .setMaxConnectionNum(128)
            .setIdleConnectionTimeout(30);
        server.registerPreSendingAdvice(
            [](const drogon::HttpRequestPtr &, const drogon::HttpResponsePtr &response)
            {
                response->addHeader("X-Content-Type-Options", "nosniff");
                response->addHeader("Referrer-Policy", "no-referrer");
                response->addHeader(
                    "Content-Security-Policy",
                    "default-src 'self'; connect-src 'self' ws: wss:; img-src 'self' blob: "
                    "data:; media-src 'self' blob:; style-src 'self' 'unsafe-inline'; "
                    "frame-ancestors 'none'; base-uri 'self'; object-src 'none'");
            });
        application.register_routes();
        register_sockets(application);
        register_live(application);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::signal(SIGPIPE, SIG_IGN);
        server.registerBeginningAdvice(
            [&server]
            {
                server.getLoop()->runEvery(.2,
                                           [&server]
                                           {
                                               if (stopping)
                                               {
                                                   server.quit();
                                               }
                                           });
            });
        std::cout << "Rockchip YOLO listening on " << bind << ":" << port
                  << ". Open http://<board-ip>:" << port
                  << " (trusted network only; no authentication).\n";
        server.run();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
