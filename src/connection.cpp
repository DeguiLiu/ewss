#include "ewss/connection.hpp"

#include <cerrno>

#include <chrono>
#include <iostream>

namespace ewss {

// Static protocol handler instances (zero heap allocation on state transitions)
static HandshakeState g_handshake_state;
static OpenState g_open_state;
static ClosingState g_closing_state;
static ClosedState g_closed_state;

static uint64_t g_next_conn_id = 1;

Connection::Connection(sockpp::tcp_socket&& sock) : id_(g_next_conn_id++), socket_(std::move(sock)) {
  socket_.set_non_blocking(true);
  protocol_handler_ = &g_handshake_state;
}

Connection::Connection(int fd) : id_(g_next_conn_id++), socket_(fd) {
  socket_.set_non_blocking(true);
  protocol_handler_ = &g_handshake_state;
}

Connection::~Connection() {
  if (socket_.is_open()) {
    socket_.close();
  }
}

ConnectionState Connection::get_state() const {
  return protocol_handler_->get_state();
}

expected<void, ErrorCode> Connection::handle_read() {
  // Zero-copy read: readv directly into RingBuffer's writable regions
  struct iovec iov[2];
  size_t iov_count = rx_buffer_.fill_iovec_write(iov, 2);

  if (iov_count == 0) {
    last_error_code_ = ErrorCode::kBufferFull;
    log_error("RX buffer full");
    return expected<void, ErrorCode>::error(ErrorCode::kBufferFull);
  }

  ssize_t n = ::readv(socket_.handle(), iov, static_cast<int>(iov_count));

  if (n > 0) {
    rx_buffer_.commit_write(static_cast<size_t>(n));
    protocol_handler_->handle_data_received(*this);
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  } else if (n == 0) {
    last_error_code_ = ErrorCode::kConnectionClosed;
    return expected<void, ErrorCode>::error(ErrorCode::kConnectionClosed);
  } else {
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      last_error_code_ = ErrorCode::kSocketError;
      log_error("Read error");
      return expected<void, ErrorCode>::error(ErrorCode::kSocketError);
    }
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  }
}

expected<void, ErrorCode> Connection::handle_write() {
  if (tx_buffer_.empty()) {
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  }

  uint8_t temp[kTempReadSize];
  size_t len = tx_buffer_.peek(temp, sizeof(temp));

  ssize_t n = socket_.write(temp, len);
  if (n > 0) {
    tx_buffer_.advance(n);
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  } else if (n < 0) {
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      last_error_code_ = ErrorCode::kSocketError;
      log_error("Write error: " + std::string(strerror(err)));
      return expected<void, ErrorCode>::error(ErrorCode::kSocketError);
    }
  }
  last_error_code_ = ErrorCode::kOk;
  return expected<void, ErrorCode>::success();
}

expected<void, ErrorCode> Connection::handle_write_vectored() {
  if (tx_buffer_.empty()) {
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  }

  // Use writev for zero-copy scatter/gather IO
  struct iovec iov[2];
  size_t iov_count = tx_buffer_.fill_iovec(iov, 2);

  if (iov_count == 0) {
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  }

  ssize_t n = ::writev(socket_.handle(), iov, static_cast<int>(iov_count));
  if (n > 0) {
    tx_buffer_.advance(static_cast<size_t>(n));
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  } else if (n < 0) {
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      last_error_code_ = ErrorCode::kSocketError;
      log_error("writev error: " + std::string(strerror(err)));
      return expected<void, ErrorCode>::error(ErrorCode::kSocketError);
    }
  }
  last_error_code_ = ErrorCode::kOk;
  return expected<void, ErrorCode>::success();
}

void Connection::send(std::string_view payload, bool binary) {
  if (get_state() != ConnectionState::kOpen) {
    log_error("Cannot send: connection not open");
    return;
  }

  ws::OpCode opcode = binary ? ws::OpCode::kBinary : ws::OpCode::kText;
  write_frame(payload, opcode);
}

void Connection::close(uint16_t code) {
  if (get_state() == ConnectionState::kClosed)
    return;

  if (get_state() == ConnectionState::kOpen) {
    write_close_frame(code);
    transition_to_state(ConnectionState::kClosing);
  } else {
    transition_to_state(ConnectionState::kClosed);
    socket_.close();
  }
}

bool Connection::is_closed() const {
  return !socket_.is_open() || get_state() == ConnectionState::kClosed;
}

void Connection::transition_to_state(ConnectionState state) {
  switch (state) {
    case ConnectionState::kHandshaking:
      protocol_handler_ = &g_handshake_state;
      break;
    case ConnectionState::kOpen:
      protocol_handler_ = &g_open_state;
      if (on_open) {
        on_open(shared_from_this());
      }
      break;
    case ConnectionState::kClosing:
      protocol_handler_ = &g_closing_state;
      break;
    case ConnectionState::kClosed:
      protocol_handler_ = &g_closed_state;
      if (on_close) {
        on_close(shared_from_this(), true);
      }
      break;
  }
}

expected<void, ErrorCode> Connection::parse_handshake() {
  // Zero-copy handshake: peek directly into RingBuffer, parse with string_view
  uint8_t temp[1024];
  size_t len = rx_buffer_.peek(temp, sizeof(temp));

  if (len == 0) {
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  std::string_view data(reinterpret_cast<const char*>(temp), len);

  // Check if we have complete HTTP request (ends with \r\n\r\n)
  size_t end_pos = data.find("\r\n\r\n");
  if (end_pos == std::string_view::npos) {
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  // Total handshake size including the terminator
  size_t handshake_size = end_pos + 4;

  // Validate request line starts with "GET "
  if (data.substr(0, 4) != "GET ") {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  // Extract Sec-WebSocket-Key using zero-copy string_view scanning
  constexpr std::string_view kKeyHeader = "Sec-WebSocket-Key: ";
  size_t key_pos = data.find(kKeyHeader);
  if (key_pos == std::string_view::npos) {
    // Try case-insensitive fallback (lowercase)
    constexpr std::string_view kKeyHeaderLower = "sec-websocket-key: ";
    key_pos = data.find(kKeyHeaderLower);
  }

  if (key_pos == std::string_view::npos) {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  // Find the value: skip header name, read until \r\n
  size_t value_start = key_pos + kKeyHeader.size();
  size_t value_end = data.find("\r\n", value_start);
  if (value_end == std::string_view::npos) {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  std::string_view ws_key = data.substr(value_start, value_end - value_start);

  // Trim trailing whitespace from key
  while (!ws_key.empty() && (ws_key.back() == ' ' || ws_key.back() == '\t')) {
    ws_key.remove_suffix(1);
  }

  if (ws_key.empty()) {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  // Consume handshake data from RX buffer
  rx_buffer_.advance(handshake_size);

  // Generate accept key (only allocation needed for SHA1+Base64)
  std::string accept_key = generate_accept_key(ws_key);

  // Build HTTP 101 response on stack
  // "HTTP/1.1 101 Switching Protocols\r\n" = 34
  // "Upgrade: websocket\r\n" = 20
  // "Connection: Upgrade\r\n" = 21
  // "Sec-WebSocket-Accept: " + key + "\r\n\r\n" = 22 + 28 + 4 = 54
  // Total max ~130 bytes, fits on stack
  char response_buf[256];
  int response_len = snprintf(response_buf, sizeof(response_buf),
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n"
      "\r\n",
      accept_key.c_str());

  if (response_len <= 0 || static_cast<size_t>(response_len) >= sizeof(response_buf)) {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  // Write response to TX buffer
  if (!tx_buffer_.push(reinterpret_cast<const uint8_t*>(response_buf),
                       static_cast<size_t>(response_len))) {
    last_error_code_ = ErrorCode::kBufferFull;
    return expected<void, ErrorCode>::error(ErrorCode::kBufferFull);
  }

  handshake_completed_ = true;
  sec_websocket_key_.clear();

  last_error_code_ = ErrorCode::kOk;
  return expected<void, ErrorCode>::success();
}

void Connection::parse_frames() {
  while (true) {
    uint8_t temp[4096];
    size_t len = rx_buffer_.peek(temp, sizeof(temp));

    if (len == 0)
      break;

    std::string_view data(reinterpret_cast<const char*>(temp), len);
    ws::FrameHeader header;

    size_t header_size = ws::parse_frame_header(data, header);
    if (header_size == 0)
      break;  // Incomplete frame

    size_t total_frame_size = header_size + header.payload_len;
    if (len < total_frame_size)
      break;  // Incomplete payload

    // Extract mask key if present
    const uint8_t* mask_key = nullptr;
    if (header.masked) {
      mask_key = temp + (header_size - 4);
    }

    // Extract payload
    uint8_t* payload = temp + header_size;
    size_t payload_len = header.payload_len;

    // Unmask if needed
    if (header.masked) {
      unmask_payload(payload, payload_len, mask_key);
    }

    // Process frame based on opcode
    switch (header.opcode) {
      case ws::OpCode::kText:
      case ws::OpCode::kBinary:
        if (on_message) {
          on_message(shared_from_this(), std::string_view(reinterpret_cast<const char*>(payload), payload_len));
        }
        break;

      case ws::OpCode::kClose:
        if (on_close) {
          on_close(shared_from_this(), false);
        }
        transition_to_state(ConnectionState::kClosed);
        socket_.close();
        return;

      case ws::OpCode::kPing:
        // Send pong frame back
        write_frame(std::string_view(reinterpret_cast<const char*>(payload), payload_len), ws::OpCode::kPong);
        break;

      case ws::OpCode::kPong:
        // Ignore pong frames
        break;

      default:
        break;
    }

    // Remove processed frame from RX buffer
    rx_buffer_.advance(total_frame_size);
  }
}

void Connection::write_frame(std::string_view payload, ws::OpCode opcode) {
  // Zero-allocation frame write: header on stack, payload direct to TxBuffer
  uint8_t header_buf[14];
  size_t header_len = ws::encode_frame_header(
      header_buf, opcode, payload.size(), false);

  if (!tx_buffer_.push(header_buf, header_len)) {
    log_error("TX buffer overflow (header)");
    return;
  }
  if (!payload.empty()) {
    if (!tx_buffer_.push(
            reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) {
      log_error("TX buffer overflow (payload)");
    }
  }
}

void Connection::write_close_frame(uint16_t code) {
  uint8_t close_payload[2];
  close_payload[0] = static_cast<uint8_t>((code >> 8) & 0xFF);
  close_payload[1] = static_cast<uint8_t>(code & 0xFF);

  uint8_t header_buf[14];
  size_t header_len = ws::encode_frame_header(
      header_buf, ws::OpCode::kClose, 2, false);

  if (!tx_buffer_.push(header_buf, header_len)) {
    log_error("TX buffer overflow (close header)");
    return;
  }
  if (!tx_buffer_.push(close_payload, 2)) {
    log_error("TX buffer overflow (close payload)");
  }
}

bool Connection::is_handshake_complete(std::string_view data) const {
  return data.find("\r\n\r\n") != std::string::npos;
}

void Connection::log_error(const std::string& /* msg */) {
  // Placeholder: integrate with newosp async logger in future
}

// ============================================================================
// State implementations
// ============================================================================

expected<void, ErrorCode> HandshakeState::handle_data_received(Connection& conn) {
  auto result = conn.parse_handshake();
  if (result.has_value()) {
    conn.transition_to_state(ConnectionState::kOpen);
    return expected<void, ErrorCode>::success();
  }
  return result;
}

expected<void, ErrorCode> HandshakeState::handle_send_request(
    Connection& /* conn */, std::string_view /* payload */) {
  return expected<void, ErrorCode>::error(ErrorCode::kInvalidState);
}

expected<void, ErrorCode> HandshakeState::handle_close_request(
    Connection& conn, uint16_t /* code */) {
  conn.socket_.close();
  conn.transition_to_state(ConnectionState::kClosed);
  return expected<void, ErrorCode>::success();
}

expected<void, ErrorCode> OpenState::handle_data_received(Connection& conn) {
  conn.parse_frames();
  return expected<void, ErrorCode>::success();
}

expected<void, ErrorCode> OpenState::handle_send_request(
    Connection& conn, std::string_view payload) {
  conn.write_frame(payload, ws::OpCode::kText);
  return expected<void, ErrorCode>::success();
}

expected<void, ErrorCode> OpenState::handle_close_request(
    Connection& conn, uint16_t code) {
  conn.write_close_frame(code);
  conn.transition_to_state(ConnectionState::kClosing);
  return expected<void, ErrorCode>::success();
}

expected<void, ErrorCode> ClosingState::handle_data_received(Connection& conn) {
  uint8_t temp[1024];
  size_t len = conn.rx_buffer_.peek(temp, sizeof(temp));

  if (len > 0) {
    std::string_view data(reinterpret_cast<const char*>(temp), len);
    ws::FrameHeader header;
    size_t header_size = ws::parse_frame_header(data, header);

    if (header_size > 0 && header.opcode == ws::OpCode::kClose) {
      conn.transition_to_state(ConnectionState::kClosed);
      conn.socket_.close();
    }
  }
  return expected<void, ErrorCode>::success();
}

expected<void, ErrorCode> ClosingState::handle_send_request(
    Connection& /* conn */, std::string_view /* payload */) {
  return expected<void, ErrorCode>::error(ErrorCode::kInvalidState);
}

expected<void, ErrorCode> ClosingState::handle_close_request(
    Connection& /* conn */, uint16_t /* code */) {
  return expected<void, ErrorCode>::success();
}

expected<void, ErrorCode> ClosedState::handle_data_received(
    Connection& /* conn */) {
  return expected<void, ErrorCode>::error(ErrorCode::kConnectionClosed);
}

expected<void, ErrorCode> ClosedState::handle_send_request(
    Connection& /* conn */, std::string_view /* payload */) {
  return expected<void, ErrorCode>::error(ErrorCode::kConnectionClosed);
}

expected<void, ErrorCode> ClosedState::handle_close_request(
    Connection& /* conn */, uint16_t /* code */) {
  return expected<void, ErrorCode>::error(ErrorCode::kConnectionClosed);
}

}  // namespace ewss
