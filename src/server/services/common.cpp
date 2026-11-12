#include "server/services/common.hpp"

#include <sstream>
#include <ctime>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <poll.h>
#include <spawn.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
extern char **environ;

namespace nvr::web
{
std::string new_id()
{
    std::array<unsigned char, 16> bytes{};
    check(getrandom(bytes.data(), bytes.size(), 0) == ssize_t(bytes.size()),
          "random ID generation failed", 500);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto b : bytes)
    {
        out << std::setw(2) << int(b);
    }
    return out.str();
}

std::string display_name(const Json &body)
{
    check(body.contains("name") && body["name"].is_string(), "Name is required");
    std::string name = body["name"];
    check(!name.empty() && name.size() <= 120, "Name must be 1–120 bytes");
    check(name.find_first_not_of(" \t\r\n") != std::string::npos, "Name cannot be blank");
    for (unsigned char c : name)
    {
        check(c >= 32 && c != 127, "Name contains control characters");
    }
    return name;
}

void private_write(const fs::path &path, const std::string &content)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    check(fd >= 0, "Cannot create private file", 500);
    size_t offset = 0;
    while (offset < content.size())
    {
        ssize_t n = write(fd, content.data() + offset, content.size() - offset);
        if (n <= 0)
        {
            close(fd);
            throw Error(500, "File write failed");
        }
        offset += size_t(n);
    }
    int result = fsync(fd);
    close(fd);
    check(result == 0, "File sync failed", 500);
}

Json read_json(const fs::path &path)
{
    std::ifstream input(path);
    check(bool(input), "Cannot read file", 500);
    Json j;
    input >> j;
    return j;
}

std::string capture(const std::vector<std::string> &args, int timeout_seconds, size_t limit,
                    std::ostream *destination, const std::atomic_bool *cancelled)
{
    check(!args.empty(), "Empty subprocess command", 500);
    int descriptors[2];
    check(pipe2(descriptors, O_CLOEXEC) == 0, "Create subprocess pipe", 500);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    if (destination)
    {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    else
    {
        posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
    }
    posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    std::vector<char *> argv;
    for (auto &argument : args)
    {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    int result = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(descriptors[1]);
    if (result != 0)
    {
        close(descriptors[0]);
        throw Error(500, "Cannot launch validation tool");
    }
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    std::string output;
    size_t received = 0;
    auto consume = [&](const char *bytes, size_t count)
    {
        if (count > limit - received)
        {
            return false;
        }
        received += count;
        if (destination)
        {
            return bool(destination->write(bytes, std::streamsize(count)));
        }
        output.append(bytes, count);
        return true;
    };
    int status = 0;
    bool exited = false, failed = false;
    auto start = std::chrono::steady_clock::now();
    while (!exited)
    {
        pollfd input{descriptors[0], POLLIN, 0};
        poll(&input, 1, 50);
        char buffer[8192];
        ssize_t n;
        while ((n = read(descriptors[0], buffer, sizeof(buffer))) > 0)
        {
            if (!consume(buffer, size_t(n)))
            {
                failed = true;
                break;
            }
        }
        exited = waitpid(pid, &status, WNOHANG) == pid;
        const bool stopped = cancelled && cancelled->load();
        if (failed || stopped ||
            std::chrono::steady_clock::now() - start > std::chrono::seconds(timeout_seconds))
        {
            if (!exited)
            {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            close(descriptors[0]);
            throw Error(422, stopped ? "Operation interrupted"
                                     : "Validation exceeded its resource limit");
        }
    }
    char tail[8192];
    ssize_t n;
    while ((n = read(descriptors[0], tail, sizeof(tail))) > 0)
    {
        if (!consume(tail, size_t(n)))
        {
            failed = true;
            break;
        }
    }
    close(descriptors[0]);
    check(!failed, "Validation exceeded its resource limit", 422);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Validation tool rejected the file", 422);
    return output;
}

Json parse_program_json(const std::string &output)
{
    auto begin = output.find('{'), end = output.rfind('}');
    check(begin != std::string::npos && end > begin, "Invalid probe response", 422);
    return Json::parse(output.substr(begin, end - begin + 1));
}

std::string redact_url(const std::string &value)
{
    std::string safe = value;
    auto query = safe.find_first_of("?#");
    if (query != std::string::npos)
    {
        safe = safe.substr(0, query) + "?***";
    }
    auto protocol = safe.find("://"), at = safe.find('@');
    if (protocol != std::string::npos && at != std::string::npos && at > protocol)
    {
        return safe.substr(0, protocol + 3) + "***@" + safe.substr(at + 1);
    }
    return safe;
}

std::string now_iso()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}
} // namespace nvr::web
