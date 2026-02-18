#include "ewss/server.hpp"
#include <cerrno>
#include <iostream>
#include <vector>

namespace ewss {

Server::Server(uint16_t port, const std::string& bind_addr)
    : port_(port), bind_addr_(bind_addr) {
  // Bind to address
  if (bind_addr_.empty()) {
    acceptor_.bind(port_);
  } else {
    acceptor_.bind(port_, bind_addr_);
  }

  if (!acceptor_) {
    throw std::runtime_error("Failed to bind port " + std::to_string(port_));
  }

  acceptor_.set_non_blocking(true);
  acceptor_.listen();

  log_info("Server initialized on " + bind_addr_ + ":" + std::to_string(port_));
}

Server::~Server() {
  if (acceptor_.is_open()) {
    acceptor_.close();
  }
}

void Server::run() {
  is_running_ = true;
  log_info("Server starting...");

  while (is_running_) {
    // Prepare poll FDs
    std::vector<pollfd> fds;

    // Add acceptor socket
    fds.push_back({(int)acceptor_.handle(), POLLIN, 0});

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
      accept_connection();
    }

    // Handle client I/O
    for (size_t i = 1; i < fds.size(); ++i) {
      if (i - 1 >= connections_.size()) break;
      handle_connection_io(connections_[i - 1], fds[i]);
    }

    // Remove closed connections
    remove_closed_connections();
  }

  log_info("Server stopped");
}

void Server::accept_connection() {
  if (connections_.size() >= max_connections_) {
    log_error("Max connections reached");
    return;
  }

  sockpp::tcp_socket sock = acceptor_.accept();
  if (!sock) {
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      log_error("Accept error: " + std::string(strerror(err)));
    }
    return;
  }

  auto conn = std::make_shared<Connection>(std::move(sock));

  // Bind callbacks
  conn->on_open = on_connect;
  conn->on_message = on_message;
  conn->on_close = on_close;
  conn->on_error = on_error;

  connections_.push_back(conn);

  log_info("New connection: " + std::to_string(connections_.size()) +
           " active connections");
}

void Server::handle_connection_io(ConnPtr& conn, const pollfd& pfd) {
  if (pfd.revents & POLLIN) {
    if (!conn->handle_read()) {
      conn->close();
    }
  }

  if (pfd.revents & POLLOUT) {
    if (!conn->handle_write()) {
      conn->close();
    }
  }

  if (pfd.revents & (POLLERR | POLLHUP)) {
    conn->close();
  }
}

void Server::remove_closed_connections() {
  connections_.erase(
      std::remove_if(connections_.begin(), connections_.end(),
                     [](const ConnPtr& c) { return c->is_closed(); }),
      connections_.end());
}

void Server::log_info(const std::string& msg) {
  std::cout << "[EWSS INFO] " << msg << std::endl;
}

void Server::log_error(const std::string& msg) {
  std::cerr << "[EWSS ERROR] " << msg << std::endl;
}

}  // namespace ewss
