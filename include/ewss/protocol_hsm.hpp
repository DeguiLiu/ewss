#ifndef EWSS_PROTOCOL_HSM_HPP_
#define EWSS_PROTOCOL_HSM_HPP_

#include "vocabulary.hpp"

#include <cstdint>

#include <functional>
#include <string_view>

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

  // Handle received data
  // Returns success() if data processed correctly, error() on protocol violation.
  virtual expected<void, ErrorCode> handle_data_received(Connection& conn) = 0;

  // Handle send request
  // Returns success() on successful frame encoding, error() if invalid state.
  virtual expected<void, ErrorCode> handle_send_request(Connection& conn, std::string_view payload) = 0;

  // Handle close request
  // Returns success() on close initiation, error() if invalid state.
  virtual expected<void, ErrorCode> handle_close_request(Connection& conn, uint16_t code) = 0;

  // Get current state
  virtual ConnectionState get_state() const = 0;
};

// ============================================================================
// State Implementations
// ============================================================================

class HandshakeState : public ProtocolHandler {
 public:
  expected<void, ErrorCode> handle_data_received(Connection& conn) override;
  expected<void, ErrorCode> handle_send_request(Connection& conn, std::string_view payload) override;
  expected<void, ErrorCode> handle_close_request(Connection& conn, uint16_t code) override;
  ConnectionState get_state() const override { return ConnectionState::kHandshaking; }
};

class OpenState : public ProtocolHandler {
 public:
  expected<void, ErrorCode> handle_data_received(Connection& conn) override;
  expected<void, ErrorCode> handle_send_request(Connection& conn, std::string_view payload) override;
  expected<void, ErrorCode> handle_close_request(Connection& conn, uint16_t code) override;
  ConnectionState get_state() const override { return ConnectionState::kOpen; }
};

class ClosingState : public ProtocolHandler {
 public:
  expected<void, ErrorCode> handle_data_received(Connection& conn) override;
  expected<void, ErrorCode> handle_send_request(Connection& conn, std::string_view payload) override;
  expected<void, ErrorCode> handle_close_request(Connection& conn, uint16_t code) override;
  ConnectionState get_state() const override { return ConnectionState::kClosing; }
};

class ClosedState : public ProtocolHandler {
 public:
  expected<void, ErrorCode> handle_data_received(Connection& conn) override;
  expected<void, ErrorCode> handle_send_request(Connection& conn, std::string_view payload) override;
  expected<void, ErrorCode> handle_close_request(Connection& conn, uint16_t code) override;
  ConnectionState get_state() const override { return ConnectionState::kClosed; }
};

}  // namespace ewss

#endif  // EWSS_PROTOCOL_HSM_HPP_
