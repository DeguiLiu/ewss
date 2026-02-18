#pragma once

#include <cstdint>
#include <string_view>
#include <functional>

namespace ewss {

// Forward declarations
class Connection;

// ============================================================================
// Events
// ============================================================================

struct EvDataReceived {
  // Socket 有新数据到达 RxBuffer
};

struct EvSendRequest {
  // 用户请求发送数据
  std::string_view payload;
  bool is_binary;
};

struct EvClose {
  // 请求关闭连接
  uint16_t code;
};

struct EvTimeout {
  // 握手超时
};

// ============================================================================
// States (HSM states representation)
// ============================================================================

enum class ConnectionState {
  kHandshaking,  // 等待 HTTP 握手请求
  kOpen,         // WebSocket 连接已建立
  kClosing,      // 正在关闭
  kClosed        // 已关闭
};

// ============================================================================
// Protocol Handler (delegates parsing logic)
// ============================================================================

class ProtocolHandler {
 public:
  virtual ~ProtocolHandler() = default;

  // 处理接收数据
  virtual void handle_data_received(Connection& conn) = 0;

  // 处理发送请求
  virtual void handle_send_request(Connection& conn,
                                    std::string_view payload) = 0;

  // 处理关闭请求
  virtual void handle_close_request(Connection& conn, uint16_t code) = 0;

  // 获取当前状态
  virtual ConnectionState get_state() const = 0;
};

// ============================================================================
// State Implementations
// ============================================================================

class HandshakeState : public ProtocolHandler {
 public:
  void handle_data_received(Connection& conn) override;
  void handle_send_request(Connection& conn, std::string_view payload) override;
  void handle_close_request(Connection& conn, uint16_t code) override;
  ConnectionState get_state() const override { return ConnectionState::kHandshaking; }
};

class OpenState : public ProtocolHandler {
 public:
  void handle_data_received(Connection& conn) override;
  void handle_send_request(Connection& conn, std::string_view payload) override;
  void handle_close_request(Connection& conn, uint16_t code) override;
  ConnectionState get_state() const override { return ConnectionState::kOpen; }
};

class ClosingState : public ProtocolHandler {
 public:
  void handle_data_received(Connection& conn) override;
  void handle_send_request(Connection& conn, std::string_view payload) override;
  void handle_close_request(Connection& conn, uint16_t code) override;
  ConnectionState get_state() const override { return ConnectionState::kClosing; }
};

class ClosedState : public ProtocolHandler {
 public:
  void handle_data_received(Connection& conn) override;
  void handle_send_request(Connection& conn, std::string_view payload) override;
  void handle_close_request(Connection& conn, uint16_t code) override;
  ConnectionState get_state() const override { return ConnectionState::kClosed; }
};

}  // namespace ewss
