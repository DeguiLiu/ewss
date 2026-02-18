#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <poll.h>
#include <vector>

#include <sockpp/tcp_acceptor.h>

#include "connection.hpp"

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
  std::function<void(const ConnPtr&, const std::string&)> on_error;

  // Status
  size_t get_connection_count() const { return connections_.size(); }

 private:
  uint16_t port_;
  std::string bind_addr_;
  sockpp::tcp_acceptor acceptor_;
  bool is_running_ = false;

  std::vector<ConnPtr> connections_;
  size_t max_connections_ = 1000;
  int poll_timeout_ms_ = 1000;

  uint64_t next_conn_id_ = 1;

  // Internal methods
  void accept_connection();
  void handle_connection_io(ConnPtr& conn, const pollfd& pfd);
  void remove_closed_connections();
  void log_info(const std::string& msg);
  void log_error(const std::string& msg);
};

}  // namespace ewss
