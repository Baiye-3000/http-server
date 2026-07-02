# HTTP-SERVER

> **本项目唯一开发手册 · 开发前必读**  
> 路径：`docs/HTTP-SERVER.md`  
> 任何改动网络、连接、发送、测试相关代码之前，先读本文档第 0 节 checklist，再读对应章节。

---

## 0. 开发前必读（Checklist）

每次开发前先过一遍：

```
□ 读本文档，确认改动落在哪个模块（连接 / 发送 / 协议 / 测试）
□ cd build && cmake .. && make -j$(nproc)     # 确认能编译
□ 改完后跑 ./test_http_server_stress          # 动连接逻辑必跑
□ 动 epoll/发送逻辑时，检查 mod_client_events 是否正确使用 EPOLLOUT
□ 不引入新的外部依赖，除非明确需要
□ 改动范围最小化，匹配现有代码风格
```

### 0.1 开发环境速记

| 项 | 值 |
|----|-----|
| 远程 VM | `192.168.61.217`，项目路径 `/root/http-server/http-server` |
| 编译 | `cd build && cmake .. && make`（CMake 自动选 gcc-toolset-10） |
| C++ 标准 | C++20（`unordered_map::contains` 等） |
| 运行 | `./simple_http_server 8080 ../www` |
| 压测 | `./test_http_server_stress` 或 `--forever` |

### 0.2 核心开发原则（经验沉淀）

#### ① 先数据结构，再改行为

长连接改造的顺序：

1. 定义 `Connection`（read/write buffer、keep_alive、closed）
2. 改 `handle_client` 生命周期（不再每次 close）
3. 改 HTTP 响应头（keep-alive / close）

**不要跳步**，否则 `parse_request` 返回空时容易误关连接。

#### ② 非阻塞 I/O 三条铁律

| 规则 | 错误做法 | 正确做法 |
|------|----------|----------|
| 不 spin | `while(EAGAIN) continue send` | `return false`，注册 EPOLLOUT，让出 epoll |
| EAGAIN 不是错误 | `cerr << "Send error"` | 静默 return，等下次事件 |
| 先发后读 | 有 write_buffer 时先读新请求 | 段 A 先 flush_write_buffer |

#### ③ 连接何时 close、何时 break

```
request.empty() + closed=true  → close（客户端断开）
request.empty() + closed=false → break（半包或等下一个请求，不关）
keep_alive=false + 响应发完    → close
keep_alive=true + 响应发完     → break，fd 留着
```

**empty 不等于 close**，这是长连接最容易踩的坑。

#### ④ epoll 事件按需注册

```
默认：              EPOLLIN          （等客户端请求）
send 遇 EAGAIN：    EPOLLIN|EPOLLOUT （等可写）
发完：              EPOLLIN          （去掉 EPOLLOUT，避免空转）
```

> **已知 bug**：`mod_client_events` 当前写死 `EPOLLIN|EPOLLET`，未使用传入的 EPOLLOUT。修改发送逻辑时优先修复。

#### ⑤ 发送路径认知

当前是 `load_file → build_response → write_buffer → send`，**不是 sendfile**。

- 小响应通常一次 send 发完，掩盖 EPOLLOUT bug
- 性能瓶颈在 syscall 和用户态拷贝，不在业务逻辑
- 优化优先考虑：sendfile、文件缓存、减少拷贝

#### ⑥ 测试策略

| 改动类型 | 必跑测试 |
|----------|----------|
| 连接/长连接/epoll | `test_http_server_stress` |
| HTML 解析 | `test_html_parser` |
| 单次请求路径 | `test_http_server` |
| 性能观察 | `--forever` + `top -p <pid>` |

压测通过标准（1300 万+ 迭代验证）：

- 无崩溃（waitpid 检测）
- VmRSS 不持续上涨（泄漏阈值 5MB）
- qps 稳定

#### ⑦ 内存观察经验

- 看 **RES / VmRSS**，不是 VIRT
- 启动 ~2MB → 压测后 ~72KB 是**正常**（Linux 回收不活跃页）
- RES 持续单调上涨才是泄漏信号
- `VmSwap` 几十 KB 在内存紧张机器上正常

#### ⑧ 代码风格

- 连接状态放 `client_conns_[fd]`，不要多个平行 map
- 用 `client_conns_.contains(fd)` 检查存在（C++20）
- 注释解释**为什么**，不解释显而易见的代码
- 不大改无关代码，不引入不必要抽象

### 0.3 改动导航（改什么读哪节）

| 我要改… | 先读 |
|---------|------|
| 连接保持/关闭 | §2、§4、§5 |
| send/发送缓冲区 | §6、§7 |
| Keep-Alive 头解析 | §5 |
| 性能优化 | §8 |
| 加测试 | §9 |
| 编译/环境问题 | §10 |

---

## 1. 项目概览

| 项 | 说明 |
|----|------|
| 语言 | C++20（CentOS 8 需 gcc-toolset-10） |
| 并发模型 | 单线程 epoll，非阻塞 I/O，边缘触发（EPOLLET） |
| 功能 | 静态文件服务；HTML → JSON；HTTP Keep-Alive 长连接 |
| 入口 | `src/main.cpp` → `HttpServer::run()` |
| 构建 | `mkdir build && cd build && cmake .. && make` |
| 运行 | `./simple_http_server 8080 ../www` |

---

## 2. 架构演进：短连接 → 长连接

### 2.1 短连接（改之前）

```
accept → 读一个请求 → 发响应 → close(fd)
```

- 每个 fd 只需一个 `read_buffer` 字符串
- 响应头固定 `Connection: close`
- `send` 遇 EAGAIN 时在原地 spin，阻塞事件循环

### 2.2 长连接（当前）

```
accept → 读请求 → 发响应 → fd 保留 → 读下一个请求 → ... → 最后才 close
```

核心改动分三步：

1. **数据结构**：`client_buffers_` → `client_conns_[fd]`（`Connection` 结构体）
2. **行为**：`handle_client` 不再处理完就 close；用 `while` 循环处理同一 fd 上多个请求
3. **协议**：响应头动态返回 `Connection: keep-alive` 或 `close`

---

## 3. 连接状态 `Connection`

```cpp
struct Connection {
    std::string read_buffer;   // 尚未读完整的请求（支持 TCP 半包/粘包）
    std::string write_buffer;  // 尚未发完的响应
    bool keep_alive = true;    // 当前连接下一个响应后是否保持
    bool closed = false;       // recv 返回 0 时置 true，表示客户端已断开
};
```

| 字段 | 作用 |
|------|------|
| `read_buffer` | 暂存未完整的 HTTP 请求头（以 `\r\n\r\n` 为界） |
| `write_buffer` | 非阻塞 `send` 未发完的数据 |
| `keep_alive` | 按每个请求解析，决定发完响应后是否关连接 |
| `closed` | 区分「请求没读完」与「客户端已断开」 |

---

## 4. 核心函数：`handle_client`

epoll 通知 fd 可读/可写时的总调度入口。

### 4.1 两大段结构

```
段 A（267-278）：write_buffer 非空 → 先续发上次没发完的响应
段 B（280-307）：while 循环 → 读请求 → 生成响应 → 发送
```

### 4.2 决策表

| 条件 | 动作 |
|------|------|
| fd 不在 `client_conns_` | return |
| `write_buffer` 非空，flush 失败（EAGAIN） | 注册 EPOLLOUT，return |
| flush 成功 + 短连接 | close |
| `request` 空 + `closed=true` | close |
| `request` 空 + `closed=false` | break，等下次 EPOLLIN |
| 响应 flush 失败 | 注册 EPOLLOUT，return |
| 响应发完 + 短连接 | close |
| 响应发完 + `read_buffer` 还有完整请求 | while 继续（粘包） |
| 响应发完 + 无下一个请求 | break，等 EPOLLIN |

### 4.3 `request.empty()` 的三种含义

| 情况 | closed | read_buffer | 处理 |
|------|--------|-------------|------|
| 客户端断开 | true | 任意 | close |
| 请求半包 | false | 非空 | break，继续等 |
| 等下一个请求 | false | 空 | break，继续等 |

---

## 5. Keep-Alive 判定：`parse_keep_alive`

HTTP 规则三句话：

1. **HTTP/1.1** → 默认长连接（true）
2. **HTTP/1.0** → 默认短连接（false）
3. 请求头 `Connection: close` → false；`Connection: keep-alive` → true

实现：读版本定默认值 → 在 request 里找 `\r\nconnection:` 行 → 匹配 close/keep-alive。

压测中的两种模式：

| 模式 | 请求头 | 效果 |
|------|--------|------|
| 短连接（98%） | `Connection: close` | 发完即关 |
| 长连接（2%） | HTTP/1.1 无 Connection 头 | 同一 socket 连发 3 个请求 |

---

## 6. 发送逻辑

### 6.1 数据路径（当前：非 sendfile）

```
磁盘 → load_file → body → html_to_json(可选) → build_response
     → write_buffer → send() → 内核发送缓冲区 → 网络 → 客户端
```

至少 4~5 次用户态拷贝。未使用 `sendfile` 零拷贝。

### 6.2 `flush_write_buffer`

```cpp
while (!write_buffer.empty()) {
    sent = send(fd, write_buffer.data(), write_buffer.size(), 0);
    if (sent > 0)  → erase 已发部分，continue
    if (EAGAIN)     → return false（正常，等 EPOLLOUT）
    else            → return false（真错误，打日志）
}
return true;  // 全部发完
```

### 6.3 `send()` 与 EAGAIN

| 返回值 | 含义 |
|--------|------|
| `> 0` | 成功发出 sent 字节（可能小于 write_buffer.size） |
| `-1` + EAGAIN | 内核发送缓冲区满，不是错误，稍后再试 |
| `-1` + 其他 | 真错误（EPIPE、ECONNRESET 等） |

`EAGAIN` 与 `EWOULDBLOCK` 在 Linux 上等价，两个都判断是为跨平台。

---

## 7. Socket 与 epoll 事件

### 7.1 Socket 是什么

socket（fd）是操作系统给的**网络连接句柄**。程序不直接碰网络，通过内核两个缓冲区收发：

| 缓冲区 | 操作 | 满/空时的行为 |
|--------|------|---------------|
| 接收缓冲区 | `recv` | 空 → EAGAIN（非阻塞） |
| 发送缓冲区 | `send` | 满 → EAGAIN（非阻塞） |

### 7.2 为什么要注册读写事件

| 事件 | 含义 | 何时注册 |
|------|------|----------|
| EPOLLIN | 接收缓冲区有数据 | 几乎一直（等客户端请求） |
| EPOLLOUT | 发送缓冲区有空间 | **仅 send 遇 EAGAIN 时** |
| EPOLLET | 边缘触发 | 配合 non-blocking |

**不要一直注册 EPOLLOUT**：socket 几乎总是可写的，会导致 epoll 空转浪费 CPU。

### 7.3 `mod_client_events` 设计意图

```
默认（accept / 发完响应）：EPOLLIN
send 发不动（EAGAIN）：      EPOLLIN + EPOLLOUT
发完：                      EPOLLIN
```

### 7.4 已知 Bug

```cpp
// 当前代码（错误）
event.events = EPOLLIN | EPOLLET;  // 忽略了传入的 EPOLLOUT

// 应改为
event.events = events | EPOLLET;
```

因响应小、一次 send 通常发完，且 EPOLLIN 时会顺带续发 write_buffer，压测仍能通过；大响应或慢网络下可能出问题。

---

## 8. 性能与瓶颈

### 8.1 实测数据（CentOS 8 VM，782MB 内存）

| 指标 | 数值 |
|------|------|
| 压测迭代 | 1300 万+ 无崩溃 |
| qps（单线程压测客户端） | ~20650 |
| 服务器 CPU | ~42% 单核 |
| VmRSS | 72~320 kB（稳定，无泄漏） |
| wrk 并发压测 | ~5.6 万 req/s |

### 8.2 瓶颈优先级

```
① 系统调用（accept/recv/send/close 频繁）  ← sy ~49%
② 无 sendfile，文件内容多次经过用户态
③ 无静态文件缓存，每次 load_file
④ html_to_json CPU 开销（GET / 热点）
⑤ write_buffer.erase(0,n) O(n) 移动
⑥ 单线程，无法利用多核
```

### 8.3 内存观察

- **RES / VmRSS**：进程实际占用的物理内存
- 启动时 ~1920 kB → 压测后 ~72 kB 是**正常现象**（非泄漏）
- 原因：启动加载开销 + Linux 回收不活跃页（Swap 有 ~80 kB 也正常）
- **异常信号**：RES 持续单调上涨

---

## 9. 自动化测试

### 9.1 测试文件

`tests/test_http_server_stress.cpp`

### 9.2 流程

```
fork 启动 simple_http_server (8082)
    ↓
记录 VmRSS 基线
    ↓
循环（默认 3000 / --forever 无限）
  ├─ 每 100 次：waitpid 检查崩溃
  ├─ 98%：Connection: close（短连接）
  └─ 2%：同一连接 3 个 keep-alive 请求
    ↓
VmRSS 增量 > 5MB → FAIL（固定模式）
```

### 9.3 运行

```bash
cd build
./test_http_server_stress              # 3000 次，CI 用
./test_http_server_stress --forever    # 无限循环，Ctrl+C 停止
ctest -R http_server_stress
```

### 9.4 监控

```bash
# 压测会打印 server pid
top -p <pid>           # 看 %CPU 和 RES
grep VmRSS /proc/<pid>/status
```

---

## 10. 构建环境

| 项 | 说明 |
|----|------|
| OS | CentOS 8 |
| 默认 GCC | 8.5（不支持 C++20 `contains`） |
| 项目用 GCC | gcc-toolset-10（CMake 自动选择） |
| C++ 标准 | C++20 |

```bash
# 手动启用 toolset
source /opt/rh/gcc-toolset-10/enable
g++ --version   # 应显示 10.3.1
```

---

## 11. 关键文件索引

| 文件 | 职责 |
|------|------|
| `src/http_server.h` | Connection 结构体、HttpServer 类声明 |
| `src/http_server.cpp` | epoll 事件循环、连接管理、请求/响应 |
| `src/html_parser.cpp` | HTML → JSON |
| `src/main.cpp` | 入口，解析端口和 doc_root |
| `tests/test_http_server_stress.cpp` | 崩溃/泄漏压测 |
| `tests/test_http_server.cpp` | e2e 单次请求测试 |
| `CMakeLists.txt` | 构建配置、测试注册 |
| `www/` | 静态文件根目录 |

---

## 12. 待办 / 已知问题

- [ ] 修复 `mod_client_events` 未注册 EPOLLOUT 的 bug
- [ ] 引入 sendfile 优化静态文件发送
- [ ] 静态文件内存缓存
- [ ] write_buffer 改用环形 buffer，避免 erase(0,n)
- [ ] SO_REUSEPORT 多进程利用多核
- [ ] idle 连接超时关闭
- [ ] Ctrl+C 时 stress test 干净退出（避免 recv Connection reset 误报）

---

## 13. 术语速查

| 术语 | 一句话 |
|------|--------|
| fd | 文件描述符，整数，代表一个连接 |
| epoll | Linux 多路复用，等多 fd 的 I/O 事件 |
| non-blocking | 操作不阻塞，做不了返回 EAGAIN |
| Keep-Alive | HTTP 长连接，同一 TCP 发多个请求 |
| EAGAIN | 「现在做不了，稍后再试」，不是错误 |
| RES / VmRSS | 进程实际占用的物理内存（KB） |
| sendfile | 内核态文件→socket 零拷贝 |

---

*最后更新：2026-06-30 · 对应 main 分支 commit `332c479`*
