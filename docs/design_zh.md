# EWSS (Embedded WebSocket Server) 设计文档

**版本**: v0.2.0
**日期**: 2026-02-19
**状态**: 稳定版本

---

## 1. 概述

EWSS 是一个轻量级 WebSocket 服务器，专为嵌入式 Linux 环境（特别是 ARM 平台）设计。核心理念是"**显式优于隐式，静态内存优于动态内存**"。

从 [Simple-WebSocket-Server](https://gitlab.com/eidheim/Simple-WebSocket-Server) 重构而来，去掉 ASIO 依赖，用 `poll()` 单线程 Reactor 替代多线程模型，用固定大小 RingBuffer 替代动态缓冲区，用状态机替代隐式的 ASIO handler 链。

### 1.1 核心指标

| 指标 | 值 |
|------|-----|
| 二进制大小 (stripped) | 67 KB |
| 每连接内存 | ~12 KB (4KB RX + 8KB TX) |
| 热路径堆分配 | 0 |
| 最大连接数 (编译期) | 64 |
| P50 延迟 (单客户端, 64B) | 35.5 us |
| P99 延迟 (单客户端, 64B) | 54.6 us |
| 单客户端吞吐量 | ~27K msg/s |
| 多客户端吞吐量 (4 clients) | ~67K msg/s |

### 1.2 目标平台

| 平台 | 支持 | 备注 |
|------|------|------|
| ARM Linux | ✓ | 主要目标平台 |
| x86 Linux | ✓ | 开发测试 |
| Windows | ✗ | 不支持 POSIX socket |

### 1.3 依赖

| 依赖 | 版本 | 用途 |
|------|------|------|
| [sockpp](https://github.com/DeguiLiu/sockpp) | v2.0 (fork) | TCP socket 封装，支持 `-fno-exceptions` |
| [Catch2](https://github.com/catchorg/Catch2) | v3.5.2 | 单元测试框架 (仅测试) |
| mbedTLS | 可选 | TLS 支持 (通过 `EWSS_WITH_TLS` 启用) |

---

## 2. 架构设计

### 2.1 整体架构

```
Server (poll Reactor)
  |
  +-- Connection #1 ─┐
  +-- Connection #2  ├─ 每个连接:
  +-- Connection #N ─┘
        RxBuffer (RingBuffer<4096>)
            | readv 零拷贝接收
        ProtocolHandler (状态机)
            | on_message 回调
        Application
            | send()
        TxBuffer (RingBuffer<8192>)
            | writev 零拷贝发送
        TCP Socket (sockpp)
```

### 2.2 核心设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| I/O 模型 | `poll()` 单线程 Reactor | 无锁、无上下文切换、Cache 友好 |
| 内存模型 | 编译期固定 RingBuffer | 运行时零堆分配，确定性内存 |
| 状态管理 | 4 状态协议处理器 (静态实例) | 零分配状态转换，职责清晰 |
| Socket I/O | `readv`/`writev` 零拷贝 | 内核直接读写 RingBuffer，省去 memcpy |
| 错误处理 | `expected<V, E>` | 类型安全，兼容 `-fno-exceptions` |
| 连接容器 | `FixedVector<ConnPtr, 64>` | 栈分配，swap-and-pop O(1) 移除 |
| 异常支持 | `EWSS_THROW` 宏 | 有异常时 throw，无异常时 abort |

### 2.3 核心模块

| 模块 | 文件 | 职责 |
|------|------|------|
| Server | `server.hpp/cpp` | Reactor 主循环、连接管理、TCP 调优、过载保护、性能监控 |
| Connection | `connection.hpp/cpp` | 连接生命周期、状态转换、零拷贝 I/O、回压控制、超时管理 |
| ProtocolHandler | `connection.hpp/cpp` | WebSocket 握手、帧解析 (HandshakeState/OpenState/ClosingState/ClosedState) |
| RingBuffer | `connection.hpp` | 固定大小循环缓冲，`readv`/`writev` iovec 接口 |
| Utils | `utils.hpp` | Base64、SHA1、WebSocket 帧编解码、掩码处理 |
| TLS | `tls.hpp` | 可选 mbedTLS 适配层 (TlsConfig/TlsContext/TlsSession) |
| Vocabulary | `vocabulary.hpp` | 基础类型: expected、optional、FixedVector、FixedString、FixedFunction、ScopeGuard |
| ConnectionPool | `connection_pool.hpp` | ServerStats 原子计数器、过载检测 |

---

## 3. 数据流

### 3.1 接收路径 (零拷贝)

```
TCP Socket
    ↓ readv (直接写入 RingBuffer 可写区域)
RxBuffer.fill_iovec_write() → iovec[2]
    ↓ readv 系统调用
RxBuffer.commit_write(n)
    ↓
ProtocolHandler::handle_data_received()
    ├─ HandshakeState: parse_handshake()
    │   ├─ 零拷贝: peek 到栈缓冲，string_view 解析
    │   ├─ 提取 Sec-WebSocket-Key
    │   ├─ 生成 Accept Key (SHA1 + Base64)
    │   ├─ snprintf 构建 HTTP 101 响应 (栈上 256B)
    │   ├─ 写入 TxBuffer
    │   └─ 转移 → OpenState, 触发 on_open
    └─ OpenState: parse_frames()
        ├─ 循环: peek → parse_frame_header()
        ├─ 检查 payload 完整性
        ├─ unmask_payload() (客户端帧)
        ├─ 分发: Text/Binary → on_message, Ping → Pong, Close → 关闭
        └─ RxBuffer.advance(total_frame_size)
```

### 3.2 发送路径 (零拷贝)

```
Application: conn->send(payload)
    ↓
write_frame()
    ├─ 栈上编码帧头 (uint8_t header_buf[14])
    ├─ TxBuffer.push(header, header_len)
    └─ TxBuffer.push(payload.data(), payload.size())
        ↓
check_high_watermark()  // 回压检测
    ↓ (TxBuffer > 75% → on_backpressure)
poll() 返回 POLLOUT
    ↓
handle_write_vectored()
    ├─ TxBuffer.fill_iovec() → iovec[2]
    ├─ writev 系统调用 (零拷贝)
    ├─ TxBuffer.advance(n)
    └─ check_low_watermark()  // TxBuffer < 25% → on_drain
```

---

## 4. 协议状态机

### 4.1 状态转换图

```
Handshaking ──(握手成功)──> Open ──(Close 帧)──> Closing ──> Closed
     |                       |                                  ^
     +──(超时 5s)────────────+──────(错误)──────────────────────+
                             |                                  ^
                             +──(Close 超时 5s)─────────────────+
```

### 4.2 静态实例 (零堆分配)

```cpp
static HandshakeState g_handshake_state;
static OpenState      g_open_state;
static ClosingState   g_closing_state;
static ClosedState    g_closed_state;
```

状态转换通过指针切换实现:

```cpp
void Connection::transition_to_state(ConnectionState state) {
  switch (state) {
    case ConnectionState::kOpen:
      protocol_handler_ = &g_open_state;
      if (on_open) on_open(shared_from_this());
      break;
    case ConnectionState::kClosing:
      protocol_handler_ = &g_closing_state;
      closing_at_ = SteadyClock::now();  // Record close start time
      break;
    case ConnectionState::kClosed:
      protocol_handler_ = &g_closed_state;
      if (on_close) on_close(shared_from_this(), true);
      break;
    // ...
  }
}
```

### 4.3 事件处理表

| 状态\事件 | EvDataReceived | EvSendRequest | EvClose | EvTimeout |
|-----------|---------------|---------------|---------|-----------|
| Handshaking | parse_handshake | error | →Closed | →Closed (5s) |
| Open | parse_frames | write_frame | →Closing | — |
| Closing | wait_close | error | no-op | →Closed (5s) |
| Closed | error | error | no-op | — |

---

## 5. 回压控制

### 5.1 水位机制

TxBuffer (8192B) 的回压控制:

| 水位 | 阈值 | 动作 |
|------|------|------|
| 高水位 | 75% (6144B) | 触发 `on_backpressure` 回调，设置 `write_paused_` |
| 低水位 | 25% (2048B) | 触发 `on_drain` 回调，清除 `write_paused_` |

```cpp
void Connection::check_high_watermark() {
  if (!write_paused_ && tx_buffer_.size() > kTxHighWatermark) {
    write_paused_ = true;
    if (on_backpressure) on_backpressure(shared_from_this());
  }
}

void Connection::check_low_watermark() {
  if (write_paused_ && tx_buffer_.size() < kTxLowWatermark) {
    write_paused_ = false;
    if (on_drain) on_drain(shared_from_this());
  }
}
```

### 5.2 触发时机

- `send()` 后检查高水位
- `handle_write()` / `handle_write_vectored()` 后检查低水位

---

## 6. 超时管理

### 6.1 超时类型

| 超时 | 时长 | 触发条件 |
|------|------|---------|
| 握手超时 | 5s (`kHandshakeTimeout`) | 连接建立后 5s 内未完成 WebSocket 握手 |
| 关闭超时 | 5s (`kCloseTimeout`) | 发送 Close 帧后 5s 内未收到对端 Close 帧 |

### 6.2 实现

基于 `std::chrono::steady_clock` 时间戳:

```cpp
// Each connection tracks key timestamps
TimePoint created_at_ = SteadyClock::now();   // Connection creation time
TimePoint closing_at_{};                       // Entering Closing state time
TimePoint last_activity_ = SteadyClock::now(); // Last activity time

// Server main loop checks timeouts
for (auto& conn : connections_) {
  if (conn->is_handshake_timed_out() || conn->is_close_timed_out()) {
    conn->close();
  }
}
```

---

## 7. Server Reactor

### 7.1 主循环

```cpp
void Server::run() {
  while (is_running_) {
    // 1. Build pollfd array (pre-allocated std::array<pollfd, 65>)
    poll_fds_[0] = {server_sock_, POLLIN, 0};
    for (uint32_t i = 0; i < connections_.size(); ++i) {
      short events = POLLIN;
      if (connections_[i]->has_data_to_send()) events |= POLLOUT;
      poll_fds_[i + 1] = {connections_[i]->get_fd(), events, 0};
    }

    // 2. Poll for events (with latency tracking)
    int ret = ::poll(poll_fds_.data(), nfds, poll_timeout_ms_);

    // 3. Handle new connections (with overload protection)
    if (poll_fds_[0].revents & POLLIN) {
      if (stats_.is_overloaded(max_connections_)) {
        // Accept and immediately close to drain kernel backlog
        int reject_sock = accept(server_sock_, ...);
        if (reject_sock >= 0) ::close(reject_sock);
      } else {
        accept_connection();
      }
    }

    // 4. Handle client I/O
    for (size_t i = 1; i < nfds; ++i) {
      handle_connection_io(connections_[i - 1], poll_fds_[i]);
    }

    // 5. Enforce timeouts (handshake + close)
    for (auto& conn : connections_) {
      if (conn->is_handshake_timed_out() || conn->is_close_timed_out())
        conn->close();
    }

    // 6. Remove closed connections (swap-and-pop)
    remove_closed_connections();
  }
}
```

### 7.2 TCP 调优

```cpp
struct TcpTuning {
  bool tcp_nodelay = false;        // Disable Nagle algorithm
  bool tcp_quickack = false;       // Reduce ACK delay (Linux-specific)
  bool so_keepalive = false;       // Enable TCP keepalive
  int keepalive_idle_s = 60;       // Seconds before first probe
  int keepalive_interval_s = 10;   // Seconds between probes
  int keepalive_count = 5;         // Max probes before dropping
};
```

### 7.3 性能监控

```cpp
struct ServerStats {
  std::atomic<uint64_t> total_connections{0};
  std::atomic<uint64_t> active_connections{0};
  std::atomic<uint64_t> rejected_connections{0};
  std::atomic<uint64_t> socket_errors{0};
  std::atomic<uint64_t> handshake_errors{0};
  std::atomic<uint64_t> last_poll_latency_us{0};
  std::atomic<uint64_t> max_poll_latency_us{0};

  bool is_overloaded(size_t max) const {
    return active_connections.load() > max * 9 / 10;  // 90% threshold
  }
};
```

---

## 8. TLS 支持

### 8.1 架构

可选 mbedTLS 适配层，通过 CMake 选项 `EWSS_WITH_TLS=ON` 启用:

```cpp
struct TlsConfig {
  std::string cert_path;           // Server certificate (PEM)
  std::string key_path;            // Server private key (PEM)
  std::string ca_path;             // CA certificate for client auth (optional)
  bool require_client_cert = false;
  int min_tls_version = 0;        // 0 = TLS 1.2 minimum
};
```

### 8.2 类层次

| 类 | 职责 | 生命周期 |
|----|------|---------|
| `TlsContext` | 管理证书、密钥、SSL 配置 | 一个 Server 一个 |
| `TlsSession` | 管理单个连接的 SSL 会话 | 一个 Connection 一个 |

TLS 禁用时，`TlsContext` 和 `TlsSession` 提供 stub 实现（所有方法返回 -1），零开销。

---

## 9. `-fno-exceptions` 支持

### 9.1 EWSS_THROW 宏

```cpp
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#define EWSS_THROW(ex) throw(ex)
#else
#define EWSS_THROW(ex)                \
  do {                                \
    std::fputs(#ex "\n", stderr);     \
    std::abort();                     \
  } while (0)
#endif
```

### 9.2 sockpp SOCKPP_THROW

sockpp (DeguiLiu/sockpp fork) 使用相同模式的 `SOCKPP_THROW` 宏，所有 throw 点统一替换，支持 `-fno-exceptions` 编译。

### 9.3 CMake 配置

```cmake
# Apply -fno-exceptions only to ewss target, not third-party deps
if(EWSS_NO_EXCEPTIONS)
  target_compile_options(ewss PUBLIC -fno-exceptions -fno-rtti)
endif()
```

---

## 10. 词汇类型

从 [newosp](https://github.com/DeguiLiu/newosp) 库移植，全部栈分配、零堆开销:

| 类型 | 替代 | 用途 |
|------|------|------|
| `expected<V, E>` | 异常 / errno | 类型安全错误处理 |
| `optional<T>` | `std::optional` | 可选值 |
| `FixedVector<T, N>` | `std::vector` | 连接列表 (N=64) |
| `FixedString<N>` | `std::string` | 固定长度字符串 |
| `FixedFunction<Sig, Cap>` | `std::function` | SBO 回调 |
| `ScopeGuard` | 手动 cleanup | RAII 资源释放 |

兼容 `-fno-exceptions -fno-rtti`，适合嵌入式编译配置。

---

## 11. 资源占用

### 11.1 内存占用 (每连接)

| 组件 | 大小 |
|------|------|
| Connection 对象 | ~200 B |
| RxBuffer (4KB) | 4,096 B |
| TxBuffer (8KB) | 8,192 B |
| **合计** | **~12.5 KB** |

### 11.2 编译产物

| 指标 | 值 |
|------|-----|
| 二进制大小 (stripped) | 67 KB |
| 静态库 (libewss.a) | 94 KB |

详细性能数据参见 [benchmark_report.md](benchmark_report.md)。

---

## 12. 测试覆盖

| 测试套件 | 内容 |
|---------|------|
| test_crypto | Base64 编解码、SHA1 哈希 |
| test_frame | WebSocket 帧头解析、编码、边界情况 |
| test_integration | 端到端测试 (握手、echo、批量消息、二进制、Ping/Pong、关闭、统计、回调) |
| test_pool | 连接池、ServerStats |
| test_ringbuffer | RingBuffer push/peek/advance、溢出、iovec |
| test_utils | 工具函数 |
| test_vocabulary | expected、optional、FixedVector、FixedString、FixedFunction、ScopeGuard |

- 7 个测试套件，全部通过
- ASan + UBSan 全部通过
- `-fno-exceptions` 模式全部通过
- 集成测试使用原始 POSIX socket 实现的 WebSocket 客户端

---

## 13. 构建

```bash
# Standard build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# -fno-exceptions build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DEWSS_NO_EXCEPTIONS=ON
cmake --build build -j

# TLS build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DEWSS_WITH_TLS=ON
cmake --build build -j
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `EWSS_BUILD_TESTS` | ON | 构建测试 |
| `EWSS_BUILD_EXAMPLES` | ON | 构建示例 |
| `EWSS_NO_EXCEPTIONS` | OFF | 禁用异常和 RTTI |
| `EWSS_WITH_TLS` | OFF | 启用 mbedTLS 支持 |

---

## 14. 设计权衡

| 取舍 | EWSS 选择 | 代价 |
|------|-----------|------|
| 最大连接数 | 64 (编译期固定) | 不能动态扩展 |
| 线程模型 | 单线程 | CPU 密集型任务会阻塞所有连接 |
| 缓冲区大小 | 固定 4KB RX / 8KB TX | 大消息需要分片 |
| poll vs epoll | poll() | POSIX 可移植，但 O(n) 扫描 |
| 内存模型 | 全部预分配 | 固定容量，不能按需增长 |

这些取舍在嵌入式场景下是合理的：64 连接足够覆盖调试面板、配置接口、数据推送等典型用途；单线程避免了锁竞争；固定内存消除了碎片化风险。

---

## 15. 参考

- [RFC 6455: The WebSocket Protocol](https://tools.ietf.org/html/rfc6455)
- [sockpp (DeguiLiu fork)](https://github.com/DeguiLiu/sockpp)
- [newosp](https://github.com/DeguiLiu/newosp)
- [Simple-WebSocket-Server](https://gitlab.com/eidheim/Simple-WebSocket-Server)
- [POSIX poll(2)](https://man7.org/linux/man-pages/man2/poll.2.html)
- [性能基准报告](benchmark_report.md)

---

**文档版本**: 2.0
**最后更新**: 2026-02-19
**维护者**: dgliu
