/**
 * EWSS Performance Benchmark Server
 * 用于与 Simple-WebSocket-Server 进行性能对比
 *
 * 编译:
 *   cd /tmp/ewss/build
 *   cmake .. -DCMAKE_BUILD_TYPE=Release -DEWSS_BUILD_EXAMPLES=ON
 *   make ewss_perf_server
 *
 * 运行:
 *   ./ewss_perf_server 8080
 *
 * 测试:
 *   wscat -c ws://localhost:8080
 *   > hello
 *   < echo: hello
 */

#include "ewss/server.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstring>

using PerfClock = std::chrono::steady_clock;

// ============================================================================
// Performance Statistics
// ============================================================================

struct PerfStats {
  std::atomic<uint64_t> total_messages{0};
  std::atomic<uint64_t> total_bytes{0};
  std::atomic<uint64_t> active_connections{0};

  std::chrono::steady_clock::time_point start_time;
  std::vector<uint64_t> latencies_us;  // microseconds

  void reset() {
    total_messages = 0;
    total_bytes = 0;
    active_connections = 0;
    start_time = PerfClock::now();
    latencies_us.clear();
  }

  void print_report() {
    auto now = PerfClock::now();
    auto elapsed_s =
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
    if (elapsed_s == 0) elapsed_s = 1;

    uint64_t msgs = total_messages.load();
    uint64_t bytes = total_bytes.load();

    std::cout << "\n╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║               EWSS Performance Report                      ║\n";
    std::cout << "╠════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Throughput:                                               ║\n";
    std::cout << "║   Messages/sec: " << std::setw(43) << (msgs / elapsed_s) << " ║\n";
    std::cout << "║   Bytes/sec:    " << std::setw(43) << (bytes / elapsed_s) << " ║\n";
    std::cout << "║   Avg msg size: " << std::setw(43)
              << (msgs > 0 ? bytes / msgs : 0) << " B ║\n";
    std::cout << "║ Connections:                                              ║\n";
    std::cout << "║   Current:      " << std::setw(43)
              << active_connections.load() << " ║\n";
    std::cout << "║ Duration:       " << std::setw(43) << elapsed_s << " sec ║\n";

    if (!latencies_us.empty()) {
      // Calculate percentiles
      std::sort(latencies_us.begin(), latencies_us.end());
      auto p50 = latencies_us[latencies_us.size() / 2];
      auto p95 = latencies_us[latencies_us.size() * 95 / 100];
      auto p99 = latencies_us[latencies_us.size() * 99 / 100];
      auto max_lat = latencies_us.back();
      auto min_lat = latencies_us.front();

      std::cout << "║ Latency (µs):                                             ║\n";
      std::cout << "║   Min:          " << std::setw(43) << min_lat << " ║\n";
      std::cout << "║   P50:          " << std::setw(43) << p50 << " ║\n";
      std::cout << "║   P95:          " << std::setw(43) << p95 << " ║\n";
      std::cout << "║   P99:          " << std::setw(43) << p99 << " ║\n";
      std::cout << "║   Max:          " << std::setw(43) << max_lat << " ║\n";
    }

    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";
  }
};

static PerfStats g_stats;

// ============================================================================
// Timestamp Extraction (for latency measurement)
// ============================================================================

uint64_t extract_timestamp_us(std::string_view msg) {
  // Message format: "bench_<timestamp_ns>"
  const char* prefix = "bench_";
  size_t pos = msg.find(prefix);
  if (pos == std::string::npos) return 0;

  try {
    uint64_t ns = std::stoull(std::string(msg.substr(pos + 6)));
    return ns / 1000;  // Convert to microseconds
  } catch (...) {
    return 0;
  }
}

// ============================================================================
// Main Server
// ============================================================================

int main(int argc, char* argv[]) {
  uint16_t port = 8080;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::atoi(argv[1]));
  }

  std::cout << "╔════════════════════════════════════════════════════════════╗\n";
  std::cout << "║         EWSS Performance Benchmark Server v0.3.0           ║\n";
  std::cout << "╠════════════════════════════════════════════════════════════╣\n";
  std::cout << "║ Listening on port: " << std::setw(44) << port << " ║\n";
  std::cout << "║ Press Ctrl+C to stop and print report                      ║\n";
  std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";

  try {
    ewss::Server server(port);

    server.on_connect = [](const std::shared_ptr<ewss::Connection>& conn) {
      g_stats.active_connections++;
    };

    server.on_message = [](const std::shared_ptr<ewss::Connection>& conn,
                           std::string_view msg) {
      auto now = PerfClock::now();
      auto now_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now.time_since_epoch())
              .count();

      g_stats.total_messages++;
      g_stats.total_bytes += msg.size();

      // Measure latency if message contains timestamp
      uint64_t send_time_us = extract_timestamp_us(msg);
      if (send_time_us > 0) {
        uint64_t latency_us = now_us - send_time_us;
        if (latency_us < 1000000) {  // Sanity check: < 1 second
          g_stats.latencies_us.push_back(latency_us);
        }
      }

      // Echo back immediately
      std::string response = "echo: ";
      response.append(msg);
      conn->send(response);
    };

    server.on_close = [](const std::shared_ptr<ewss::Connection>& conn, bool clean) {
      g_stats.active_connections--;
    };

    server.on_error = [](const std::shared_ptr<ewss::Connection>& conn) {
      std::cerr << "Client #" << conn->get_id() << " error" << std::endl;
    };

    // Start timing
    g_stats.reset();

    // Run server (blocking)
    server.run();

  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  }

  // Print final report
  g_stats.print_report();

  return 0;
}
