#ifndef EWSS_SERVER_HPP_
#define EWSS_SERVER_HPP_

#include "connection.hpp"
#include "connection_pool.hpp"
#include "vocabulary.hpp"

#include <cstdint>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <poll.h>

namespace ewss {

// ============================================================================
// TCP Tuning Configuration
// ============================================================================

struct TcpTuning {
  bool tcp_nodelay = false;     // Disable Nagle algorithm
  bool tcp_quickack = false;    // Reduce ACK delay (Linux-specific)
  bool so_keepalive = false;    // Enable TCP keepalive

  // Keepalive parameters (Linux-specific, effective when so_keepalive=true)
  int keepalive_idle_s = 60;    // Seconds before first keepalive probe
  int keepalive_interval_s = 10;  // Seconds between probes
  int keepalive_count = 5;      // Max probes before dropping connection
};

// ============================================================================
// Server (Reactor pattern with writev + monitoring)
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

  Server& set_tcp_tuning(const TcpTuning& tuning) {
    tcp_tuning_ = tuning;
    return *this;
  }

  Server& set_use_writev(bool enable) {
    use_writev_ = enable;
    return *this;
  }

  // Callbacks
  std::function<void(const ConnPtr&)> on_connect;
  std::function<void(const ConnPtr&, std::string_view)> on_message;
  std::function<void(const ConnPtr&, bool)> on_close;
  std::function<void(const ConnPtr&)> on_error;
  std::function<void(const ConnPtr&)> on_backpressure;
  std::function<void(const ConnPtr&)> on_drain;

  // Status
  size_t get_connection_count() const { return connections_.size(); }

  // Performance monitoring
  const ServerStats& stats() const { return stats_; }
  void reset_stats() { stats_.reset(); }

  // Get error statistics (for monitoring)
  uint64_t get_total_socket_errors() const { return stats_.socket_errors.load(); }
  uint64_t get_total_handshake_errors() const { return stats_.handshake_errors.load(); }

  // Maximum supported connections (compile-time fixed)
  static constexpr size_t kMaxConnections = 64;

 private:
  uint16_t port_;
  std::string bind_addr_;
  int server_sock_ = -1;
  bool is_running_ = false;
  bool use_writev_ = true;  // Default: use writev for zero-copy

  FixedVector<ConnPtr, kMaxConnections> connections_;
  size_t max_connections_ = 50;
  int poll_timeout_ms_ = 1000;
  TcpTuning tcp_tuning_;

  uint64_t next_conn_id_ = 1;

  // Pre-allocated pollfd array (1 for server + kMaxConnections for clients)
  std::array<pollfd, kMaxConnections + 1> poll_fds_{};

  // Performance monitoring
  ServerStats stats_;

  // Internal methods
  expected<void, ErrorCode> accept_connection();
  void handle_connection_io(ConnPtr& conn, const pollfd& pfd);
  void remove_closed_connections();
  void apply_tcp_tuning(int fd);
  void log_info(const std::string& msg);
  void log_error(const std::string& msg);
};

}  // namespace ewss

#endif  // EWSS_SERVER_HPP_
