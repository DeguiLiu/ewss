#include "ewss.hpp"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  uint16_t port = 8080;

  if (argc > 1) {
    port = std::stoi(argv[1]);
  }

  try {
    ewss::Server server(port);

    server.on_connect = [](const std::shared_ptr<ewss::Connection>& conn) {
      std::cout << "Client #" << conn->get_id() << " connected" << std::endl;
    };

    server.on_message = [](const std::shared_ptr<ewss::Connection>& conn,
                           std::string_view msg) {
      std::cout << "Client #" << conn->get_id() << " sent: " << msg
                << std::endl;
      // Echo back
      conn->send(std::string("Echo: ") + std::string(msg));
    };

    server.on_close = [](const std::shared_ptr<ewss::Connection>& conn,
                         bool clean) {
      std::cout << "Client #" << conn->get_id() << " closed ("
                << (clean ? "clean" : "unclean") << ")" << std::endl;
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
