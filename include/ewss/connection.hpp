#ifndef EWSS_CONNECTION_HPP_
#define EWSS_CONNECTION_HPP_

#include "protocol_hsm.hpp"
#include "utils.hpp"
#include "vocabulary.hpp"

#include <cstdint>
#include <cstring>

#include <array>
#include <functional>
#include <memory>
#include <sockpp/tcp_socket.h>
#include <string>
#include <string_view>
#include <sys/uio.h>  // writev
#include <vector>

namespace ewss {

// ============================================================================
// RingBuffer (Circular buffer for fixed memory allocation)
// ============================================================================

template <typename T, size_t Size>
class alignas(kCacheLine) RingBuffer {
 public:
  static constexpr size_t kCapacity = Size;

  RingBuffer() = default;

  // Write data to buffer
  bool push(const T* data, size_t len) {
    if (available() < len)
      return false;
    for (size_t i = 0; i < len; ++i) {
      buffer_[write_idx_] = data[i];
      write_idx_ = (write_idx_ + 1) % kCapacity;
    }
    count_ += len;
    return true;
  }

  // Read data from buffer without removing
  size_t peek(T* data, size_t max_len) const {
    size_t len = std::min(max_len, count_);
    size_t idx = read_idx_;
    for (size_t i = 0; i < len; ++i) {
      data[i] = buffer_[idx];
      idx = (idx + 1) % kCapacity;
    }
    return len;
  }

  // Remove data from buffer
  void advance(size_t len) {
    if (len > count_)
      len = count_;
    read_idx_ = (read_idx_ + len) % kCapacity;
    count_ -= len;
  }

  // Get size of readable data
  size_t size() const { return count_; }

  // Get available space
  size_t available() const { return kCapacity - count_; }

  // Check if empty
  bool empty() const { return count_ == 0; }

  // Clear buffer
  void clear() {
    read_idx_ = 0;
    write_idx_ = 0;
    count_ = 0;
  }

  // Get data as string_view (only works if data is contiguous)
  std::string_view view() const {
    if (empty())
      return {};
    return std::string_view(reinterpret_cast<const char*>(buffer_.data() + read_idx_), count_);
  }

  // Fill iovec for writev (scatter/gather IO, zero-copy send)
  // Returns number of iovec entries filled (1 or 2)
  size_t fill_iovec(struct iovec* iov, size_t max_iov) const {
    if (empty() || max_iov == 0) return 0;

    size_t contiguous = kCapacity - read_idx_;
    if (contiguous >= count_) {
      // Data is contiguous
      iov[0].iov_base = const_cast<T*>(buffer_.data() + read_idx_);
      iov[0].iov_len = count_;
      return 1;
    }

    if (max_iov < 2) {
      // Only one iovec available, return first chunk
      iov[0].iov_base = const_cast<T*>(buffer_.data() + read_idx_);
      iov[0].iov_len = contiguous;
      return 1;
    }

    // Data wraps around: two chunks
    iov[0].iov_base = const_cast<T*>(buffer_.data() + read_idx_);
    iov[0].iov_len = contiguous;
    iov[1].iov_base = const_cast<T*>(buffer_.data());
    iov[1].iov_len = count_ - contiguous;
    return 2;
  }

 private:
  alignas(kCacheLine) std::array<T, kCapacity> buffer_{};
  size_t read_idx_ = 0;
  size_t write_idx_ = 0;
  size_t count_ = 0;
};

// ============================================================================
// Connection (Manages socket, buffers, and protocol state)
// ============================================================================

class alignas(kCacheLine) Connection : public std::enable_shared_from_this<Connection> {
 public:
  // Static configuration
  static constexpr size_t kRxBufferSize = 4096;
  static constexpr size_t kTxBufferSize = 8192;
  static constexpr size_t kTempReadSize = 512;
  static constexpr size_t kHandshakeTimeout = 5000;  // ms

  using ConnPtr = std::shared_ptr<Connection>;

  explicit Connection(sockpp::tcp_socket&& sock);
  explicit Connection(int fd);  // Native socket fd constructor
  ~Connection();

  // --- Reactor I/O API ---

  // Handle readable event from poll/epoll
  // Returns success() on successful read, error(kSocketError) on socket error,
  // error(kBufferFull) if RX buffer is full, error(kConnectionClosed) on peer close.
  expected<void, ErrorCode> handle_read();

  // Handle writable event from poll/epoll
  // Returns success() on successful write, error(kSocketError) on socket error,
  // error(kBufferEmpty) if nothing to send.
  expected<void, ErrorCode> handle_write();

  // --- User API ---

  void send(std::string_view payload) { send(payload, false); }

  void send_binary(std::string_view payload) { send(payload, true); }

  void close(uint16_t code = 1000);

  bool is_closed() const;

  bool has_data_to_send() const { return !tx_buffer_.empty(); }

  // writev-based zero-copy send (scatter/gather IO)
  expected<void, ErrorCode> handle_write_vectored();

  int get_fd() const { return socket_.handle(); }

  uint64_t get_id() const { return id_; }

  // --- Callbacks ---

  std::function<void(const ConnPtr&)> on_open;
  std::function<void(const ConnPtr&, std::string_view)> on_message;
  std::function<void(const ConnPtr&, bool)> on_close;
  std::function<void(const ConnPtr&)> on_error;

  // --- Getters ---

  ConnectionState get_state() const;

  // Get last error code (cached from most recent operation)
  ErrorCode get_last_error() const { return last_error_code_; }

 private:
  friend class HandshakeState;
  friend class OpenState;
  friend class ClosingState;
  friend class ClosedState;

  uint64_t id_;
  sockpp::tcp_socket socket_;
  RingBuffer<uint8_t, kRxBufferSize> rx_buffer_;
  RingBuffer<uint8_t, kTxBufferSize> tx_buffer_;

  std::unique_ptr<ProtocolHandler> protocol_handler_;

  // --- Protocol state ---

  bool handshake_completed_ = false;
  std::string sec_websocket_key_;

  // --- Error tracking ---
  ErrorCode last_error_code_ = ErrorCode::kOk;

  // --- Helper functions ---

  void send(std::string_view payload, bool binary);

  void transition_to_state(ConnectionState state);

  // Parse HTTP upgrade request during handshake
  // Returns success() on complete handshake, error(kHandshakeFailed) if invalid.
  expected<void, ErrorCode> parse_handshake();

  // Parse WebSocket frames during open state
  void parse_frames();

  // Send WebSocket frame
  void write_frame(std::string_view payload, ws::OpCode opcode);

  // Send close frame
  void write_close_frame(uint16_t code);

  // Generate WebSocket accept key
  static std::string generate_accept_key(std::string_view client_key);

  // Check if handshake data is complete
  bool is_handshake_complete(std::string_view data) const;

  // Unmask payload data
  static void unmask_payload(uint8_t* payload, size_t len, const uint8_t* mask_key);

  // Log helper
  void log_error(const std::string& msg);
};

// ============================================================================
// Inline implementations
// ============================================================================

inline std::string Connection::generate_accept_key(std::string_view client_key) {
  constexpr std::string_view kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string key(client_key);
  key.append(kMagic);

  auto hash = SHA1::compute(reinterpret_cast<const uint8_t*>(key.data()), key.size());
  return Base64::encode(hash.data(), hash.size());
}

inline void Connection::unmask_payload(uint8_t* payload, size_t len, const uint8_t* mask_key) {
  for (size_t i = 0; i < len; ++i) {
    payload[i] ^= mask_key[i % 4];
  }
}

}  // namespace ewss

#endif  // EWSS_CONNECTION_HPP_
