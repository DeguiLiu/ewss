# EWSS - Embedded WebSocket Server

Lightweight, ASIO-free WebSocket server designed for embedded Linux systems (ARM). Built with modern C++17, using `sockpp` for socket management, `ringbuffer` for fixed-memory buffers, and hierarchical state machines for protocol handling.

**Repository**: https://gitee.com/liudegui/ewss

## Key Features

- **No ASIO dependency**: Direct `poll()`/`epoll()` based Reactor pattern for minimal overhead
- **Fixed memory allocation**: RingBuffer-based TX/RX buffers eliminate memory fragmentation
- **Hierarchical state machine**: Clean protocol state management (Handshaking, Open, Closing, Closed)
- **Zero-copy frame handling**: Efficient WebSocket frame parsing and masking
- **Single-threaded design**: No locks needed; suitable for embedded systems with limited resources
- **C++17 standard**: Modern C++ with RAII resource management
- **Header-only core**: Easy integration; core functionality can be header-only with appropriate changes

## Design Philosophy

**"Explicit over implicit, static allocation over dynamic"**

- All buffer sizes are compile-time configurable (`constexpr`)
- No hidden dynamic allocations in hot paths
- Clear, simple API with callbacks for application integration
- Minimal dependencies (only sockpp, no Boost/ASIO/Libuv)

## Architecture

```
┌─────────────────────────────────────────┐
│          Server (Reactor)               │
│  - poll() / epoll() event loop          │
│  - manage connections                   │
└─────────────────────────────────────────┘
           │
           ├─→ Connection #1
           ├─→ Connection #2
           └─→ Connection #N

Each Connection:
  ┌─────────────────────────────────────┐
  │  RxBuffer (ringbuffer)              │
  │         ↓ (parser)                  │
  │  ProtocolHandler (state machine)    │
  │         ↓ (callback)                │
  │  Application (on_message)           │
  │         ↓ (user send)               │
  │  TxBuffer (ringbuffer)              │
  │         ↓ (poll POLLOUT)            │
  │  TCP Socket                         │
  └─────────────────────────────────────┘
```

## Building

### Requirements

- CMake 3.14+
- C++17 compiler (GCC 7+, Clang 5+)
- sockpp library (auto-fetched via FetchContent)

### Build Steps

```bash
cd ewss
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Build Options

```bash
# Disable exceptions and RTTI
cmake .. -DEWSS_NO_EXCEPTIONS=ON

# Skip tests and examples
cmake .. -DEWSS_BUILD_TESTS=OFF -DEWSS_BUILD_EXAMPLES=OFF
```

## Quick Start

### Echo Server Example

```cpp
#include "ewss/server.hpp"

int main() {
  ewss::Server server(8080);

  server.on_connect = [](const auto& conn) {
    std::cout << "Client connected: " << conn->get_id() << std::endl;
  };

  server.on_message = [](const auto& conn, std::string_view msg) {
    std::cout << "Received: " << msg << std::endl;
    conn->send(std::string("Echo: ") + std::string(msg));
  };

  server.on_close = [](const auto& conn, bool clean) {
    std::cout << "Client closed" << std::endl;
  };

  server.run();
  return 0;
}
```

Build and run:
```bash
$ ./echo_server 8080
[EWSS INFO] Server initialized on :8080
[EWSS INFO] Server starting...
```

Test with WebSocket client:
```bash
# Using wscat (npm install -g wscat)
$ wscat -c ws://localhost:8080
Connected (press CTRL+C to quit)
> Hello
< Echo: Hello
```

## API Reference

### Server

```cpp
class Server {
  Server(uint16_t port, const std::string& bind_addr = "");

  void run();                                    // Start blocking event loop
  void stop();                                   // Stop the server

  Server& set_max_connections(size_t max);      // Configure max connections
  Server& set_poll_timeout_ms(int timeout);     // Configure poll timeout

  std::function<void(const ConnPtr&)> on_connect;
  std::function<void(const ConnPtr&, std::string_view)> on_message;
  std::function<void(const ConnPtr&, bool)> on_close;
  std::function<void(const ConnPtr&, const std::string&)> on_error;

  size_t get_connection_count() const;
};
```

### Connection

```cpp
class Connection : public std::enable_shared_from_this<Connection> {
  void send(std::string_view payload);
  void send_binary(std::string_view payload);
  void close(uint16_t code = 1000);

  bool is_closed() const;
  bool has_data_to_send() const;
  uint64_t get_id() const;
  int get_fd() const;
  ConnectionState get_state() const;

  // Callbacks
  std::function<void(const ConnPtr&)> on_open;
  std::function<void(const ConnPtr&, std::string_view)> on_message;
  std::function<void(const ConnPtr&, bool)> on_close;
  std::function<void(const ConnPtr&)> on_error;
};
```

## Testing

Run unit tests:
```bash
cd build
ctest --verbose
```

Test coverage includes:
- Base64 encoding/decoding
- SHA-1 hashing (for WebSocket key generation)
- WebSocket frame parsing and encoding
- Protocol state transitions
- Connection lifecycle

## Performance Characteristics

- **Zero dynamic allocations** in the hot path (data receive/send)
- **Fixed buffer sizes** (4KB RX, 8KB TX per connection)
- **O(1) buffer operations** (circular buffer)
- **Single-threaded** (no context switches, no lock contention)
- **Poll-based** (suitable for up to ~1000 connections; epoll available for larger)

### Resource Usage (per connection)

- Memory: ~12 KB fixed (4KB RX + 8KB TX buffers)
- CPU: O(1) per message on modern systems

## Embedded Platform Support

Designed for:
- ARM-Linux (32/64-bit)
- RT-Thread RTOS (future migration target)
- Any POSIX-compliant system

**Not supported**:
- Windows (Windows Sockets differ from POSIX)
- WebAssembly
- Real-time MCUs without networking stack

## Examples

See `examples/`:
- `echo_server.cpp` - Simple echo server
- `broadcast_server.cpp` - Broadcast all messages to connected clients

## Documentation

- `docs/design_zh.md` - Detailed design document (Chinese)
- `docs/api.md` - API reference
- `docs/faq.md` - Frequently asked questions

## Known Limitations

1. **No TLS/SSL support** (use nginx/HAProxy as reverse proxy if needed)
2. **No compression** (deflate extension not implemented)
3. **Single-threaded only** (cannot use multi-worker pattern directly)
4. **No persistent storage** (sessions are in-memory)

## License

MIT License - See LICENSE file for details

## Contributing

Contributions welcome! Please:
1. Follow the Google C++ Style Guide
2. Ensure all tests pass
3. Add tests for new features
4. Update documentation

## References

- [WebSocket RFC 6455](https://tools.ietf.org/html/rfc6455)
- [sockpp Documentation](https://github.com/fpagliughi/sockpp)
- [POSIX Poll](https://man7.org/linux/man-pages/man2/poll.2.html)

---

**Status**: Active development (v0.1.0-alpha)

Last updated: 2026-02-18
