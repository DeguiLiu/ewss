# EWSS - Embedded WebSocket Server

[![CI](https://github.com/DeguiLiu/ewss/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/ewss/actions/workflows/ci.yml)
[![Tests](https://img.shields.io/badge/Tests-7%20passed-brightgreen)](https://github.com/DeguiLiu/ewss/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

[中文文档](README_zh.md)

Lightweight, ASIO-free WebSocket server for embedded Linux (ARM). C++17, poll-based Reactor, fixed-memory RingBuffers, zero-copy I/O.

## Features

- ASIO-free: poll()-based single-threaded Reactor, no Boost/Libuv dependency
- Fixed memory: RingBuffer TX/RX (4KB/8KB per connection), no heap allocation in hot path
- Zero-copy: writev scatter/gather I/O, string_view-based handshake parsing
- State machine: HSM-driven WebSocket lifecycle (Handshaking/Open/Closing/Closed)
- TCP tuning: TCP_NODELAY, TCP_QUICKACK, SO_KEEPALIVE with configurable parameters
- Performance monitoring: atomic counters for throughput, latency, errors, overload protection
- Optional TLS: mbedTLS integration (compile-time toggle via EWSS_WITH_TLS)
- Cache-line aligned: alignas(64) on hot data structures for ARM/x86

## Architecture

```
Server (Reactor, poll-based)
  |
  +-- Connection #1 ─┐
  +-- Connection #2  ├─ Each connection:
  +-- Connection #N ─┘
        RxBuffer (RingBuffer<4096>)
            | parse
        ProtocolHandler (HSM state machine)
            | callback
        Application (on_message / on_close)
            | send
        TxBuffer (RingBuffer<8192>)
            | writev / write
        TCP Socket (sockpp)
```

## Building

Requirements: CMake 3.14+, C++17 compiler (GCC 7+, Clang 5+)

```bash
git clone https://github.com/DeguiLiu/ewss.git
cd ewss
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Build options:

| Option | Default | Description |
|--------|---------|-------------|
| EWSS_BUILD_TESTS | ON | Build Catch2 unit tests |
| EWSS_BUILD_EXAMPLES | ON | Build example servers |
| EWSS_NO_EXCEPTIONS | OFF | Disable exceptions and RTTI |
| EWSS_WITH_TLS | OFF | Enable mbedTLS support |

## Quick Start

```cpp
#include "ewss/server.hpp"

int main() {
  ewss::Server server(8080);

  // TCP tuning for low latency
  ewss::TcpTuning tuning;
  tuning.tcp_nodelay = true;
  server.set_tcp_tuning(tuning);

  server.on_message = [](const auto& conn, std::string_view msg) {
    conn->send(msg);  // Echo back
  };

  server.run();
}
```

Test with wscat:
```bash
wscat -c ws://localhost:8080
> hello
< hello
```

## API

```cpp
// Server
ewss::Server server(port);
server.set_max_connections(50);
server.set_tcp_tuning(tuning);
server.set_use_writev(true);
server.on_connect  = [](const ConnPtr&) {};
server.on_message  = [](const ConnPtr&, std::string_view) {};
server.on_close    = [](const ConnPtr&, bool clean) {};
server.on_error    = [](const ConnPtr&) {};
server.run();

// Connection
conn->send("text message");
conn->send_binary(binary_data);
conn->close(1000);
conn->get_id();
conn->get_state();
```

## Performance

Benchmark on x86-64 (single-threaded echo server):

| Metric | Value |
|--------|-------|
| Throughput | ~680K msg/s |
| P50 latency | 0.015 ms |
| P99 latency | 0.062 ms |
| Memory per connection | ~12 KB fixed |

## Header Files

| File | Description |
|------|-------------|
| vocabulary.hpp | Error types, expected, optional, FixedVector, FixedString, FixedFunction, Logger, kCacheLine |
| utils.hpp | Base64, SHA-1, WebSocket frame parsing/encoding |
| protocol_hsm.hpp | Protocol state machine (Handshake/Open/Closing/Closed) |
| connection.hpp | RingBuffer, Connection class |
| connection_pool.hpp | ObjectPool, ServerStats |
| server.hpp | Server class, TcpTuning |
| tls.hpp | Optional TLS (mbedTLS) |

## Testing

```bash
cd build && ctest --output-on-failure
```

7 test cases covering Base64, SHA-1, WebSocket frame parsing/encoding.

## Examples

- `echo_server.cpp` - Echo server
- `broadcast_server.cpp` - Broadcast to all clients
- `perf_server.cpp` - Performance benchmark server

## Platform Support

- ARM-Linux (32/64-bit) - primary target
- x86-64 Linux
- Any POSIX-compliant system

Not supported: Windows, macOS (not tested in CI)

## License

MIT License - See [LICENSE](LICENSE) file.

## References

- [WebSocket RFC 6455](https://tools.ietf.org/html/rfc6455)
- [sockpp](https://github.com/fpagliughi/sockpp)
