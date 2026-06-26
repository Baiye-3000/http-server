#include "http_server.h"
#include <iostream>
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
HttpServer::HttpServer(int port, const std::string& doc_root)
    : port_(port), doc_root_(doc_root), listen_fd_(-1), epoll_fd_(-1)
{}

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

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
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
//
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

    epoll_event ev{};
    ev.events = EPOLLIN;
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

bool HttpServer::set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0); // 获取文件状态标志
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
                accept_connection();
            } else if (events[i].events & EPOLLIN) {
                handle_client(fd);
            } else {
                std::cerr << "Unexpected event for fd " << fd << std::endl;
                close(fd);
                client_buffers_.erase(fd);
            }
        }
    }
}
// 处理客户端连接!!!
void HttpServer::accept_connection()
{
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        //一个listen_id对应多个client_id, listen_id 只是负责监听，真正由client_id负责真是客户端连接 。 每次 accept() 都会返回一个新的 client_fd：
        if ( client_fd == -1 ) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // No more connections to accept
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
        client_buffers_[client_fd] = ""; // Initialize buffer for this client
    }
}
void HttpServer::handle_client(int client_fd)
{
    std::string request=parse_request(client_fd);
    if(request.empty())
    {
        if(client_buffers_.count(client_fd))
        {
            client_buffers_.erase(client_fd);
        }
        close(client_fd);
        client_buffers_.erase(client_fd);
        return ;
    }
    std::string response=process_request(request);
    ssize_t total_sent=0;
    while(total_sent<response.size())
    {
        //
        ssize_t sent=send(client_fd , response.data()+total_sent , response.size()-total_sent , 0);
        total_sent+=sent;
        if(sent==-1)
        {
            if(errno==EAGAIN||errno==EWOULDBLOCK)
            {
                continue;
            }
            std::cerr<<"Send error: "<<strerror(errno)<<std::endl;
            break;
        }
   `}
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    client_buffers_.erase(client_fd);
    close(client_fd);
    //顺序还很重要,epoll_ctl删除后再close,否则可能会出现fd被复用的情况,或者是epoll_ctl操作不可用client_id
}
