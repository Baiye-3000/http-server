#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

struct Connection {
    std::string read_buffer;
    std::string write_buffer;
    bool keep_alive = true;
    bool closed = false;
};
// 简单 HTTP 服务器类，基于 epoll 监听并发连接
class HttpServer {
public:
    // port: 监听端口，doc_root: 静态文件根目录
    HttpServer(int port, const std::string& doc_root);
    ~HttpServer();

    // 启动服务器并进入事件循环
    int run();

private:
    int create_listen_socket();
    bool set_nonblocking(int fd);
    void handle_events();
    void accept_connection();
    void handle_client(int client_fd);
    void close_connection(int client_fd);
    void mod_client_events(int client_fd, uint32_t events);
    bool flush_write_buffer(int client_fd);
    bool parse_keep_alive(const std::string& request);

    // 构造响应、解析请求、服务请求、加载文件
    std::string build_response(const std::string& body, const std::string& content_type, int status_code, bool keep_alive);
    std::string parse_request(int client_fd);
    std::string serve_request(const std::string& request, bool keep_alive);
    std::string load_file(const std::string& path);

    int port_;
    std::string doc_root_;
    int listen_fd_;
    int epoll_fd_;
    std::unordered_map<int, Connection> client_conns_;
};

