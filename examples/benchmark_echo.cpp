// EWSS Performance Benchmark
// Measures: throughput (msg/s), latency (P50/P99), memory per connection
//
// Usage: ./benchmark_echo [num_clients] [messages_per_client] [payload_size]

#include "ewss.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Minimal benchmark WebSocket client
// ============================================================================

class BenchClient {
 public:
  bool connect(uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    // TCP_NODELAY for low latency
    int opt = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    return true;
  }

  bool handshake() {
    const char* req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    size_t len = strlen(req);
    if (::send(fd_, req, len, MSG_NOSIGNAL) != static_cast<ssize_t>(len))
      return false;

    char buf[512];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
      ssize_t n = ::recv(fd_, buf + total, sizeof(buf) - 1 - total, 0);
      if (n <= 0) return false;
      total += static_cast<size_t>(n);
      buf[total] = '\0';
      if (strstr(buf, "\r\n\r\n")) break;
    }
    return strstr(buf, "101") != nullptr;
  }

  bool send_masked(const void* payload, size_t payload_len) {
    uint8_t header[14 + 4];
    size_t pos = 0;
    header[pos++] = 0x81;  // FIN + Text

    if (payload_len < 126) {
      header[pos++] = static_cast<uint8_t>(0x80 | payload_len);
    } else if (payload_len < 65536) {
      header[pos++] = 0x80 | 126;
      header[pos++] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
      header[pos++] = static_cast<uint8_t>(payload_len & 0xFF);
    } else {
      header[pos++] = 0x80 | 127;
      for (int i = 7; i >= 0; --i)
        header[pos++] = static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF);
    }

    // Fixed mask key
    header[pos++] = 0x12;
    header[pos++] = 0x34;
    header[pos++] = 0x56;
    header[pos++] = 0x78;

    if (::send(fd_, header, pos, MSG_NOSIGNAL) != static_cast<ssize_t>(pos))
      return false;

    // Send masked payload
    const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    std::vector<uint8_t> masked(payload_len);
    const uint8_t* src = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < payload_len; ++i)
      masked[i] = src[i] ^ mask[i % 4];

    if (::send(fd_, masked.data(), masked.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(masked.size()))
      return false;
    return true;
  }

  bool recv_frame(size_t expected_payload_len) {
    // Read header (2 bytes min)
    uint8_t hdr[2];
    if (!recv_exact(hdr, 2)) return false;

    uint64_t plen = hdr[1] & 0x7F;
    if (plen == 126) {
      uint8_t ext[2];
      if (!recv_exact(ext, 2)) return false;
      plen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (plen == 127) {
      uint8_t ext[8];
      if (!recv_exact(ext, 8)) return false;
      plen = 0;
      for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
    }

    // Drain payload
    size_t remaining = static_cast<size_t>(plen);
    uint8_t drain[4096];
    while (remaining > 0) {
      size_t chunk = std::min(remaining, sizeof(drain));
      if (!recv_exact(drain, chunk)) return false;
      remaining -= chunk;
    }
    return plen == expected_payload_len;
  }

  void close_fd() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;

  bool recv_exact(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
      ssize_t n = ::recv(fd_, buf + got, len - got, 0);
      if (n <= 0) return false;
      got += static_cast<size_t>(n);
    }
    return true;
  }
};

// ============================================================================
// Benchmark runner
// ============================================================================

struct BenchResult {
  double throughput_msg_per_sec;
  double p50_us;
  double p99_us;
  double avg_us;
  double min_us;
  double max_us;
  uint64_t total_messages;
  double elapsed_sec;
};

BenchResult run_echo_benchmark(uint16_t port, int num_clients,
                                int msgs_per_client, int payload_size) {
  std::string payload(payload_size, 'A');
  std::vector<std::vector<double>> all_latencies(num_clients);
  std::atomic<int> ready_count{0};
  std::atomic<bool> go{false};

  // Create client threads
  std::vector<std::thread> threads;
  for (int c = 0; c < num_clients; ++c) {
    threads.emplace_back([&, c]() {
      BenchClient client;
      if (!client.connect(port) || !client.handshake()) {
        std::cerr << "Client " << c << " connect failed\n";
        return;
      }

      all_latencies[c].reserve(msgs_per_client);
      ++ready_count;

      // Wait for all clients to be ready
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();

      for (int i = 0; i < msgs_per_client; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        if (!client.send_masked(payload.data(), payload.size())) break;
        if (!client.recv_frame(payload.size())) break;
        auto t1 = std::chrono::steady_clock::now();

        double us = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        t1 - t0).count() / 1000.0;
        all_latencies[c].push_back(us);
      }

      client.close_fd();
    });
  }

  // Wait for all clients to connect
  while (ready_count.load() < num_clients)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

  auto bench_start = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);

  for (auto& t : threads) t.join();
  auto bench_end = std::chrono::steady_clock::now();

  // Aggregate latencies
  std::vector<double> merged;
  for (auto& v : all_latencies) {
    merged.insert(merged.end(), v.begin(), v.end());
  }

  BenchResult result{};
  if (merged.empty()) return result;

  std::sort(merged.begin(), merged.end());
  result.total_messages = merged.size();
  result.elapsed_sec = std::chrono::duration<double>(bench_end - bench_start).count();
  result.throughput_msg_per_sec = result.total_messages / result.elapsed_sec;
  result.avg_us = std::accumulate(merged.begin(), merged.end(), 0.0) / merged.size();
  result.min_us = merged.front();
  result.max_us = merged.back();
  result.p50_us = merged[merged.size() * 50 / 100];
  result.p99_us = merged[merged.size() * 99 / 100];

  return result;
}

void print_result(const char* label, const BenchResult& r) {
  std::cout << "\n=== " << label << " ===\n";
  std::cout << "  Total messages: " << r.total_messages << "\n";
  std::cout << "  Elapsed:        " << r.elapsed_sec << " s\n";
  std::cout << "  Throughput:     " << static_cast<int>(r.throughput_msg_per_sec) << " msg/s\n";
  std::cout << "  Latency P50:    " << r.p50_us << " us\n";
  std::cout << "  Latency P99:    " << r.p99_us << " us\n";
  std::cout << "  Latency avg:    " << r.avg_us << " us\n";
  std::cout << "  Latency min:    " << r.min_us << " us\n";
  std::cout << "  Latency max:    " << r.max_us << " us\n";
}

int main(int argc, char* argv[]) {
  int num_clients = (argc > 1) ? atoi(argv[1]) : 1;
  int msgs_per_client = (argc > 2) ? atoi(argv[2]) : 10000;
  int payload_size = (argc > 3) ? atoi(argv[3]) : 64;

  constexpr uint16_t kPort = 19090;

  std::cout << "EWSS Echo Benchmark\n";
  std::cout << "  Clients:          " << num_clients << "\n";
  std::cout << "  Messages/client:  " << msgs_per_client << "\n";
  std::cout << "  Payload size:     " << payload_size << " bytes\n";

  // Start server
  ewss::Server server(kPort);
  ewss::TcpTuning tuning;
  tuning.tcp_nodelay = true;
  server.set_tcp_tuning(tuning);
  server.set_max_connections(64);
  server.set_poll_timeout_ms(1);

  server.on_message = [](const auto& conn, std::string_view msg) {
    conn->send(msg);
  };

  std::thread server_thread([&]() { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Run benchmark
  auto result = run_echo_benchmark(kPort, num_clients, msgs_per_client,
                                    payload_size);
  print_result("Echo Benchmark", result);

  // Print server stats
  const auto& stats = server.stats();
  std::cout << "\n=== Server Stats ===\n";
  std::cout << "  Total connections:    " << stats.total_connections.load() << "\n";
  std::cout << "  Max poll latency:     " << stats.max_poll_latency_us.load() << " us\n";
  std::cout << "  Socket errors:        " << stats.socket_errors.load() << "\n";
  std::cout << "  Rejected connections: " << stats.rejected_connections.load() << "\n";

  server.stop();
  server_thread.join();

  return 0;
}
