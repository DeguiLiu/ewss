#pragma once

#include "connection.hpp"
#include "vocabulary.hpp"

#include <cstdint>

#include <algorithm>
#include <functional>
#include <memory>
#include <poll.h>
#include <vector>

namespace ewss {

// ============================================================================
// Server (Reactor pattern)
// ============================================================================

class Server {
 public:
  using ConnPtr = std::shared_ptr<Connection>;

  explicit Server(uint16_t port, const std::string& bind_addr = "");
  ~Server();

  // Start the server (blocking)
  void run();

  // Stop the server
  void stop() { is_running_ = false; }

  // Configuration
  Server& set_max_connections(size_t max) {
    max_connections_ = max;
    return *this;
  }

  Server& set_poll_timeout_ms(int timeout) {
    poll_timeout_ms_ = timeout;
    return *this;
  }

  // Callbacks
  std::function<void(const ConnPtr&)> on_connect;
  std::function<void(const ConnPtr&, std::string_view)> on_message;
  std::function<void(const ConnPtr&, bool)> on_close;
  std::function<void(const ConnPtr&)> on_error;

  // Status
  size_t get_connection_count() const { return connections_.size(); }

  // Get error statistics (for monitoring)
  uint64_t get_total_socket_errors() const { return total_socket_errors_; }
  uint64_t get_total_handshake_errors() const { return total_handshake_errors_; }

 private:
  uint16_t port_;
  std::string bind_addr_;
  int server_sock_ = -1;  // Native socket instead of sockpp
  bool is_running_ = false;

  std::vector<ConnPtr> connections_;
  size_t max_connections_ = 1000;
  int poll_timeout_ms_ = 1000;

  uint64_t next_conn_id_ = 1;

  // Error statistics
  uint64_t total_socket_errors_ = 0;
  uint64_t total_handshake_errors_ = 0;

  // Internal methods
  expected<void, ErrorCode> accept_connection();
  void handle_connection_io(ConnPtr& conn, const pollfd& pfd);
  void remove_closed_connections();
  void log_info(const std::string& msg);
  void log_error(const std::string& msg);
};

}  // namespace ewss
