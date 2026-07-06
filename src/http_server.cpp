#include "http_server.h"
#include "html_parser.h"
#include <boost/asio/buffer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
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
#include <sys/stat.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cctype>

namespace beast = boost::beast;
namespace http = beast::http;
namespace {

class StringWriteStream {
public:
    explicit StringWriteStream(std::string& out) : out_(out) {}

    template<class ConstBufferSequence>
    std::size_t write_some(ConstBufferSequence const& buffers) {
        boost::system::error_code ec;
        return write_some(buffers, ec);
    }

    template<class ConstBufferSequence>
    std::size_t write_some(ConstBufferSequence const& buffers, boost::system::error_code& ec) {
        ec.clear();
        std::size_t total = 0;
        for (auto it = boost::asio::buffer_sequence_begin(buffers);
             it != boost::asio::buffer_sequence_end(buffers); ++it) {
            const auto buffer = *it;
            out_.append(static_cast<const char*>(buffer.data()), buffer.size());
            total += buffer.size();
        }
        return total;
    }

private:
    std::string& out_;
};

}  // namespace

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
    // SO_REUSEPORT 允许多进程 bind 同一端口，由内核分发 accept
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        std::cerr << "Failed to set SO_REUSEPORT: " << strerror(errno) << std::endl;
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
                handle_client(fd);
            } else if (events[i].events & EPOLLOUT) {
                handle_client(fd);
            } else {
                std::cerr << "Unexpected event for fd " << fd << std::endl;
                close_connection(fd);
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
        client_conns_.try_emplace(client_fd);//添加客户端连接
    }
}

void HttpServer::close_connection(int client_fd)
{
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    client_conns_.erase(client_fd);
    close(client_fd);
}
//mod_client_events: 修改客户端事件，设置为边缘触发模式
void HttpServer::mod_client_events(int client_fd, uint32_t events)
{
    epoll_event event{};
    event.data.fd = client_fd;
    event.events = events | EPOLLET;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client_fd, &event);
}

bool HttpServer::flush_write_buffer(int client_fd)
{
    if (!client_conns_.contains(client_fd)) {
        return true;
    }

    std::string& write_buffer = client_conns_[client_fd].write_buffer;
    while (!write_buffer.empty()) {
        ssize_t sent = send(client_fd, write_buffer.data(), write_buffer.size(), 0);
        if (sent > 0) {
            write_buffer.erase(0, static_cast<size_t>(sent));
            //内核发送缓冲区空间有限，非阻塞模式下可能只收一部分，删掉已发，继续发剩下的。
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;  // 非阻塞正常情况：发送缓冲区满，等 EPOLLOUT 再发。资源繁忙，请重试
        }
        std::cerr << "Send error: " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

// handle_client: 读取请求、生成响应；长连接下同一 fd 可处理多个请求
void HttpServer::handle_client(int client_fd)
{
    if(!client_conns_.contains(client_fd)) {
        return;
    }
    Connection& conn = client_conns_[client_fd];
    //如果发送缓冲区有数据，先发完。因为是上一个连接未发完的数据
    if (!conn.write_buffer.empty()) {
        if (!flush_write_buffer(client_fd)) {
            mod_client_events(client_fd, EPOLLIN | EPOLLOUT);//监听客户端发请求和发送数据
            return;//返回，等待下一次事件到来。避免一直while等send成功，导致cpu占用过高。
        }
        mod_client_events(client_fd, EPOLLIN);//监听客户端发请求
//那就不需要继续监听 EPOLLOUT。
//因为 socket 大部分时间都是“可写”的，如果一直监听 EPOLLOUT，epoll 会频繁通知你，浪费 CPU。
        if (!conn.keep_alive) {//这就是那个定义结构体的优点，可以方便的判断是否是长连接
            close_connection(client_fd);
            return;
        }
    }

    while (true) {
        std::optional<HttpRequest> request = parse_request(client_fd);
        if (!request) {
            if (conn.closed) {
                close_connection(client_fd);
                return;
            }
            break;
        }

        conn.keep_alive = request->keep_alive();
        std::string response = serve_request(*request, conn.keep_alive);
        conn.write_buffer += response;

        if (!flush_write_buffer(client_fd)) {
            mod_client_events(client_fd, EPOLLIN | EPOLLOUT);
            return;
        }

        if (!conn.keep_alive) {
            close_connection(client_fd);
            return;
        }

        if (conn.read_buffer.size() == 0) {//如果读缓冲区为空，则退出循环
            break;
        }
    }
}

// parse_request: Boost.Beast 解析 HTTP 请求头，支持分片与 keep-alive 后续请求
std::optional<HttpServer::HttpRequest> HttpServer::parse_request(int client_fd)
{
    if (!client_conns_.contains(client_fd)) {
        return std::nullopt;
    }
    Connection& conn = client_conns_[client_fd];
    if (!conn.parser) {//如果解析器不存在，创建一个
        conn.parser.emplace();
    }

    while (true) {
        if (conn.read_buffer.size() > 0) {
            boost::beast::error_code ec;
            const std::size_t bytes_used = conn.parser->put(conn.read_buffer.data(), ec);//将读缓冲区数据放入解析器
            if (ec) {
                conn.closed = true;
                return std::nullopt;
            }
            conn.read_buffer.consume(bytes_used);//消费掉已解析的数据
            if (conn.parser->is_done()) {
                HttpRequest request = conn.parser->release();// 释放解析器，返回解析好的请求
                conn.parser.emplace();//重新创建解析器
                return request;//返回解析好的请求
                //处理长连接，keep-alive为true，则继续解析下一个请求
            }

        }

        char buffer[4096];
        const ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            const auto out = conn.read_buffer.prepare(static_cast<std::size_t>(bytes_read));
            boost::asio::buffer_copy(out, boost::asio::buffer(buffer, static_cast<std::size_t>(bytes_read)));
            conn.read_buffer.commit(static_cast<std::size_t>(bytes_read));
            continue;
        }

        if (bytes_read == 0) {
            conn.closed = true;
            return std::nullopt;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }

        perror("recv: CONNECTION ERROR ");
        conn.closed = true;
        return std::nullopt;
    }
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

// build_response: Boost.Beast 序列化 HTTP 响应
std::string HttpServer::build_response(const std::string& body, const std::string& content_type, int status_code, bool keep_alive) {
    http::response<http::string_body> res;
    switch (status_code) {
        case 200: res.result(http::status::ok); break;
        case 400: res.result(http::status::bad_request); break;
        case 405: res.result(http::status::method_not_allowed); break;
        default: res.result(http::status::not_found); break;
    } 
    res.version(11);
    res.set(http::field::content_type, content_type);
    res.keep_alive(keep_alive);
    res.body() = body;
    res.prepare_payload();

    std::string serialized;
    serialized.reserve(body.size() + 128);
    StringWriteStream stream(serialized);
    http::response_serializer<http::string_body> sr{res};//序列化响应，创建序列化器
    while (!sr.is_done()) {
        http::write_some(stream, sr);//写入响应
    }
    return serialized;
}

// serve_request: 解析请求并生成 HTTP 响应
std::string HttpServer::serve_request(const HttpRequest& request, bool keep_alive) {
    if (request.method() != http::verb::get) {
        return build_response("Method Not Allowed", "text/plain", 405, keep_alive);
    }

    std::string path = std::string(request.target());
    if (path == "/") {
        path = "/index.html";
    }
    std::string fullpath = doc_root_ + path;
    //使用缓存，减少文件读取次数
    std::string ctype = get_content_type(fullpath);
    if (ctype == "text/html") {
        struct stat st {};
        if (stat(fullpath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            return build_response("Not Found", "text/plain", 404, keep_alive);
        }
        if (json_cache_.contains(fullpath)) {
            const JsonCacheEntry& entry = json_cache_[fullpath];
            if (entry.mtime == st.st_mtime) {
                return build_response(entry.json_body, "application/json", 200, keep_alive);
            }
        }
        std::string body = load_file(fullpath);
        if (body.empty()) {
            return build_response("Not Found", "text/plain", 404, keep_alive);
        }
        JsonCacheEntry entry;
        entry.json_body = html_to_json(body);
        entry.mtime = st.st_mtime;
        json_cache_[fullpath] = std::move(entry);
        return build_response(json_cache_[fullpath].json_body, "application/json", 200, keep_alive);
    }
    std::string body = load_file(fullpath);
    if (body.empty()) {
        return build_response("Not Found", "text/plain", 404, keep_alive);
    }
    return build_response(body, ctype, 200, keep_alive);
}
//并发测试，性能测试，两个请求过来，返回的是同一个吗
//close fd 有影响吗
//用成熟的库
//string的性能
//Linux命令
//为什么设置为nonblocking
//负载均衡
