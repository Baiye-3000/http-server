#include "http_server.h"
#include "html_parser.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <cerrno>

HttpServer::HttpServer(int port, const std::string& doc_root)
    : port_(port), doc_root_(doc_root), listen_fd_(-1), epoll_fd_(-1) {}

HttpServer::~HttpServer() {
    if (listen_fd_ >= 0) {
        close(listen_fd_);
    }
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

int HttpServer::run() {
    listen_fd_ = create_listen_socket();
    if (listen_fd_ < 0) {
        return -1;
    }

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        perror("epoll_create1");
        close(listen_fd_);
        return -1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        perror("epoll_ctl: listen_fd");
        return -1;
    }

    std::cout << "HTTP server running on port " << port_ << "\n";
    handle_events();
    return 0;
}

int HttpServer::create_listen_socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    if (!set_nonblocking(fd)) {
        close(fd);
        return -1;
    }

    return fd;
}

bool HttpServer::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
        return false;
    }
    return true;
}

void HttpServer::handle_events() {
    const int MAX_EVENTS = 16;
    std::vector<epoll_event> events(MAX_EVENTS);

    while (true) {
        int n = epoll_wait(epoll_fd_, events.data(), MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd_) {
                accept_connection();
            } else if (events[i].events & EPOLLIN) {
                handle_client(fd);
            }
        }
    }
}

void HttpServer::accept_connection() {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("accept");
            break;
        }

        if (!set_nonblocking(client_fd)) {
            close(client_fd);
            continue;
        }

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = client_fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            perror("epoll_ctl: client_fd");
            close(client_fd);
            continue;
        }

        client_buffers_[client_fd] = "";
    }
}

std::string HttpServer::parse_request(int client_fd) {
    char buffer[4096];
    std::string& request_buffer = client_buffers_[client_fd];

    while (true) {
        ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            request_buffer.append(buffer, bytes_read);
            if (request_buffer.find("\r\n\r\n") != std::string::npos) {
                break;
            }
            continue;
        }

        if (bytes_read == 0) {
            return "";
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        perror("recv");
        return "";
    }

    size_t end_of_headers = request_buffer.find("\r\n\r\n");
    if (end_of_headers == std::string::npos) {
        return "";
    }

    std::string request = request_buffer.substr(0, end_of_headers + 4);
    request_buffer.clear();
    return request;
}

std::string HttpServer::serve_request(const std::string& request) {
    std::istringstream stream(request);
    std::string method;
    std::string uri;
    std::string version;
    stream >> method >> uri >> version;
    if (method != "GET") {
        return build_response("{\"error\": \"Method not allowed\"}", "application/json", 405);
    }

    if (uri.empty() || uri[0] != '/') {
        return build_response("{\"error\": \"Bad request\"}", "application/json", 400);
    }

    std::string path = uri;
    if (path == "/") {
        path = "/index.html";
    }

    std::string full_path = doc_root_ + path;
    std::string body = load_file(full_path);
    if (body.empty()) {
        return build_response("{\"error\": \"Not found\"}", "application/json", 404);
    }

    std::string json_body = html_to_json(body);
    return build_response(json_body, "application/json", 200);
}

std::string HttpServer::load_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string HttpServer::build_response(const std::string& body, const std::string& content_type, int status_code) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << " ";
    if (status_code == 200) response << "OK";
    else if (status_code == 400) response << "Bad Request";
    else if (status_code == 404) response << "Not Found";
    else if (status_code == 405) response << "Method Not Allowed";
    else response << "Internal Server Error";

    response << "\r\nContent-Type: " << content_type << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n\r\n";
    response << body;
    return response.str();
}

void HttpServer::handle_client(int client_fd) {
    std::string request = parse_request(client_fd);
    if (request.empty()) {
        if (client_buffers_.count(client_fd) == 0) {
            close(client_fd);
        }
        return;
    }

    std::string response = serve_request(request);
    ssize_t total_sent = 0;
    while (total_sent < static_cast<ssize_t>(response.size())) {
        ssize_t sent = send(client_fd, response.data() + total_sent, response.size() - total_sent, 0);
        if (sent <= 0) {
            break;
        }
        total_sent += sent;
    }

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    client_buffers_.erase(client_fd);
    close(client_fd);
}
