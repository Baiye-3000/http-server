# 简单 HTTP 服务器开发

> **开发前必读：[docs/HTTP-SERVER.md](docs/HTTP-SERVER.md)**（架构、原则、测试、经验沉淀）

这是一个基于 C++ 和 Linux epoll 的简单 HTTP 服务器项目。

## 目标

- 使用 Linux 内核并基于 `epoll` 实现并发连接
- 读取 HTML 文件并将文本内容转换为 JSON 输出
- 使用 CMake 构建项目

## 本地开发流程

### 1. 在 VS Code 中打开项目

直接在 VS Code 里打开工作区目录 `e:\HTML网页服务器搭建`。

### 2. 代码编辑

需要关注的文件：

- `CMakeLists.txt`
- `src/main.cpp`
- `src/http_server.h`
- `src/http_server.cpp`
- `src/html_parser.h`
- `src/html_parser.cpp`
- `www/index.html`

### 3. 运行环境

此项目依赖 Linux 的 `epoll`，所以建议使用以下之一：

- VS Code Remote-SSH 连接远程 CentOS 8 服务器进行编译运行
- WSL (Windows Subsystem for Linux) 在本地运行 Linux 环境

如果你在 Windows 本地直接编辑，仍然可以在 VS Code 里写代码，最后把代码部署到服务器上测试。

### 4. 编译步骤（Linux / WSL / 远程服务器）

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

### 5. 运行服务器

```bash
./simple_http_server 8080 ../www
```

### 6. 测试接口

```bash
curl -v http://127.0.0.1:8080/
```

如果你在远程服务器上运行，则改为：

```bash
curl -v http://192.168.61.217:8080/
```

## 关键点说明

> 完整手册（开发前必读）：[docs/HTTP-SERVER.md](docs/HTTP-SERVER.md)

### epoll

代码使用一个全局 `epoll_fd` 来管理所有连接，避免为每个客户端都创建新的 epoll 实例。

### HTML -> JSON

通过简单的文本提取函数去掉 HTML 标签，并将网页内容组合成一个 JSON 字符串返回。

### 结构建议

- `src/main.cpp`：程序入口
- `src/http_server.cpp`：网络、epoll、请求解析、响应生成
- `src/html_parser.cpp`：HTML 转 JSON

## 后续改进方向

- 增加更完整的 HTTP 请求解析
- 支持多种 HTML 标签的抽取
- 增加文件缓存和 MIME 类型支持
- 实现简单负载均衡（例如 Nginx upstream）
