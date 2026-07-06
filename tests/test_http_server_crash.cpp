#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const int kPort = 8083;
static pid_t g_server_pid = -1;

int connect_server()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect");
        close(sock);
        return -1;
    }
    return sock;
}

long parse_content_length(const std::string& headers)
{
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos) {
            end = headers.size();
        }
        std::string line = headers.substr(pos, end - pos);
        pos = (end == headers.size()) ? end : end + 2;
        if (line.size() >= 15) {
            std::string lower = line;
            for (char& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (lower.rfind("content-length:", 0) == 0) {
                return std::stol(line.substr(line.find(':') + 1));
            }
        }
    }
    return -1;
}

bool read_response(int sock, std::string& out)
{
    out.clear();
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        out.append(buf, static_cast<size_t>(n));
    }

    const size_t header_end = out.find("\r\n\r\n");
    long content_length = parse_content_length(out.substr(0, header_end));
    if (content_length < 0) {
        return false;
    }

    const size_t body_start = header_end + 4;
    while (static_cast<long>(out.size() - body_start) < content_length) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    return true;
}

bool wait_for_server()
{
    for (int i = 0; i < 20; ++i) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool ok = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        close(sock);
        if (ok) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

bool run_crash_test()
{
    int sock = connect_server();
    if (sock < 0) {
        return false;
    }

    // 发送一个格式异常的请求，测试服务器应当稳定处理并不崩溃
    const std::string bad_request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"
        "Content-Length: abc\r\n"
        "\r\n";

    if (send(sock, bad_request.data(), bad_request.size(), 0) < 0) {
        std::perror("send");
        close(sock);
        return false;
    }
    close(sock);

    // 等待一点时间，给服务器处理异常请求的机会
    usleep(100000);

    // 确保服务器进程还活着
    if (kill(g_server_pid, 0) != 0 && errno == ESRCH) {
        return false;
    }

    // 再发送一个正常请求，确认服务器继续工作
    sock = connect_server();
    if (sock < 0) {
        return false;
    }

    const std::string good_request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (send(sock, good_request.data(), good_request.size(), 0) < 0) {
        std::perror("send");
        close(sock);
        return false;
    }

    std::string response;
    bool ok = read_response(sock, response);
    close(sock);
    if (!ok) {
        return false;
    }
    return response.find("HTTP/1.1 200") != std::string::npos;
}

int main()
{
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        return 1;
    }
    if (pid == 0) {
        char exe_path[PATH_MAX] = {0};
        if (readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1) <= 0) {
            std::perror("readlink");
            _exit(1);
        }

        std::string exe_dir = exe_path;
        size_t pos = exe_dir.rfind('/');
        if (pos != std::string::npos) {
            exe_dir.resize(pos);
        }

        std::string server_path = exe_dir + "/simple_http_server";
        std::string www_path = exe_dir + "/../www";

        execl(server_path.c_str(), "simple_http_server", "8083",
              www_path.c_str(), nullptr);
        std::perror("execl");
        _exit(1);
    }
    g_server_pid = pid;

    if (!wait_for_server()) {
        std::cerr << "Server did not start" << std::endl;
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return 1;
    }

    bool ok = run_crash_test();
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);

    if (!ok) {
        std::cerr << "Crash resilience test failed" << std::endl;
        return 1;
    }

    std::cout << "Crash resilience test passed" << std::endl;
    return 0;
}
