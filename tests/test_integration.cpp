#include "ewss.hpp"

#include <cstring>

#include <arpa/inet.h>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ============================================================================
// Minimal WebSocket test client (raw POSIX socket)
// ============================================================================

class WsTestClient {
 public:
  WsTestClient() = default;
  ~WsTestClient() { disconnect(); }

  bool connect(uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0)
      return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    return true;
  }

  bool handshake(int timeout_ms = 2000) {
    // Set receive timeout for handshake
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Fixed client key for deterministic testing
    const char* client_key = "dGhlIHNhbXBsZSBub25jZQ==";

    std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: ";
    request += client_key;
    request += "\r\n\r\n";

    if (!send_raw(request.data(), request.size()))
      return false;

    // Read HTTP 101 response
    char buf[1024];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
      ssize_t n = ::recv(fd_, buf + total, sizeof(buf) - 1 - total, 0);
      if (n <= 0)
        return false;
      total += static_cast<size_t>(n);
      buf[total] = '\0';
      if (strstr(buf, "\r\n\r\n") != nullptr)
        break;
    }

    // Verify 101 status
    return strstr(buf, "101") != nullptr;
  }

  bool send_text(std::string_view payload) { return send_frame(0x01, payload.data(), payload.size()); }

  bool send_binary(const void* data, size_t len) { return send_frame(0x02, data, len); }

  bool send_ping(std::string_view payload = "") { return send_frame(0x09, payload.data(), payload.size()); }

  bool send_close(uint16_t code = 1000) {
    uint8_t close_payload[2];
    close_payload[0] = static_cast<uint8_t>((code >> 8) & 0xFF);
    close_payload[1] = static_cast<uint8_t>(code & 0xFF);
    return send_frame(0x08, close_payload, 2);
  }

  // Receive a WebSocket frame. Returns payload as string.
  // Sets out_opcode to the frame opcode.
  std::string recv_frame(uint8_t* out_opcode = nullptr, int timeout_ms = 2000) {
    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t header[2];
    if (!recv_exact(header, 2))
      return {};

    uint8_t opcode = header[0] & 0x0F;
    if (out_opcode)
      *out_opcode = opcode;

    uint64_t payload_len = header[1] & 0x7F;
    if (payload_len == 126) {
      uint8_t ext[2];
      if (!recv_exact(ext, 2))
        return {};
      payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
      uint8_t ext[8];
      if (!recv_exact(ext, 8))
        return {};
      payload_len = 0;
      for (int i = 0; i < 8; ++i) {
        payload_len = (payload_len << 8) | ext[i];
      }
    }

    std::string payload(payload_len, '\0');
    if (payload_len > 0) {
      if (!recv_exact(reinterpret_cast<uint8_t*>(payload.data()), payload_len)) {
        return {};
      }
    }
    return payload;
  }

  void disconnect() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd() const { return fd_; }

 private:
  int fd_ = -1;

  bool send_raw(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
      ssize_t n = ::send(fd_, p + sent, len - sent, MSG_NOSIGNAL);
      if (n <= 0)
        return false;
      sent += static_cast<size_t>(n);
    }
    return true;
  }

  bool recv_exact(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
      ssize_t n = ::recv(fd_, buf + got, len - got, 0);
      if (n <= 0)
        return false;
      got += static_cast<size_t>(n);
    }
    return true;
  }

  // Send masked WebSocket frame (client-to-server must be masked per RFC 6455)
  bool send_frame(uint8_t opcode, const void* payload, size_t len) {
    uint8_t frame[14 + 4];  // max header + mask key
    size_t pos = 0;

    // FIN + opcode
    frame[pos++] = 0x80 | opcode;

    // Mask bit + payload length
    if (len < 126) {
      frame[pos++] = static_cast<uint8_t>(0x80 | len);
    } else if (len < 65536) {
      frame[pos++] = 0x80 | 126;
      frame[pos++] = static_cast<uint8_t>((len >> 8) & 0xFF);
      frame[pos++] = static_cast<uint8_t>(len & 0xFF);
    } else {
      frame[pos++] = 0x80 | 127;
      for (int i = 7; i >= 0; --i) {
        frame[pos++] = static_cast<uint8_t>((len >> (i * 8)) & 0xFF);
      }
    }

    // Mask key (fixed for deterministic tests)
    uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(frame + pos, mask_key, 4);
    pos += 4;

    if (!send_raw(frame, pos))
      return false;

    // Send masked payload
    if (len > 0) {
      std::vector<uint8_t> masked(len);
      const uint8_t* src = static_cast<const uint8_t*>(payload);
      for (size_t i = 0; i < len; ++i) {
        masked[i] = src[i] ^ mask_key[i % 4];
      }
      if (!send_raw(masked.data(), masked.size()))
        return false;
    }
    return true;
  }
};

// ============================================================================
// Test helper: run server in background thread
// ============================================================================

static constexpr uint16_t kTestPort = 18080;

struct ServerFixture {
  ewss::Server server;
  std::thread server_thread;

  explicit ServerFixture(uint16_t port = kTestPort) : server(port) {
    server.set_max_connections(32);
    server.set_poll_timeout_ms(50);
  }

  void start() {
    server_thread = std::thread([this]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void stop() {
    server.stop();
    if (server_thread.joinable()) {
      server_thread.join();
    }
  }

  ~ServerFixture() { stop(); }
};

// ============================================================================
// Integration tests
// ============================================================================

TEST_CASE("Integration - Server start and stop", "[integration]") {
  ServerFixture fixture;
  fixture.start();

  // Server should be running, try to connect
  WsTestClient client;
  REQUIRE(client.connect(kTestPort));
  client.disconnect();

  fixture.stop();

  // After stop, new handshake should fail (server not processing)
  WsTestClient client2;
  if (client2.connect(kTestPort)) {
    // TCP connect may succeed due to SO_REUSEADDR / kernel backlog,
    // but handshake should fail since server loop has exited
    REQUIRE_FALSE(client2.handshake(500));
  }
}

TEST_CASE("Integration - Server restart", "[integration]") {
  {
    ServerFixture fixture;
    fixture.start();
    WsTestClient client;
    REQUIRE(client.connect(kTestPort));
    client.disconnect();
    fixture.stop();
  }

  // Restart on same port
  {
    ServerFixture fixture;
    fixture.start();
    WsTestClient client;
    REQUIRE(client.connect(kTestPort));
    client.disconnect();
    fixture.stop();
  }
}

TEST_CASE("Integration - WebSocket handshake", "[integration]") {
  ServerFixture fixture;
  fixture.start();

  WsTestClient client;
  REQUIRE(client.connect(kTestPort));
  REQUIRE(client.handshake());
  client.disconnect();
}

TEST_CASE("Integration - Single echo", "[integration]") {
  ServerFixture fixture;
  fixture.server.on_message = [](const auto& conn, std::string_view msg) { conn->send(msg); };
  fixture.start();

  WsTestClient client;
  REQUIRE(client.connect(kTestPort));
  REQUIRE(client.handshake());

  REQUIRE(client.send_text("Hello"));

  uint8_t opcode = 0;
  std::string reply = client.recv_frame(&opcode);
  REQUIRE(opcode == 0x01);  // Text
  REQUIRE(reply == "Hello");

  client.send_close(1000);
  client.disconnect();
}

TEST_CASE("Integration - Multiple sequential connections", "[integration]") {
  std::atomic<int> open_count{0};
  std::atomic<int> msg_count{0};
  std::atomic<int> close_count{0};

  ServerFixture fixture;
  fixture.server.on_connect = [&](const auto&) { ++open_count; };
  fixture.server.on_message = [&](const auto& conn, std::string_view msg) {
    ++msg_count;
    conn->send(msg);
  };
  fixture.server.on_close = [&](const auto&, bool) { ++close_count; };
  fixture.start();

  constexpr int kIterations = 20;
  for (int i = 0; i < kIterations; ++i) {
    WsTestClient client;
    REQUIRE(client.connect(kTestPort));
    REQUIRE(client.handshake());

    std::string msg = "msg_" + std::to_string(i);
    REQUIRE(client.send_text(msg));

    uint8_t opcode = 0;
    std::string reply = client.recv_frame(&opcode);
    REQUIRE(opcode == 0x01);
    REQUIRE(reply == msg);

    client.send_close(1000);
    // Small delay for server to process close
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    client.disconnect();
  }

  // Wait for server to process all closes
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  REQUIRE(msg_count.load() == kIterations);
}

TEST_CASE("Integration - Batch messages single connection", "[integration]") {
  ServerFixture fixture;
  fixture.server.on_message = [](const auto& conn, std::string_view msg) { conn->send(msg); };
  fixture.start();

  WsTestClient client;
  REQUIRE(client.connect(kTestPort));
  REQUIRE(client.handshake());

  constexpr int kBatchSize = 50;

  // Send all messages
  for (int i = 0; i < kBatchSize; ++i) {
    std::string msg = "batch_" + std::to_string(i);
    REQUIRE(client.send_text(msg));
  }

  // Receive all echoes
  for (int i = 0; i < kBatchSize; ++i) {
    uint8_t opcode = 0;
    std::string reply = client.recv_frame(&opcode);
    REQUIRE(opcode == 0x01);
    std::string expected = "batch_" + std::to_string(i);
    REQUIRE(reply == expected);
  }

  client.send_close(1000);
  client.disconnect();
}

TEST_CASE("Integration - Binary message echo", "[integration]") {
  ServerFixture fixture;
  fixture.server.on_message = [](const auto& conn, std::string_view msg) { conn->send_binary(msg); };
  fixture.start();

  WsTestClient client;
  REQUIRE(client.connect(kTestPort));
  REQUIRE(client.handshake());

  // Send binary data with non-printable bytes
  uint8_t binary_data[] = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0x80, 0x7F};
  REQUIRE(client.send_binary(binary_data, sizeof(binary_data)));

  uint8_t opcode = 0;
  std::string reply = client.recv_frame(&opcode);
  REQUIRE(opcode == 0x02);  // Binary
  REQUIRE(reply.size() == sizeof(binary_data));
  REQUIRE(memcmp(reply.data(), binary_data, sizeof(binary_data)) == 0);

  client.send_close(1000);
  client.disconnect();
}

TEST_CASE("Integration - Ping Pong", "[integration]") {
  ServerFixture fixture;
  fixture.server.on_message = [](const auto& conn, std::string_view msg) { conn->send(msg); };
  fixture.start();

  WsTestClient client;
  REQUIRE(client.connect(kTestPort));
  REQUIRE(client.handshake());

  // Send ping with payload
  REQUIRE(client.send_ping("ping_data"));

  uint8_t opcode = 0;
  std::string reply = client.recv_frame(&opcode);
  REQUIRE(opcode == 0x0A);  // Pong
  REQUIRE(reply == "ping_data");

  client.send_close(1000);
  client.disconnect();
}

TEST_CASE("Integration - Client-initiated close", "[integration]") {
  std::atomic<bool> close_called{false};

  ServerFixture fixture;
  fixture.server.on_message = [](const auto& conn, std::string_view msg) { conn->send(msg); };
  fixture.server.on_close = [&](const auto&, bool) { close_called = true; };
  fixture.start();

  WsTestClient client;
  REQUIRE(client.connect(kTestPort));
  REQUIRE(client.handshake());

  // Send close frame
  REQUIRE(client.send_close(1000));

  // Wait for server to process
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  client.disconnect();

  REQUIRE(close_called.load());
}

TEST_CASE("Integration - Server stats tracking", "[integration]") {
  ServerFixture fixture;
  fixture.server.on_message = [](const auto& conn, std::string_view msg) { conn->send(msg); };
  fixture.start();

  REQUIRE(fixture.server.stats().total_connections.load() == 0);

  // Make 3 connections
  for (int i = 0; i < 3; ++i) {
    WsTestClient client;
    REQUIRE(client.connect(kTestPort));
    REQUIRE(client.handshake());
    REQUIRE(client.send_text("stats_test"));
    client.recv_frame();
    client.send_close(1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.disconnect();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  REQUIRE(fixture.server.stats().total_connections.load() == 3);
}

TEST_CASE("Integration - Large message", "[integration]") {
  ServerFixture fixture;
  fixture.server.on_message = [](const auto& conn, std::string_view msg) { conn->send(msg); };
  fixture.start();

  WsTestClient client;
  REQUIRE(client.connect(kTestPort));
  REQUIRE(client.handshake());

  // Send 1000-byte message
  std::string large_msg(1000, 'X');
  for (size_t i = 0; i < large_msg.size(); ++i) {
    large_msg[i] = static_cast<char>('A' + (i % 26));
  }

  REQUIRE(client.send_text(large_msg));

  uint8_t opcode = 0;
  std::string reply = client.recv_frame(&opcode);
  REQUIRE(opcode == 0x01);
  REQUIRE(reply == large_msg);

  client.send_close(1000);
  client.disconnect();
}

TEST_CASE("Integration - Connection callbacks", "[integration]") {
  std::atomic<int> open_count{0};
  std::atomic<int> msg_count{0};
  std::atomic<int> close_count{0};
  std::atomic<int> error_count{0};

  ServerFixture fixture;
  fixture.server.on_connect = [&](const auto&) { ++open_count; };
  fixture.server.on_message = [&](const auto& conn, std::string_view msg) {
    ++msg_count;
    conn->send(msg);
  };
  fixture.server.on_close = [&](const auto&, bool) { ++close_count; };
  fixture.server.on_error = [&](const auto&) { ++error_count; };
  fixture.start();

  {
    WsTestClient client;
    REQUIRE(client.connect(kTestPort));
    REQUIRE(client.handshake());

    // Wait for on_open to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(open_count.load() == 1);

    REQUIRE(client.send_text("callback_test"));
    client.recv_frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(msg_count.load() == 1);

    client.send_close(1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client.disconnect();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REQUIRE(close_count.load() >= 1);
  REQUIRE(error_count.load() == 0);
}

TEST_CASE("Integration - Empty message", "[integration]") {
  ServerFixture fixture;
  fixture.server.on_message = [](const auto& conn, std::string_view msg) { conn->send(msg); };
  fixture.start();

  WsTestClient client;
  REQUIRE(client.connect(kTestPort));
  REQUIRE(client.handshake());

  // Send empty text frame
  REQUIRE(client.send_text(""));

  uint8_t opcode = 0;
  std::string reply = client.recv_frame(&opcode);
  REQUIRE(opcode == 0x01);
  REQUIRE(reply.empty());

  client.send_close(1000);
  client.disconnect();
}
