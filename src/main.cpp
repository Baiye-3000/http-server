#include <http_server.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string doc_root = "../www";
    int workers = 1;

    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    if (argc > 2) {
        doc_root = argv[2];
    }
    if (argc > 3) {
        workers = std::stoi(argv[3]);
    }
    if (workers < 1) {
        workers = 1;
    }

    if (workers == 1) {
        HttpServer server(port, doc_root);
        return server.run();
    }

    std::cout << "Starting " << workers << " workers on port " << port << std::endl;
    for (int i = 0; i < workers; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            HttpServer server(port, doc_root);
            std::exit(server.run());
        }
        if (pid < 0) {
            std::cerr << "fork failed: " << std::strerror(errno) << std::endl;
            return 1;
        }
    }

    while (wait(nullptr) > 0 || errno == EINTR) {
    }
    return 0;
}
