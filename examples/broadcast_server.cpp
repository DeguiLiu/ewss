#include "ewss.hpp"
#include <iostream>
#include <memory>
#include <vector>
#include <mutex>

// Simple broadcast server that echoes messages to all clients
int main(int argc, char* argv[]) {
  uint16_t port = 8080;

  if (argc > 1) {
    port = std::stoi(argv[1]);
  }

  try {
    ewss::Server server(port);

    // Store weak pointers to broadcast
    std::vector<std::weak_ptr<ewss::Connection>> broadcast_list;
    std::mutex broadcast_mutex;

    server.on_connect = [&](const std::shared_ptr<ewss::Connection>& conn) {
      std::lock_guard<std::mutex> lock(broadcast_mutex);
      broadcast_list.push_back(conn);
      std::cout << "Client #" << conn->get_id() << " connected. ("
                << broadcast_list.size() << " total)" << std::endl;
    };

    server.on_message = [&](const std::shared_ptr<ewss::Connection>& conn,
                            std::string_view msg) {
      std::lock_guard<std::mutex> lock(broadcast_mutex);

      std::string broadcast_msg =
          "Client #" + std::to_string(conn->get_id()) + ": " + std::string(msg);

      // Broadcast to all clients
      for (auto& weak_conn : broadcast_list) {
        if (auto target = weak_conn.lock()) {
          target->send(broadcast_msg);
        }
      }
    };

    server.on_close = [&](const std::shared_ptr<ewss::Connection>& conn,
                          bool clean) {
      std::lock_guard<std::mutex> lock(broadcast_mutex);

      // Remove closed connection
      broadcast_list.erase(
          std::remove_if(broadcast_list.begin(), broadcast_list.end(),
                         [&](const std::weak_ptr<ewss::Connection>& weak) {
                           auto ptr = weak.lock();
                           return !ptr || ptr->get_id() == conn->get_id();
                         }),
          broadcast_list.end());

      std::cout << "Client #" << conn->get_id() << " closed ("
                << (clean ? "clean" : "unclean") << "). ("
                << broadcast_list.size() << " remaining)" << std::endl;
    };

    server.on_error = [](const std::shared_ptr<ewss::Connection>& conn) {
      std::cerr << "Client #" << conn->get_id() << " error" << std::endl;
    };

    server.run();

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
