#include "ewss/server.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

namespace ewss {

Server::Server(uint16_t port, const std::string& bind_addr) : port_(port), bind_addr_(bind_addr) {
  // Create socket
  server_sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_sock_ < 0) {
    throw std::runtime_error("Failed to create socket");
  }

  // Set SO_REUSEADDR
  int reuse = 1;
  setsockopt(server_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  // Bind
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = bind_addr_.empty() ? htonl(INADDR_ANY) : inet_addr(bind_addr_.c_str());

  if (bind(server_sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    int err = errno;
    close(server_sock_);
    throw std::runtime_error("Failed to bind port " + std::to_string(port_) + ": " + strerror(err));
  }

  // Listen
  if (listen(server_sock_, 128) < 0) {
    close(server_sock_);
    throw std::runtime_error("Failed to listen");
  }

  // Set non-blocking
  fcntl(server_sock_, F_SETFL, O_NONBLOCK);

  log_info("Server initialized on " + bind_addr_ + ":" + std::to_string(port_));
}

Server::~Server() {
  if (server_sock_ >= 0) {
    close(server_sock_);
  }
}

void Server::run() {
  is_running_ = true;
  log_info("Server starting...");

  while (is_running_) {
    // Prepare poll FDs
    std::vector<pollfd> fds;

    // Add acceptor socket
    fds.push_back({server_sock_, POLLIN, 0});

    // Add client sockets
    for (auto& conn : connections_) {
      short events = POLLIN;
      if (conn->has_data_to_send()) {
        events |= POLLOUT;
      }
      fds.push_back({(int)conn->get_fd(), events, 0});
    }

    // Poll
    int ret = ::poll(fds.data(), fds.size(), poll_timeout_ms_);
    if (ret < 0) {
      log_error("Poll error: " + std::string(strerror(errno)));
      break;
    }

    if (ret == 0) {
      // Timeout
      continue;
    }

    // Handle new connections
    if (fds[0].revents & POLLIN) {
      auto accept_result = accept_connection();
      if (!accept_result.has_value() && accept_result.get_error() != ErrorCode::kSocketError) {
        // Log the error but continue
      }
    }

    // Handle client I/O
    for (size_t i = 1; i < fds.size(); ++i) {
      if (i - 1 >= connections_.size())
        break;
      handle_connection_io(connections_[i - 1], fds[i]);
    }

    // Remove closed connections
    remove_closed_connections();
  }

  log_info("Server stopped");
}

expected<void, ErrorCode> Server::accept_connection() {
  if (connections_.size() >= max_connections_) {
    log_error("Max connections reached");
    return expected<void, ErrorCode>::error(ErrorCode::kMaxConnectionsExceeded);
  }

  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  int client_sock = accept(server_sock_, (struct sockaddr*)&client_addr, &client_addr_len);

  if (client_sock < 0) {
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      log_error("Accept error: " + std::string(strerror(err)));
      total_socket_errors_++;
      return expected<void, ErrorCode>::error(ErrorCode::kSocketError);
    }
    return expected<void, ErrorCode>::success();
  }

  // Set non-blocking
  fcntl(client_sock, F_SETFL, O_NONBLOCK);

  auto conn = std::make_shared<Connection>(client_sock);

  // Bind callbacks
  conn->on_open = on_connect;
  conn->on_message = on_message;
  conn->on_close = on_close;
  conn->on_error = on_error;

  connections_.push_back(conn);

  log_info("New connection: " + std::to_string(connections_.size()) + " active connections");
  return expected<void, ErrorCode>::success();
}

void Server::handle_connection_io(ConnPtr& conn, const pollfd& pfd) {
  if (pfd.revents & POLLIN) {
    auto read_result = conn->handle_read();
    if (!read_result.has_value()) {
      conn->close();
    }
  }

  if (pfd.revents & POLLOUT) {
    auto write_result = conn->handle_write();
    if (!write_result.has_value()) {
      conn->close();
    }
  }

  if (pfd.revents & (POLLERR | POLLHUP)) {
    conn->close();
  }
}

void Server::remove_closed_connections() {
  connections_.erase(
      std::remove_if(connections_.begin(), connections_.end(), [](const ConnPtr& c) { return c->is_closed(); }),
      connections_.end());
}

void Server::log_info(const std::string& msg) {
  std::cout << "[EWSS INFO] " << msg << std::endl;
}

void Server::log_error(const std::string& msg) {
  std::cerr << "[EWSS ERROR] " << msg << std::endl;
}

}  // namespace ewss
