#include "ewss/connection.hpp"
#include <cerrno>
#include <chrono>
#include <iostream>
#include <sstream>

namespace ewss {

static uint64_t g_next_conn_id = 1;

Connection::Connection(sockpp::tcp_socket&& sock)
    : id_(g_next_conn_id++), socket_(std::move(sock)) {
  socket_.set_non_blocking(true);
  protocol_handler_ = std::make_unique<HandshakeState>();
}

Connection::~Connection() {
  if (socket_.is_open()) {
    socket_.close();
  }
}

ConnectionState Connection::get_state() const {
  return protocol_handler_->get_state();
}

bool Connection::handle_read() {
  uint8_t temp[kTempReadSize];
  ssize_t n = socket_.read(temp, sizeof(temp));

  if (n > 0) {
    // Append data to rx_buffer
    if (!rx_buffer_.push(temp, n)) {
      log_error("RX buffer overflow");
      return false;
    }

    // Let protocol handler process the data
    protocol_handler_->handle_data_received(*this);
    return true;
  } else if (n == 0) {
    // Connection closed by peer
    return false;
  } else {
    // Error
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      log_error("Read error: " + std::string(strerror(err)));
      return false;
    }
    return true;
  }
}

bool Connection::handle_write() {
  if (tx_buffer_.empty()) return true;

  uint8_t temp[kTempReadSize];
  size_t len = tx_buffer_.peek(temp, sizeof(temp));

  ssize_t n = socket_.write(temp, len);
  if (n > 0) {
    tx_buffer_.advance(n);
    return true;
  } else if (n < 0) {
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      log_error("Write error: " + std::string(strerror(err)));
      return false;
    }
  }
  return true;
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
  if (get_state() == ConnectionState::kClosed) return;

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
      protocol_handler_ = std::make_unique<HandshakeState>();
      break;
    case ConnectionState::kOpen:
      protocol_handler_ = std::make_unique<OpenState>();
      if (on_open) {
        on_open(shared_from_this());
      }
      break;
    case ConnectionState::kClosing:
      protocol_handler_ = std::make_unique<ClosingState>();
      break;
    case ConnectionState::kClosed:
      protocol_handler_ = std::make_unique<ClosedState>();
      if (on_close) {
        on_close(shared_from_this(), true);
      }
      break;
  }
}

bool Connection::parse_handshake() {
  // Read handshake data from RX buffer
  uint8_t temp[1024];
  size_t len = rx_buffer_.peek(temp, sizeof(temp));

  if (len == 0) return false;

  std::string data(reinterpret_cast<const char*>(temp), len);
  handshake_buffer_ += data;

  // Check if we have complete HTTP request (ends with \r\n\r\n)
  if (!is_handshake_complete(handshake_buffer_)) {
    return false;
  }

  // Remove consumed data from rx_buffer
  rx_buffer_.advance(len);

  // Parse HTTP headers
  std::istringstream iss(handshake_buffer_);
  std::string line;

  // Read request line
  if (!std::getline(iss, line)) return false;
  if (line.find("GET") == std::string::npos) return false;

  // Read headers
  sec_websocket_key_.clear();
  while (std::getline(iss, line)) {
    if (line == "\r" || line.empty()) break;

    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) continue;

    std::string header_name = line.substr(0, colon_pos);
    std::string header_value = line.substr(colon_pos + 1);

    // Trim whitespace
    header_value.erase(0, header_value.find_first_not_of(" \t\r\n"));
    header_value.erase(header_value.find_last_not_of(" \t\r\n") + 1);

    if (header_name == "Sec-WebSocket-Key") {
      sec_websocket_key_ = header_value;
    }
  }

  if (sec_websocket_key_.empty()) {
    return false;
  }

  // Generate accept key
  std::string accept_key = generate_accept_key(sec_websocket_key_);

  // Build HTTP 101 response
  std::string response =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " +
      accept_key +
      "\r\n"
      "\r\n";

  // Write response to TX buffer
  if (!tx_buffer_.push(reinterpret_cast<const uint8_t*>(response.data()),
                        response.size())) {
    log_error("TX buffer overflow during handshake");
    return false;
  }

  handshake_completed_ = true;
  handshake_buffer_.clear();

  return true;
}

void Connection::parse_frames() {
  while (true) {
    uint8_t temp[4096];
    size_t len = rx_buffer_.peek(temp, sizeof(temp));

    if (len == 0) break;

    std::string_view data(reinterpret_cast<const char*>(temp), len);
    ws::FrameHeader header;

    size_t header_size = ws::parse_frame_header(data, header);
    if (header_size == 0) break;  // Incomplete frame

    size_t total_frame_size = header_size + header.payload_len;
    if (len < total_frame_size) break;  // Incomplete payload

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
          on_message(shared_from_this(),
                      std::string_view(reinterpret_cast<const char*>(payload),
                                       payload_len));
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
        write_frame(std::string_view(reinterpret_cast<const char*>(payload),
                                      payload_len),
                    ws::OpCode::kPong);
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
  // Server doesn't mask outgoing frames
  auto frame = ws::encode_frame(opcode, payload, false);
  if (!tx_buffer_.push(frame.data(), frame.size())) {
    log_error("TX buffer overflow");
  }
}

void Connection::write_close_frame(uint16_t code) {
  uint8_t payload[2];
  payload[0] = (code >> 8) & 0xFF;
  payload[1] = code & 0xFF;

  std::string_view payload_view(reinterpret_cast<const char*>(payload), 2);
  auto frame = ws::encode_frame(ws::OpCode::kClose, payload_view, false);

  if (!tx_buffer_.push(frame.data(), frame.size())) {
    log_error("TX buffer overflow");
  }
}

bool Connection::is_handshake_complete(std::string_view data) const {
  return data.find("\r\n\r\n") != std::string::npos;
}

void Connection::log_error(const std::string& msg) {
  if (on_error) {
    on_error(shared_from_this(), msg);
  }
}

// ============================================================================
// State implementations
// ============================================================================

void HandshakeState::handle_data_received(Connection& conn) {
  if (conn.parse_handshake()) {
    conn.transition_to_state(ConnectionState::kOpen);
  }
}

void HandshakeState::handle_send_request(Connection& conn,
                                         std::string_view payload) {
  // Cannot send before handshake is complete
}

void HandshakeState::handle_close_request(Connection& conn, uint16_t code) {
  conn.socket_.close();
  conn.transition_to_state(ConnectionState::kClosed);
}

void OpenState::handle_data_received(Connection& conn) {
  conn.parse_frames();
}

void OpenState::handle_send_request(Connection& conn,
                                     std::string_view payload) {
  conn.write_frame(payload, ws::OpCode::kText);
}

void OpenState::handle_close_request(Connection& conn, uint16_t code) {
  conn.write_close_frame(code);
  conn.transition_to_state(ConnectionState::kClosing);
}

void ClosingState::handle_data_received(Connection& conn) {
  // Wait for close frame from peer or timeout
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
}

void ClosingState::handle_send_request(Connection& conn,
                                        std::string_view payload) {
  // Cannot send after close has been initiated
}

void ClosingState::handle_close_request(Connection& conn, uint16_t code) {
  // Already closing
}

void ClosedState::handle_data_received(Connection& conn) {
  // Connection is closed
}

void ClosedState::handle_send_request(Connection& conn,
                                       std::string_view payload) {
  // Connection is closed
}

void ClosedState::handle_close_request(Connection& conn, uint16_t code) {
  // Already closed
}

}  // namespace ewss
