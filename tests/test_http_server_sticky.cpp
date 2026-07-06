#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const int kPort = 8083;

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

bool run_sticky_test()
{
    int sock = connect_server();
    if (sock < 0) {
        return false;
    }

    const std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "GET /style.css HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    ssize_t sent = send(sock, request.data(), request.size(), 0);
    if (sent != static_cast<ssize_t>(request.size())) {
        std::perror("send");
        close(sock);
        return false;
    }

    std::string first_response;
    if (!read_response(sock, first_response)) {
        close(sock);
        return false;
    }
    assert(first_response.find("HTTP/1.1 200") != std::string::npos);
    assert(first_response.find("Content-Type: text/html") != std::string::npos ||
           first_response.find("Content-Type: application/json") != std::string::npos);

    std::string second_response;
    if (!read_response(sock, second_response)) {
        close(sock);
        return false;
    }
    assert(second_response.find("HTTP/1.1 200") != std::string::npos);
    assert(second_response.find("Content-Type: text/css") != std::string::npos);

    close(sock);
    return true;
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

    if (!wait_for_server()) {
        std::cerr << "Server did not start" << std::endl;
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return 1;
    }

    bool ok = run_sticky_test();
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);

    if (!ok) {
        std::cerr << "Sticky packet test failed" << std::endl;
        return 1;
    }

    std::cout << "Sticky packet test passed" << std::endl;
    return 0;
}
