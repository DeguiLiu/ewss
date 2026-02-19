# EWSS (Embedded WebSocket Server) 设计文档

**版本**: v0.1.0-alpha
**日期**: 2026-02-18
**状态**: 活跃开发中

---

## 1. 概述

EWSS 是一个轻量级 WebSocket 服务器，专为嵌入式 Linux 环境（特别是 ARM 平台）设计。核心理念是"**显式优于隐式，静态内存优于动态内存**"。

### 1.1 核心需求

- **极简依赖**: 仅依赖 sockpp（socket 封装），极简依赖
- **确定性内存**: 所有 buffer 大小编译期固定，无动态扩展
- **单线程 Reactor**: 简化同步模型，适合嵌入式场景
- **高效帧处理**: 零拷贝 WebSocket 帧编解码
- **优雅状态管理**: 层次状态机清晰表达协议流程

### 1.2 目标平台

| 平台 | 支持 | 备注 |
|------|------|------|
| ARM Linux | ✓ | 主要目标平台 |
| x86 Linux | ✓ | 开发测试 |
| RT-Thread | 规划中 | 需 POSIX 支持 |
| Windows | ✗ | 不支持 POSIX socket |

---

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────┐
│           Application Layer                 │
│  (用户回调: on_connect, on_message, etc)   │
└─────────────────────────────────────────────┘
                    ↑ ↓
┌─────────────────────────────────────────────┐
│      Server (Reactor Kernel)                │
│  - poll() / epoll() 事件循环               │
│  - 连接生命周期管理                        │
│  - 连接 I/O 分发                           │
└─────────────────────────────────────────────┘
         ↑             ↓            ↑
    ┌──────┴────────────────────────┴──────┐
    │                                       │
┌─────────────┐  ┌──────────────┐  ┌──────────────┐
│ Connection1 │  │ Connection2  │  │ ConnectionN  │
│             │  │              │  │              │
│ ┌─────────┐ │  │ ┌──────────┐ │  │ ┌──────────┐ │
│ │RxBuffer │ │  │ │ RxBuffer │ │  │ │RxBuffer  │ │
│ └────┬────┘ │  │ └────┬─────┘ │  │ └────┬─────┘ │
│      ↓      │  │      ↓       │  │      ↓       │
│ ┌─────────┐ │  │ ┌──────────┐ │  │ ┌──────────┐ │
│ │ Parser  │ │  │ │ Parser   │ │  │ │ Parser   │ │
│ │ (HSM)   │ │  │ │ (HSM)    │ │  │ │ (HSM)    │ │
│ └────┬────┘ │  │ └────┬─────┘ │  │ └────┬─────┘ │
│      ↓      │  │      ↓       │  │      ↓       │
│  on_message │  │ on_message  │  │ on_message  │
│      ↑      │  │      ↑       │  │      ↑       │
│ ┌─────────┐ │  │ ┌──────────┐ │  │ ┌──────────┐ │
│ │TxBuffer │ │  │ │ TxBuffer │ │  │ │TxBuffer  │ │
│ └────┬────┘ │  │ └────┬─────┘ │  │ └────┬─────┘ │
│      ↓      │  │      ↓       │  │      ↓       │
│  TCP Socket │  │ TCP Socket  │  │ TCP Socket  │
└─────────────┘  └──────────────┘  └──────────────┘
```

### 2.2 核心模块

| 模块 | 职责 | 数据结构 |
|------|------|---------|
| **Server** | Reactor 主循环、连接管理 | `vector<Connection>` |
| **Connection** | 单个连接的生命周期、状态转换 | Socket + RingBuffer × 2 + ProtocolHandler |
| **ProtocolHandler** | WebSocket 握手、帧解析逻辑 | 多态处理器（HandshakeState, OpenState 等） |
| **RingBuffer** | 固定大小循环缓冲，存储 TX/RX 数据 | 数组 + 读写指针 |
| **Utils** | Base64、SHA1、WebSocket 帧编解码 | 工具函数集合 |

---

## 3. 数据流分析

### 3.1 接收路径 (RX)

```
TCP Socket
    ↓ (handle_read)
RxBuffer.push()
    ↓ (on readable event)
ProtocolHandler::handle_data_received()
    ├─ HandshakeState: parse_handshake()
    │   ├─ 查找 \r\n\r\n
    │   ├─ 解析 Sec-WebSocket-Key
    │   ├─ 生成 Accept Key (SHA1 + Base64)
    │   ├─ 写入 TxBuffer (HTTP 101 response)
    │   └─ 转移 → OpenState
    └─ OpenState: parse_frames()
        ├─ 循环: parse_frame_header()
        ├─ 检查 payload 完整性
        ├─ 如果 masked: unmask_payload()
        ├─ 触发 on_message 回调
        └─ 从 RxBuffer advance()
```

### 3.2 发送路径 (TX)

```
Application: conn->send("msg")
    ↓
write_frame()
    ├─ 构造 WS 帧头
    ├─ 服务端不掩码 (mask=0)
    └─ TxBuffer.push()
        ↓
    poll() 返回 POLLOUT
        ↓
    handle_write()
        ├─ TxBuffer.peek()
        ├─ socket.write()
        └─ TxBuffer.advance()
            ↓
        TCP 发送到客户端
```

### 3.3 缓冲区不变式

```c++
// RingBuffer 始终保证:
// - 读指针 <= 写指针 (模 capacity)
// - count = 已缓冲字节数
// - available = capacity - count (可写空间)

// 关键操作:
// push(data, len): count >= len ⟹ 缓冲 ✓; 否则返回 false
// peek(data, max_len): 读 min(max_len, count) 字节，不移除
// advance(len): 移除前 len 字节
```

---

## 4. 协议状态机设计

### 4.1 状态定义

```
┌──────────────────────────────────────────────────────┐
│                    HANDSHAKING                        │
│  等待 HTTP GET + Upgrade 请求                        │
│  - 接收: 解析 HTTP 头，验证 Sec-WebSocket-Key      │
│  - 发送: HTTP 101 + Sec-WebSocket-Accept           │
│  - 超时: 5s (可配置)                                │
└────┬─────────────────────────────────────────────────┘
     │ (握手成功)
     ↓
┌──────────────────────────────────────────────────────┐
│                       OPEN                            │
│  WebSocket 连接建立，正常收发帧                      │
│  - 接收: 解析 WebSocket 帧，触发 on_message         │
│  - 发送: send() 打包数据为 WS 帧                    │
│  - Ping/Pong: 自动回复 Ping                         │
└────┬──────────────────────────────────────┬──────────┘
     │ (客户端或应用发起关闭)                │
     ↓                                      │
┌──────────────────────────────────────────────────────┐
│                     CLOSING                           │
│  已发送 Close 帧，等待对端 Close 帧或超时           │
│  - 接收: 等待 Close 帧，收到后立即关闭              │
│  - 发送: 不允许发送数据帧                            │
│  - 超时: 5s 后强制关闭                               │
└────┬──────────────────────────────────────────────────┘
     │
     ↓
┌──────────────────────────────────────────────────────┐
│                      CLOSED                           │
│  连接已关闭，回收资源                                │
│  - 所有 I/O 操作不允许                               │
│  - socket.close(), 从连接池移除                     │
└──────────────────────────────────────────────────────┘
```

### 4.2 事件模型

```cpp
// 事件类型
EvDataReceived   // Socket 有可读数据
EvSendRequest    // 应用调用 send()
EvClose          // 应用调用 close() 或对端 Close 帧
EvTimeout        // 握手/关闭超时 (可选)

// 状态转换表
State\Event      EvDataRx   EvSendReq   EvClose    EvTimeout
────────────────────────────────────────────────────────────
Handshaking      parse_hs   (error)     →Closed    →Closed
Open             parse_fr   write_frame →Closing   (no-op)
Closing          wait_close (ignore)    (no-op)    →Closed
Closed           (ignore)   (ignore)    (no-op)    (no-op)
```

---

## 5. 核心设计决策

### 5.1 为何使用 RingBuffer 而非 std::string/vector

**问题**: 原版 Simple-WebSocket-Server 使用 `std::stringstream` 或动态 `vector`, 导致:
- 内存碎片化 (多次 reallocate → 分散的堆块)
- 不可预测的时延 (GC/重分配)
- 嵌入式系统内存紧张

**方案**: 固定大小循环缓冲
```cpp
RingBuffer<uint8_t, 4096>  // 编译期固定大小
- push(): O(1), 无分配
- peek(): O(1), 无拷贝
- advance(): O(1)
```

**权衡**:
| 优势 | 劣势 |
|------|------|
| 确定性内存占用 | 需手动配置大小 |
| 无堆分配 | 缓冲区满时拒绝数据 |
| 缓存友好 (连续) | 不能自动扩展 |

### 5.2 为何选择单线程 Reactor 而非多线程

**嵌入式场景特征**:
- 核心数少 (1-4 核)
- 内存有限 (每线程需 1-2 MB 栈)
- 实时性要求 (避免上下文切换)

**单线程优势**:
```
No locks          → 无死锁、无竞态
No context switch → Cache locality 好
Linear scaling    → 与核心数独立
```

**多连接处理**:
- 小规模 (< 100): poll() 无压力
- 中等 (100-1000): epoll 替换 poll
- 大规模 (> 1000): 可分片 (每个工作线程维护独立的 poll 子集)

### 5.3 为何使用状态机而非 if-else 嵌套

**问题**: 握手/帧解析逻辑交织，难以维护
```cpp
// 反面教材
if (state == Handshaking) {
  if (has_data()) {
    if (valid_http_headers()) {
      state = Open;
      // ... 50 行握手逻辑
    }
  }
} else if (state == Open) {
  if (has_frame_header()) {
    if (has_payload()) {
      // ... 40 行帧解析逻辑
    }
  }
}  // 嵌套层级深，易出错
```

**方案**: 多态处理器
```cpp
// 每个状态是独立类，职责单一
class HandshakeState : public ProtocolHandler {
  void handle_data_received(Connection& conn) {
    // 仅处理握手逻辑
  }
};

class OpenState : public ProtocolHandler {
  void handle_data_received(Connection& conn) {
    // 仅处理帧解析
  }
};
```

### 5.4 WebSocket 握手实现

遵循 RFC 6455 §4.2.1:

```
客户端请求:
┌─────────────────────────────────────────────────────┐
GET / HTTP/1.1
Host: localhost:8080
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
└─────────────────────────────────────────────────────┘

服务器响应:
┌─────────────────────────────────────────────────────┐
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
└─────────────────────────────────────────────────────┘

Accept Key = Base64(SHA1(Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
```

**实现细节**:
```cpp
// Step 1: 提取客户端 Key
std::string client_key = parse_header("Sec-WebSocket-Key");

// Step 2: 计算 Accept Key
std::string magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
auto combined = client_key + magic_string;
auto sha1_hash = SHA1::compute(combined);
auto accept_key = Base64::encode(sha1_hash);

// Step 3: 构建 HTTP 101 响应
```

### 5.5 WebSocket 帧格式

```
  0 1 2 3 4 5 6 7 8 9 A B C D E F
┌─────────────────────────────────┐
│F|R|R|R|O|O|O|O| |M|L|L|L|L|L|L|  Byte 0-1
├─────────────────────────────────┤
│                                 │
│   Mask key (optional, 4 bytes)  │
│                                 │
├─────────────────────────────────┤
│                                 │
│   Payload data (variable)       │
│                                 │
└─────────────────────────────────┘

F     = FIN (1 = 最后一帧)
R     = Reserved (0)
O     = Opcode (0x1=Text, 0x2=Binary, 0x8=Close, 0x9=Ping, 0xA=Pong)
M     = Mask (客户端→服务器必须 1, 服务器→客户端必须 0)
L     = Payload length encoding:
        0-125 = actual length
        126   = next 2 bytes are length
        127   = next 8 bytes are length
```

**关键约定**:
- 客户端→服务器: 必须 masked (安全考虑)
- 服务器→客户端: 必须 unmasked
- 本实现: 服务端自动解掩码客户端帧，不对服务端帧掩码

---

## 6. 资源占用分析

### 6.1 内存占用 (per connection)

```
Component           Size
────────────────────────
Connection object   ~100 B
RxBuffer (4KB)      4,096 B
TxBuffer (8KB)      8,192 B
ProtocolHandler     ~100 B
handshake_buffer    ~256 B (握手时)
───────────────────────────
Total (steady)      ~12.5 KB
Peak (握手)         ~12.8 KB
```

**1000 连接**:
- 内存: 12.5 MB
- 文件描述符: 1000 + 1 (acceptor)

### 6.2 CPU 占用

**处理 1 条消息的操作数** (典型路径):
```
receive: socket.read() [1] + ringbuffer.push() [payload_len]
  → parse_frame_header() [2]
  → unmask_payload() [payload_len]
  → on_message() [user callback]
  → ringbuffer.advance() [1]

Total: O(payload_len) 字节操作, O(1) 结构操作
```

**poll() 开销**:
- 1000 连接: poll(1001) ≈ 1-2 ms (Linux 在 epoll 前的水位)
- epoll 改进: O(active events) 而非 O(total connections)

---

## 7. 错误处理与安全

### 7.1 缓冲区溢出

```cpp
// RingBuffer 防护
if (!rx_buffer_.push(data, len)) {
  log_error("RX buffer overflow");
  return false;  // 拒绝连接或关闭
}
```

恶意客户端发送超大帧 → 缓冲区满 → 连接关闭 ✓

### 7.2 握手超时

```cpp
// TODO: 实现握手超时 (5s)
// 在 Connection 中记录握手开始时间
// 在 Server 主循环检查:
//   time - handshake_start_time > 5s → close
```

### 7.3 无限循环防护

```cpp
// parse_frames() 防护
while (true) {
  if (!has_complete_frame()) break;  // 不够数据 → 等待
  // process frame
  ringbuffer.advance();  // 必须前进，否则无限循环
}
```

---

## 8. 性能优化技巧

### 8.1 零拷贝设计

- `peek()`: 获取指针，不复制数据
- `std::string_view`: 避免字符串临时副本
- 只在必要时掩码解掩码

### 8.2 缓存友好

```cpp
// 连续内存布局
class Connection {
  sockpp::tcp_socket socket_;              // 8 B
  RingBuffer<uint8_t, 4096> rx_buffer_;   // 4KB (cache line aligned)
  RingBuffer<uint8_t, 8192> tx_buffer_;   // 8KB
  // ... 其他字段
};
// 避免指针链，CPU 预取器友好
```

### 8.3 批量处理

parse_frames() 在单个 readable event 中处理所有完整帧，而非逐帧触发回调。

---

## 9. 扩展与集成

### 9.1 多线程支持 (未来)

分片模式:
```cpp
// 4 个 worker threads, 每个维护 250 连接
Server server1(port1);
Server server2(port2);
// ...
// 上层负载均衡器分发连接
```

### 9.2 TLS/SSL

建议使用反向代理:
```
Client (WSS)  →  nginx (TLS)  →  EWSS (WS)
```

### 9.3 消息压缩

可在应用层实现:
```cpp
server.on_message = [](const auto& conn, auto msg) {
  auto decompressed = decompress(msg);
  // process
};
```

---

## 10. 测试策略

### 10.1 单元测试

| 模块 | 用例 | 覆盖率目标 |
|------|------|----------|
| Base64 | encode/decode, edge cases | 100% |
| SHA1 | standard vectors | 100% |
| Frame parsing | header variants, large payloads | 100% |
| RingBuffer | push/peek/advance, overflow | 95%+ |
| Connection | handshake, frame handling, state transitions | 90%+ |

### 10.2 集成测试

```cpp
// 模拟客户端发送握手 + 消息 + 关闭
// 验证:
// - 接收消息正确
// - 状态转换正确
// - 资源清理完整
```

### 10.3 压力测试

```bash
# 使用 Artillery 或 wscat 脚本
# - 1000 并发连接
# - 持续发送消息
# - 测量内存、CPU、延迟
```

---

## 11. 已知限制与改进方向

| 限制项 | 原因 | 改进方向 |
|--------|------|---------|
| 无 TLS | 增加复杂度 | nginx 反向代理 |
| 无压缩 | 实现复杂 | 应用层压缩 |
| 单线程 | 简化设计 | 分片或异步 I/O |
| 固定 buffer | 嵌入式约束 | 可配置大小 |

---

## 12. 术语表

| 术语 | 定义 |
|------|------|
| **Reactor** | 基于事件循环的 I/O 多路复用设计模式 |
| **RingBuffer** | 循环缓冲区，写指针追上读指针时覆盖 |
| **Masking** | RFC 6455 要求客户端→服务器的帧加掩码，防止缓存中毒 |
| **FIN** | 帧最后一位，1 表示此帧是消息的最后一帧 |
| **Opcode** | 帧类型标识 (Text, Binary, Close, Ping, Pong, etc) |
| **HSM** | 层次状态机，支持子状态和转换条件 |

---

## 13. 参考资源

- RFC 6455: The WebSocket Protocol
  - https://tools.ietf.org/html/rfc6455

- sockpp Documentation
  - https://github.com/fpagliughi/sockpp

- POSIX poll(2)
  - https://man7.org/linux/man-pages/man2/poll.2.html

- Linux epoll(7)
  - https://man7.org/linux/man-pages/man7/epoll.7.html

---

**文档版本**: 1.0
**最后更新**: 2026-02-18
**维护者**: dgliu
