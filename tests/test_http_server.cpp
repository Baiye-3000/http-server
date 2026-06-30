#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        return 1;
    }
    if (pid == 0) {
        execl("./simple_http_server", "simple_http_server", "8081", "../www", nullptr);
        std::perror("execl");
        return 1;
    }

    sleep(1);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::perror("socket");
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8081);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect");
        close(sock);
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return 1;
    }

    const char* req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (send(sock, req, strlen(req), 0) < 0) {
        std::perror("send");
        close(sock);
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return 1;
    }

    std::string response;
    char buf[1024];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, static_cast<size_t>(n));
    }
    close(sock);

    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);

    if (response.find("HTTP/1.1 200 OK") == std::string::npos) {
        std::cerr << "Did not receive 200 OK\n";
        return 1;
    }
    if (response.find("Content-Type: application/json") == std::string::npos) {
        std::cerr << "Did not receive JSON content type\n";
        return 1;
    }
    if (response.find("\"text\": \"这是一个测试文件 欢迎使用简单HTTP服务器 这是一个测试文件\"") == std::string::npos) {
        std::cerr << "Response body did not contain expected JSON text\n";
        std::cerr << response << std::endl;
        return 1;
    }

    std::cout << "http_server e2e test passed" << std::endl;
    return 0;
}
