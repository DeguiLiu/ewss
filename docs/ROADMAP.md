# EWSS 改进路线图（P0 ✓ -> P1 -> P2）

**当前状态**: P0 完成 (v0.2.0)
**更新时间**: 2026-02-18

---

## P0 阶段（已完成 ✓）

**目标**: 集成 newosp 编码标准，建立规范基础

### 完成项

- ✓ 复制 newosp .ai/ 目录（.clang-format, .clang-tidy, CPPLINT.cfg）
- ✓ 创建 vocabulary.hpp：expected<V,E>, optional<T>, FixedString, FixedVector, FixedFunction, function_ref, ScopeGuard
- ✓ 定义 ErrorCode 枚举用于类型安全的错误处理
- ✓ 更新 CMakeLists.txt (v0.2.0) 依赖配置
- ✓ 创建 .ai/ 目录并建立符号链接

**提交**: c0acbf7 (P0: Integrate newosp coding standards and vocabulary types)

---

## P1 阶段（进行中）

**目标**: 完善协议处理、错误处理、回压机制

### 计划内容

#### 1.1 错误处理重构（预计 2-3h）

将所有 `bool` 返回值替换为 `expected<void, ErrorCode>`：

```cpp
// 之前 (不够明确)
bool Connection::handle_read() {
  if (!socket_.is_open()) return false;  // 哪种错误？
}

// 之后 (明确的错误码)
expected<void, ErrorCode> Connection::handle_read() {
  if (!socket_.is_open()) {
    return expected<void, ErrorCode>::error(ErrorCode::kSocketError);
  }
  return expected<void, ErrorCode>::success();
}
```

**文件修改**:
- `connection.hpp/cpp`: 所有 I/O 方法返回 expected
- `protocol_hsm.hpp/cpp`: 状态转换返回 expected
- `server.hpp/cpp`: 连接管理方法返回 expected

**测试**: 新增单元测试验证错误码传播

---

#### 1.2 回压（Backpressure）机制（预计 2-3h）

当 TxBuffer 满时，实现明确的回压策略：

```cpp
// 新增 API
class Connection {
  // 返回是否成功入队
  expected<void, ErrorCode> send(std::string_view payload);

  // 回调：缓冲区满，应用应暂停发送
  function_ref<void(ConnectionState)> on_backpressure;

  // 回调：缓冲区有空间，可恢复发送
  function_ref<void(ConnectionState)> on_resume;

  // 查询缓冲区状态
  uint32_t get_tx_buffer_usage() const;  // 返回 0-100%
};
```

**高水位/低水位阈值**（可配置）:
- High Water Mark: TxBuffer > 80% → 触发 on_backpressure
- Low Water Mark: TxBuffer < 50% → 触发 on_resume

**实现位置**: `connection.hpp` + `src/connection.cpp`

---

#### 1.3 完整协议边界测试（预计 2h）

补充握手和帧解析的边界测试：

```cpp
// tests/test_protocol_edge_cases.cpp

TEST_CASE("Handshake: incomplete header", "[protocol]") {
  // 只发送 "GET / HTTP" 不发送完整 \r\n\r\n
  // 期望: 握手状态保持，等待更多数据
}

TEST_CASE("Frame parsing: large payload", "[protocol]") {
  // 发送 127+ 字节的 WebSocket 帧
  // 验证: 正确解析 2/8 字节长度编码
}

TEST_CASE("Frame parsing: fragmented frame", "[protocol]") {
  // 帧分多个 TCP 包到达
  // 验证: 缓冲区能正确处理分片
}

TEST_CASE("Masking: RFC 6455 test vectors", "[protocol]") {
  // RFC 6455 Section 5.7 test vectors
  // 验证掩码解码正确性
}
```

**添加文件**: `tests/test_protocol_edge_cases.cpp`

---

#### 1.4 连接生命周期改进（预计 1-2h）

优化状态转换和超时处理：

```cpp
// 新增连接超时管理
class Connection {
  static constexpr uint32_t kHandshakeTimeoutMs = 5000;
  static constexpr uint32_t kClosingTimeoutMs = 5000;

  // 返回距超时剩余时间（ms），0 = 已超时
  uint32_t get_remaining_timeout_ms() const;

  // 触发超时回调
  function_ref<void()> on_timeout;
};

// 在 Server 主循环检查超时
for (auto& conn : connections_) {
  if (conn->get_remaining_timeout_ms() == 0) {
    on_timeout_handler(conn);
  }
}
```

---

### P1 验收标准

- [ ] 所有 bool 返回值替换为 expected
- [ ] TxBuffer 回压机制实现并测试
- [ ] 协议边界测试覆盖率 > 90%
- [ ] clang-tidy 无 ERROR 级别警告
- [ ] 单元测试通过率 100%

### P1 时间表

| 子任务 | 预计时间 | 状态 |
|--------|---------|------|
| 1.1 错误处理 | 2-3h | pending |
| 1.2 回压机制 | 2-3h | pending |
| 1.3 边界测试 | 2h | pending |
| 1.4 超时管理 | 1-2h | pending |
| 集成测试 | 1h | pending |
| **合计** | **8-10h** | |

---

## P2 阶段（设计中）

**目标**: TLS 支持、性能优化、文档完善、RT-Thread 移植

### 计划内容

#### 2.1 TLS 模块化设计（预计 3-4h）

定义 `ITlsAdapter` 接口：

```cpp
class ITlsAdapter {
 public:
  virtual ~ITlsAdapter() = default;

  // 非阻塞握手
  virtual expected<void, ErrorCode> do_handshake() = 0;

  // 加密读/写
  virtual expected<size_t, ErrorCode> read(uint8_t* buf, size_t len) = 0;
  virtual expected<size_t, ErrorCode> write(const uint8_t* buf, size_t len) = 0;

  // 状态查询
  virtual bool is_handshake_complete() const = 0;
};

// 两种实现
class NoTlsAdapter : public ITlsAdapter { /* ... */ };  // 裸 TCP
class MbedTlsAdapter : public ITlsAdapter { /* ... */ };  // MbedTLS
```

CMake 选项:
```bash
cmake .. -DENABLE_TLS=ON -DTLS_IMPL=mbedtls
```

#### 2.2 性能基准与优化（预计 3-4h）

```cpp
// examples/benchmark.cpp
// 对标:
// - ws4j (Go)
// - websocketpp (C++)
// - libwebsockets (C)

void benchmark_throughput() {
  // 1000 个连接，每秒 1 条消息
  // 测量延迟、吞吐、内存占用
}

void benchmark_large_payload() {
  // 1MB+ 消息处理能力
}

void benchmark_memory_profile() {
  // Valgrind profiling，验证无内存泄漏
}
```

#### 2.3 文档补充（预计 2-3h）

- API 参考文档补充
- 性能报告与优化建议
- RT-Thread 移植指南
- 常见问题 (FAQ)

#### 2.4 RT-Thread 兼容性验证（预计 2-3h）

- POSIX 层适配检查
- 移除 Linux 特定 syscall (epoll → poll)
- 静态内存预配置示例

---

## 技术债与后续计划

| 项 | 优先级 | 工作量 | 备注 |
|----|--------|--------|------|
| 事件驱动抽象接口 (IReactor) | P0-next | 3-4h | 支持 select/epoll/RTOS |
| 静态连接池 (FixedVector) | P1-next | 2h | 配置最大连接数 |
| 日志集成 (loghelper) | P1 | 1-2h | 观测性改进 |
| WebSocket 扩展 (permessage-deflate) | P2+ | 5-6h | 消息压缩 |

---

## 如何参与

如果您希望加速某个阶段的开发：

1. **指定优先级**: 若 P1 中某项更重要，请告知
2. **提供反馈**: 协议检查、错误码设计等
3. **并行开发**: P1 可能 3-5 个子任务并行（使用 sub-agent）

---

## 版本对应表

| 版本 | 阶段 | 主要功能 | 发布计划 |
|------|------|---------|---------|
| v0.1.0-alpha | P0-init | 基础 Reactor + 帧编解码 | ✓ 已发布 |
| v0.2.0-beta | P0 | newosp 标准集成 + vocabulary | ✓ 已提交 |
| v0.3.0-rc1 | P1 | 错误处理 + 回压 + 超时 | 预计 1-2w |
| v0.4.0 | P1+P2 | TLS + 性能优化 | 预计 3-4w |
| v1.0.0 | 稳定 | 生产级别 + 文档完善 | 预计 2m |

---

**下一步**: 确认 P1 优先级，开始并行实现子任务。
