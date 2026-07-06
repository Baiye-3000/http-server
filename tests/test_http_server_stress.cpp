#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kPort = 8082;
constexpr int kDefaultIterations = 3000;
constexpr int kAliveCheckInterval = 100;
constexpr int kKeepAliveInterval = 50;
constexpr int kStatsInterval = 1000;
constexpr long kLeakThresholdKb = 5120;  // 5 MB

std::atomic<bool> g_stop{false};

void on_sigint(int)
{
    g_stop = true;
}

void cleanup_server(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
    }
}

const char* signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    case SIGTERM: return "SIGTERM";
    case SIGKILL: return "SIGKILL";
    default:      return strsignal(sig);
    }
}

long read_vmrss_kb(pid_t pid)
{
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream ifs(path);
    if (!ifs) {
        return -1;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            if (std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb) == 1) {
                return kb;
            }
            return -1;
        }
    }
    return -1;
}

bool check_server_alive(pid_t pid)
{
    int status = 0;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == 0) {
        return true;
    }
    if (result < 0) {
        std::perror("waitpid");
        return false;
    }

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        std::cerr << "Server process died from signal " << sig
                  << " (" << signal_name(sig) << ")\n";
    } else if (WIFEXITED(status)) {
        std::cerr << "Server process exited with code "
                  << WEXITSTATUS(status) << "\n";
    } else {
        std::cerr << "Server process terminated unexpectedly\n";
    }
    return false;
}

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
        size_t line_end = headers.find("\r\n", pos);
        const size_t len = (line_end == std::string::npos)
                               ? headers.size() - pos
                               : line_end - pos;
        if (len == 0) {
            break;
        }
        std::string line = headers.substr(pos, len);
        pos = (line_end == std::string::npos) ? headers.size() : line_end + 2;

        std::string lower = line;
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lower.rfind("content-length:", 0) == 0) {
            return std::stol(line.substr(line.find(':') + 1));
        }
    }
    return -1;
}

bool read_one_response(int sock)
{
    std::string data;
    char buf[4096];

    while (data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        data.append(buf, static_cast<size_t>(n));
    }

    size_t hdr_end = data.find("\r\n\r\n");
    long content_length = parse_content_length(data.substr(0, hdr_end));
    if (content_length < 0) {
        return false;
    }

    size_t body_start = hdr_end + 4;
    while (data.size() - body_start < static_cast<size_t>(content_length)) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        data.append(buf, static_cast<size_t>(n));
    }

    return data.find("HTTP/1.1 200") != std::string::npos ||
           data.find("HTTP/1.0 200") != std::string::npos;
}

bool request_close_connection()
{
    int sock = connect_server();
    if (sock < 0) {
        return false;
    }

    const char* req =
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (send(sock, req, std::strlen(req), 0) < 0) {
        std::perror("send");
        close(sock);
        return false;
    }

    char buf[4096];
    ssize_t total = 0;
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            total += n;
            continue;
        }
        if (n == 0) {
            close(sock);
            return total > 0;
        }
        std::perror("recv");
        close(sock);
        return false;
    }
}

bool request_keep_alive_burst()
{
    int sock = connect_server();
    if (sock < 0) {
        return false;
    }

    const char* paths[] = {"/", "/style.css", "/script.js"};
    for (const char* path : paths) {
        std::string req = std::string("GET ") + path +
                          " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        if (send(sock, req.data(), req.size(), 0) < 0) {
            std::perror("send");
            close(sock);
            return false;
        }
        if (!read_one_response(sock)) {
            std::cerr << "keep-alive response failed for " << path << "\n";
            close(sock);
            return false;
        }
    }

    close(sock);
    return true;
}

}  // namespace

int run_stress_loop(pid_t pid, long rss_before, bool forever, int max_iterations)
{
    auto start = std::chrono::steady_clock::now();
    int i = 0;

    while (!g_stop) {
        ++i;
        if (!forever && i > max_iterations) {
            break;
        }

        if (i % kAliveCheckInterval == 0) {
            if (!check_server_alive(pid)) {
                std::cerr << "Server crashed during iteration " << i << "\n";
                cleanup_server(pid);
                return 1;
            }
        }

        bool ok = false;
        if (i % kKeepAliveInterval == 0) {
            ok = request_keep_alive_burst();
        } else {
            ok = request_close_connection();
        }

        if (!ok) {
            std::cerr << "Request failed at iteration " << i << "\n";
            if (!check_server_alive(pid)) {
                std::cerr << "Server also appears dead\n";
            }
            cleanup_server(pid);
            return 1;
        }

        if (forever && i % kStatsInterval == 0) {
            long rss = read_vmrss_kb(pid);
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start);
            double qps = elapsed.count() > 0
                               ? static_cast<double>(i) / elapsed.count()
                               : 0.0;
            std::cout << "[iter " << i << "] server pid=" << pid
                      << " VmRSS=" << rss << " kB (delta "
                      << (rss - rss_before) << " kB)"
                      << " qps=" << std::fixed << std::setprecision(0) << qps
                      << "  (Ctrl+C to stop)\n";
        }
    }

    if (!check_server_alive(pid)) {
        std::cerr << "Server crashed after stress loop\n";
        cleanup_server(pid);
        return 1;
    }

    long rss_after = read_vmrss_kb(pid);
    if (rss_after < 0) {
        std::cerr << "Failed to read final VmRSS\n";
        cleanup_server(pid);
        return 1;
    }

    long delta_kb = rss_after - rss_before;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start);
    std::cout << "VmRSS before: " << rss_before << " kB, after: "
              << rss_after << " kB, delta: " << delta_kb << " kB\n";
    std::cout << "Total iterations: " << i << ", elapsed: "
              << elapsed.count() << "s\n";

    cleanup_server(pid);

    if (!forever && delta_kb > kLeakThresholdKb) {
        std::cerr << "Suspected memory leak: VmRSS grew by " << delta_kb
                  << " kB (threshold " << kLeakThresholdKb << " kB)\n";
        return 1;
    }

    if (forever) {
        std::cout << "http_server stress loop stopped by user\n";
    } else {
        std::cout << "http_server stress test passed (" << i
                  << " iterations)" << std::endl;
    }
    return 0;
}

int main(int argc, char* argv[])
{
    bool forever = false;
    for (int arg = 1; arg < argc; ++arg) {
        if (std::strcmp(argv[arg], "--forever") == 0) {
            forever = true;
        } else {
            std::cerr << "Usage: " << argv[0] << " [--forever]\n";
            return 1;
        }
    }

    if (forever) {
        std::signal(SIGINT, on_sigint);
    }
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

        execl(server_path.c_str(), "simple_http_server",
              "8082", www_path.c_str(), nullptr);
        std::perror("execl");
        _exit(1);
    }

    sleep(1);

    if (!check_server_alive(pid)) {
        std::cerr << "Server failed to start\n";
        cleanup_server(pid);
        return 1;
    }

    long rss_before = read_vmrss_kb(pid);
    if (rss_before < 0) {
        std::cerr << "Failed to read initial VmRSS\n";
        cleanup_server(pid);
        return 1;
    }

    if (forever) {
        std::cout << "Running forever — server pid=" << pid
                  << " VmRSS=" << rss_before << " kB\n";
        std::cout << "Watch CPU: top -p " << pid << "\n";
    }

    return run_stress_loop(pid, rss_before, forever, kDefaultIterations);
}
