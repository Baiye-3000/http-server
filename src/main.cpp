#include<http_server.h>
#include <iostream>

int main(int argc, char* argv[]) {
    int port=8080;
    std::string doc_root="../www";
    if(argc>1){
        port=std::stoi(argv[1]);
    }
    if(argc>2){
        doc_root=argv[2];
    }
    HttpServer server(port, doc_root);
    return server.run();
}
//./server 8081  ->argc=2
//./server 8081 ./www->argc=3