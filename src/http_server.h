#pragma once

#include <string>
#include <unordered_map>

class HttpServer {
public:
    HttpServer(int port, const std::string& doc_root);
    ~HttpServer();
    int run();

private:
    int create_listen_socket();
    bool set_nonblocking(int fd);
    void handle_events();
    void accept_connection();
    void handle_client(int client_fd);
    std::string build_response(const std::string& body, const std::string& content_type, int status_code);
    std::string parse_request(int client_fd);
    std::string serve_request(const std::string& request);
    std::string load_file(const std::string& path);

    int port_;
    std::string doc_root_;
    int listen_fd_;
    int epoll_fd_;
    std::unordered_map<int, std::string> client_buffers_;
};


