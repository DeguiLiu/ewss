# EWSS - 嵌入式 WebSocket 服务器

[![CI](https://github.com/DeguiLiu/ewss/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/ewss/actions/workflows/ci.yml)
[![Tests](https://img.shields.io/badge/Tests-7%20passed-brightgreen)](https://github.com/DeguiLiu/ewss/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

轻量级、无 ASIO 依赖的 WebSocket 服务器，专为嵌入式 Linux (ARM) 设计。C++17，poll 驱动 Reactor，固定内存 RingBuffer，零拷贝 I/O。

## 特性

- 无 ASIO 依赖: 基于 poll() 的单线程 Reactor，不依赖 Boost/Libuv
- 固定内存: RingBuffer TX/RX (4KB/8KB 每连接)，热路径零堆分配
- 零拷贝: writev 分散/聚集 I/O，string_view 握手解析
- 状态机: HSM 驱动 WebSocket 生命周期 (Handshaking/Open/Closing/Closed)
- TCP 调优: TCP_NODELAY, TCP_QUICKACK, SO_KEEPALIVE 可配置
- 性能监控: 原子计数器跟踪吞吐量、延迟、错误，过载保护
- 可选 TLS: mbedTLS 集成 (编译期开关 EWSS_WITH_TLS)
- 缓存行对齐: alignas(64) 热数据结构，适配 ARM/x86

## 架构

```
Server (Reactor, poll 驱动)
  |
  +-- Connection #1 ─┐
  +-- Connection #2  ├─ 每个连接:
  +-- Connection #N ─┘
        RxBuffer (RingBuffer<4096>)
            | 解析
        ProtocolHandler (HSM 状态机)
            | 回调
        Application (on_message / on_close)
            | 发送
        TxBuffer (RingBuffer<8192>)
            | writev / write
        TCP Socket (sockpp)
```

## 构建

依赖: CMake 3.14+, C++17 编译器 (GCC 7+, Clang 5+)

```bash
git clone https://github.com/DeguiLiu/ewss.git
cd ewss
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

构建选项:

| 选项 | 默认值 | 说明 |
|------|--------|------|
| EWSS_BUILD_TESTS | ON | 构建 Catch2 单元测试 |
| EWSS_BUILD_EXAMPLES | ON | 构建示例服务器 |
| EWSS_NO_EXCEPTIONS | OFF | 禁用异常和 RTTI |
| EWSS_WITH_TLS | OFF | 启用 mbedTLS 支持 |

## 快速开始

```cpp
#include "ewss/server.hpp"

int main() {
  ewss::Server server(8080);

  // TCP 调优 (低延迟)
  ewss::TcpTuning tuning;
  tuning.tcp_nodelay = true;
  server.set_tcp_tuning(tuning);

  server.on_message = [](const auto& conn, std::string_view msg) {
    conn->send(msg);  // 回显
  };

  server.run();
}
```

使用 wscat 测试:
```bash
wscat -c ws://localhost:8080
> hello
< hello
```

## API

```cpp
// 服务器
ewss::Server server(port);
server.set_max_connections(50);
server.set_tcp_tuning(tuning);
server.set_use_writev(true);
server.on_connect  = [](const ConnPtr&) {};
server.on_message  = [](const ConnPtr&, std::string_view) {};
server.on_close    = [](const ConnPtr&, bool clean) {};
server.on_error    = [](const ConnPtr&) {};
server.run();

// 连接
conn->send("文本消息");
conn->send_binary(binary_data);
conn->close(1000);
conn->get_id();
conn->get_state();
```

## 性能

x86-64 单线程回显服务器基准测试:

| 指标 | 数值 |
|------|------|
| 吞吐量 | ~680K msg/s |
| P50 延迟 | 0.015 ms |
| P99 延迟 | 0.062 ms |
| 每连接内存 | ~12 KB 固定 |

## 头文件结构

| 文件 | 说明 |
|------|------|
| vocabulary.hpp | 错误类型, expected, optional, FixedVector, FixedString, FixedFunction, Logger, kCacheLine |
| utils.hpp | Base64, SHA-1, WebSocket 帧解析/编码 |
| protocol_hsm.hpp | 协议状态机 (Handshake/Open/Closing/Closed) |
| connection.hpp | RingBuffer, Connection 类 |
| connection_pool.hpp | ObjectPool, ServerStats |
| server.hpp | Server 类, TcpTuning |
| tls.hpp | 可选 TLS (mbedTLS) |

## 测试

```bash
cd build && ctest --output-on-failure
```

7 个测试用例覆盖 Base64、SHA-1、WebSocket 帧解析/编码。

## 示例

- `echo_server.cpp` - 回显服务器
- `broadcast_server.cpp` - 广播服务器
- `perf_server.cpp` - 性能基准测试服务器

## 平台支持

- ARM-Linux (32/64 位) - 主要目标平台
- x86-64 Linux
- 任何 POSIX 兼容系统

不支持: Windows, macOS (CI 不测试)

## 许可证

MIT License - 详见 [LICENSE](LICENSE) 文件。

## 参考

- [WebSocket RFC 6455](https://tools.ietf.org/html/rfc6455)
- [sockpp](https://github.com/fpagliughi/sockpp)
