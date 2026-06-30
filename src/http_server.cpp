#include "http_server.h"
#include "html_parser.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unordered_map>
#include <vector>

// HttpServer 构造函数：初始化端口和文档根目录
HttpServer::HttpServer(int port, const std::string& doc_root)
    : port_(port), doc_root_(doc_root), listen_fd_(-1), epoll_fd_(-1)
{}

// HttpServer 析构函数：关闭监听套接字和 epoll 实例
HttpServer::~HttpServer()
{
    if (listen_fd_ != -1) {
        close(listen_fd_);
    }
    if (epoll_fd_ != -1) {
        close(epoll_fd_);
    }
}
/*要好好理解*/
// create_listen_socket: 创建 TCP 监听套接字，并设置为非阻塞
// 返回值：成功时返回监听套接字 fd，失败时返回 -1
int HttpServer::create_listen_socket()
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return -1;
    }
    if (set_nonblocking(listen_fd_) == -1) {
        std::cerr << "Failed to set non-blocking: " << strerror(errno) << std::endl;
        close(listen_fd_);
        return -1;
    }

    // 绑定到指定端口，监听所有网卡地址
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    // SO_REUSEADDR 允许重启服务器时快速重用端口
    int opt = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::cerr << "Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
        close(listen_fd_);
        return -1;
    }

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
        std::cerr << "Failed to bind: " << strerror(errno) << std::endl;
        close(listen_fd_);
        return -1;
    }

    if (listen(listen_fd_, SOMAXCONN) == -1) {
        std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
        close(listen_fd_);
        return -1;
    }
    return listen_fd_;
}

// run: 启动服务器，创建 epoll 实例并进入事件循环
int HttpServer::run()
{
    listen_fd_ = create_listen_socket();
    if (listen_fd_ == -1) {
        return 1;
    }

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) {
        std::cerr << "Failed to create epoll instance: " << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        return 1;
    }

    // 将监听套接字加入 epoll，等待新的连接到来
    epoll_event ev{};
    ev.events = EPOLLIN;//监听可读事件
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) == -1) {
        std::cerr << "Failed to add listen fd to epoll: " << strerror(errno) << std::endl;
        close(listen_fd_);
        close(epoll_fd_);
        listen_fd_ = -1;
        epoll_fd_ = -1;
        return 1;
    }

    handle_events();
    return 0;
}

// set_nonblocking: 将 fd 设置为非阻塞模式，适用于 epoll-edge-triggered
bool HttpServer::set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0); // 获取当前文件状态标志
    if (flags == -1) {
        std::cerr << "Failed to get file flags: " << strerror(errno) << std::endl;
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        std::cerr << "Failed to set non-blocking: " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

// handle_events: 主事件循环，通过 epoll_wait 等待 I/O 事件
void HttpServer::handle_events()
{
    const int MAX_EVENTS = 16;
    std::vector<epoll_event> events(MAX_EVENTS);
    while (true) {
        int n = epoll_wait(epoll_fd_, events.data(), MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd_) {
                // 监听套接字有新连接到来
                accept_connection();
            } else if (events[i].events & EPOLLIN) {
                // 客户端套接字可读，处理请求
                handle_client(fd);
            } else {
                std::cerr << "Unexpected event for fd " << fd << std::endl;
                close(fd);
                client_buffers_.erase(fd);
            }
        }
    }
    
}

// accept_connection: 接收所有挂起的新客户端连接，并将它们加入 epoll 监控
void HttpServer::accept_connection()
{
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        // 一个 listen_fd_ 对应多个 client_fd，每次 accept 都返回一个新的客户端套接字
        if ( client_fd == -1 ) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 没有更多连接可接受
            }
            std::cerr << "Accept errror: " << strerror(errno) << std::endl;
            break;
        }

        if (!set_nonblocking(client_fd)) {
            close(client_fd);
            continue;
        }
        epoll_event event{};
        event.data.fd = client_fd;
        event.events = EPOLLIN | EPOLLET;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) == -1) {
            std::cerr << "Failed to add client fd to epoll: " << strerror(errno) << std::endl;
            close(client_fd);
        }
        client_buffers_[client_fd] = ""; // 初始化连接缓冲区
    }
    
}

// handle_client: 读取请求、生成响应并关闭连接
void HttpServer::handle_client(int client_fd)
{
    std::string request = parse_request(client_fd);
    if (request.empty()) {
        // 没有完整请求或者连接已关闭，清理连接状态
        
            client_buffers_.erase(client_fd);
        
        close(client_fd);
        return;
    }

    std::string response = serve_request(request);
    ssize_t total_sent = 0;
    while (total_sent < static_cast<ssize_t>(response.size())) {
        ssize_t sent = send(client_fd, response.data() + total_sent, response.size() - total_sent, 0);
        total_sent += sent;
        if (sent == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; // 发送缓冲区已满，继续重试
            }
            std::cerr << "Send error: " << strerror(errno) << std::endl;
            break;
        }
    }

    // 从 epoll 中删除连接，并关闭 fd
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    client_buffers_.erase(client_fd);
    close(client_fd);
    // 不关闭连接
// 继续监听 EPOLLIN


}

// parse_request: 从客户端缓冲区读取完整 HTTP 请求头，支持分片接收
std::string HttpServer::parse_request(int client_fd)
{
    char buffer[4096];
    std::string& request_buffer = client_buffers_[client_fd];

    while (true) {
        ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            request_buffer.append(buffer, bytes_read);
            if (request_buffer.find("\r\n\r\n") != std::string::npos) {
                break; // 已经读到完整的请求头
            }
            continue;
        }

        if (bytes_read == 0) {
            return ""; // 客户端关闭连接
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break; // 当前没有更多数据可读
        }

        perror("recv: CONNECTION ERROR ");
        return "";
    }

    size_t end_of_headers = request_buffer.find("\r\n\r\n");
    if (end_of_headers == std::string::npos) {
        return "";
    }

    std::string request = request_buffer.substr(0, 4 + end_of_headers);
    request_buffer.erase(0, 4 + end_of_headers);
    return request;
}

// load_file: 从文档根目录读取文件内容，失败返回空字符串
std::string HttpServer::load_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs) return std::string();
    std::string content;
    ifs.seekg(0, std::ios::end);
    std::streamsize size = ifs.tellg();
    if (size <= 0) return std::string();
    content.resize(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    ifs.read(&content[0], size);
    return content;
}
/*load_file为AI生成*/
// get_content_type: 根据文件后缀返回 MIME 类型
static std::string get_content_type(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size()-5) == ".html") return "text/html";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".css") return "text/css";
    if (path.size() >= 3 && path.substr(path.size()-3) == ".js") return "application/javascript";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".png") return "image/png";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".jpg") return "image/jpeg";
    return "text/plain";
}

// build_response: 根据状态码、内容类型和响应体构建完整 HTTP 响应字符串
std::string HttpServer::build_response(const std::string& body, const std::string& content_type, int status_code) {
    const char* status = (status_code == 200) ? "200 OK" : "404 Not Found";//字符串常量更轻量，避免了每次调用函数都构造和析构一个 std::string 对象
    std::string resp;
    //提前赋予容量，减少扩容 
    resp.reserve(body.size()+128);
    /*resp +=("HTTP/1.1 " + status + "\r\n");
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Content-Type: " + content_type + "\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;*/
    //  避免了+拼接字符串，需要构造临时对象str = str + "abc" 这样的写法通常会创建临时对象，并可能重新分配内存和复制已有内容。如果在循环中频繁使用，性能较差。+=直接在原本的缓冲区写，根本没有新对象，整体复制，
    resp += "HTTP/1.1 ";
    resp += status;
    resp += "\r\n";

    resp += "Content-Length: ";
    resp += std::to_string(body.size());
    resp += "\r\n";

    resp += "Content-Type: ";
    resp += content_type;
    resp += "\r\n";

    resp += "Connection: close\r\n";
    resp += "\r\n";

    resp += body;

    return resp;
}

// serve_request: 解析请求并生成 HTTP 响应
std::string HttpServer::serve_request(const std::string& request) {
    // parse request line: METHOD PATH HTTP/1.1
    std::istringstream iss(request);
    std::string method, path, version;
    if (!(iss >> method >> path >> version)) {
        return build_response("Bad Request", "text/plain", 400);
    }
    if (method != "GET") {
        return build_response("Method Not Allowed", "text/plain", 405);
    }
    if (path == "/") {
        path = "/index.html";
    }
    std::string fullpath = doc_root_ + path;
    std::string body = load_file(fullpath);
    if (body.empty()) {
        return build_response("Not Found", "text/plain", 404);
    }
    std::string ctype = get_content_type(fullpath);
    if (ctype == "text/html") {
        std::string json_body = html_to_json(body);
        return build_response(json_body, "application/json", 200);
    }//后续扩展，解析png,css等
    return build_response(body, ctype, 200);
}
//并发测试，性能测试，两个请求过来，返回的是同一个吗
//close fd 有影响吗
//用成熟的库
//string的性能
//Linux命令
//为什么设置为nonblocking
//负载均衡
