#include <http_server.h>
#include <iostream>

// main: 程序入口，解析命令行参数并启动 HTTP 服务器
int main(int argc, char* argv[]) {
    int port = 8080;
    std::string doc_root = "../www";

    // 支持通过命令行覆盖端口和文档根目录
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    if (argc > 2) {
        doc_root = argv[2];
    }

    HttpServer server(port, doc_root);
    return server.run();
}
// 使用示例：./simple_http_server 8080 ../www
