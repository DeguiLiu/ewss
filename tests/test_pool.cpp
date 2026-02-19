#include "ewss.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ewss;

// ============================================================================
// ObjectPool
// ============================================================================

TEST_CASE("ObjectPool - initial state", "[pool]") {
  ObjectPool<int, 4> pool;
  REQUIRE(pool.available() == 4);
  REQUIRE(pool.capacity() == 4);
  REQUIRE(pool.in_use() == 0);
}

TEST_CASE("ObjectPool - acquire and release", "[pool]") {
  ObjectPool<int, 4> pool;
  int32_t idx = pool.acquire();
  REQUIRE(idx >= 0);
  REQUIRE(pool.in_use() == 1);
  REQUIRE(pool.available() == 3);
  REQUIRE(pool.is_active(idx));

  pool.release(idx);
  REQUIRE(pool.in_use() == 0);
  REQUIRE(pool.available() == 4);
  REQUIRE(!pool.is_active(idx));
}

TEST_CASE("ObjectPool - exhaust pool", "[pool]") {
  ObjectPool<int, 3> pool;
  int32_t a = pool.acquire();
  (void)pool.acquire();  // b
  int32_t c = pool.acquire();
  REQUIRE(a >= 0);

  REQUIRE(c >= 0);
  REQUIRE(pool.available() == 0);

  // Should fail
  int32_t d = pool.acquire();
  REQUIRE(d == -1);
}

TEST_CASE("ObjectPool - release and reacquire", "[pool]") {
  ObjectPool<int, 2> pool;
  int32_t a = pool.acquire();
  int32_t b = pool.acquire();
  (void)b;
  REQUIRE(pool.acquire() == -1);

  pool.release(a);
  REQUIRE(pool.available() == 1);

  int32_t c = pool.acquire();
  REQUIRE(c >= 0);
  REQUIRE(pool.available() == 0);
}

TEST_CASE("ObjectPool - double release ignored", "[pool]") {
  ObjectPool<int, 4> pool;
  int32_t idx = pool.acquire();
  pool.release(idx);
  pool.release(idx);  // Should be safe (no-op)
  REQUIRE(pool.available() == 4);
}

TEST_CASE("ObjectPool - invalid index release", "[pool]") {
  ObjectPool<int, 4> pool;
  pool.release(-1);   // Should be safe
  pool.release(100);  // Should be safe
  REQUIRE(pool.available() == 4);
}

TEST_CASE("ObjectPool - is_active checks", "[pool]") {
  ObjectPool<int, 4> pool;
  REQUIRE(!pool.is_active(-1));
  REQUIRE(!pool.is_active(100));
  REQUIRE(!pool.is_active(0));

  int32_t idx = pool.acquire();
  REQUIRE(pool.is_active(idx));
  pool.release(idx);
  REQUIRE(!pool.is_active(idx));
}

TEST_CASE("ObjectPool - get typed pointer", "[pool]") {
  ObjectPool<uint32_t, 4> pool;
  int32_t idx = pool.acquire();
  uint32_t* ptr = pool.get(idx);
  REQUIRE(ptr != nullptr);
  *ptr = 12345;
  REQUIRE(*pool.get(idx) == 12345);
  pool.release(idx);
}

TEST_CASE("ObjectPool - reset", "[pool]") {
  ObjectPool<int, 4> pool;
  pool.acquire();
  pool.acquire();
  REQUIRE(pool.in_use() == 2);

  pool.reset();
  REQUIRE(pool.in_use() == 0);
  REQUIRE(pool.available() == 4);
}

// ============================================================================
// ServerStats
// ============================================================================

TEST_CASE("ServerStats - initial state", "[stats]") {
  ServerStats stats;
  REQUIRE(stats.total_connections.load() == 0);
  REQUIRE(stats.active_connections.load() == 0);
  REQUIRE(stats.socket_errors.load() == 0);
}

TEST_CASE("ServerStats - increment counters", "[stats]") {
  ServerStats stats;
  stats.total_connections.fetch_add(1, std::memory_order_relaxed);
  stats.active_connections.fetch_add(1, std::memory_order_relaxed);
  stats.total_messages_in.fetch_add(100, std::memory_order_relaxed);
  stats.total_bytes_in.fetch_add(5000, std::memory_order_relaxed);

  REQUIRE(stats.total_connections.load() == 1);
  REQUIRE(stats.active_connections.load() == 1);
  REQUIRE(stats.total_messages_in.load() == 100);
  REQUIRE(stats.total_bytes_in.load() == 5000);
}

TEST_CASE("ServerStats - reset", "[stats]") {
  ServerStats stats;
  stats.total_connections = 10;
  stats.socket_errors = 5;
  stats.max_poll_latency_us = 1000;

  stats.reset();
  REQUIRE(stats.total_connections.load() == 0);
  REQUIRE(stats.socket_errors.load() == 0);
  REQUIRE(stats.max_poll_latency_us.load() == 0);
}

TEST_CASE("ServerStats - overload detection", "[stats]") {
  ServerStats stats;
  // 90% of 100 = 90
  stats.active_connections = 89;
  REQUIRE(!stats.is_overloaded(100));

  stats.active_connections = 91;
  REQUIRE(stats.is_overloaded(100));
}

TEST_CASE("ServerStats - overload edge case", "[stats]") {
  ServerStats stats;
  // 90% of 10 = 9
  stats.active_connections = 9;
  REQUIRE(!stats.is_overloaded(10));

  stats.active_connections = 10;
  REQUIRE(stats.is_overloaded(10));
}

TEST_CASE("ServerStats - all counters reset", "[stats]") {
  ServerStats stats;
  stats.total_messages_in = 1;
  stats.total_messages_out = 2;
  stats.total_bytes_in = 3;
  stats.total_bytes_out = 4;
  stats.total_connections = 5;
  stats.active_connections = 6;
  stats.rejected_connections = 7;
  stats.handshake_errors = 8;
  stats.socket_errors = 9;
  stats.buffer_overflows = 10;
  stats.last_poll_latency_us = 11;
  stats.max_poll_latency_us = 12;
  stats.pool_acquires = 13;
  stats.pool_releases = 14;
  stats.pool_exhausted = 15;

  stats.reset();

  REQUIRE(stats.total_messages_in.load() == 0);
  REQUIRE(stats.total_messages_out.load() == 0);
  REQUIRE(stats.total_bytes_in.load() == 0);
  REQUIRE(stats.total_bytes_out.load() == 0);
  REQUIRE(stats.total_connections.load() == 0);
  REQUIRE(stats.active_connections.load() == 0);
  REQUIRE(stats.rejected_connections.load() == 0);
  REQUIRE(stats.handshake_errors.load() == 0);
  REQUIRE(stats.socket_errors.load() == 0);
  REQUIRE(stats.buffer_overflows.load() == 0);
  REQUIRE(stats.last_poll_latency_us.load() == 0);
  REQUIRE(stats.max_poll_latency_us.load() == 0);
  REQUIRE(stats.pool_acquires.load() == 0);
  REQUIRE(stats.pool_releases.load() == 0);
  REQUIRE(stats.pool_exhausted.load() == 0);
}
