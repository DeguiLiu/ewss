#include "ewss/connection.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ewss;

// ============================================================================
// RingBuffer Basic Operations
// ============================================================================

TEST_CASE("RingBuffer - initial state", "[ringbuffer]") {
  RingBuffer<uint8_t, 64> buf;
  REQUIRE(buf.size() == 0);
  REQUIRE(buf.available() == 64);
  REQUIRE(buf.empty() == true);
}

TEST_CASE("RingBuffer - push and peek", "[ringbuffer]") {
  RingBuffer<uint8_t, 64> buf;
  uint8_t data[] = {1, 2, 3, 4, 5};
  REQUIRE(buf.push(data, 5) == true);
  REQUIRE(buf.size() == 5);
  REQUIRE(buf.available() == 59);

  uint8_t out[5];
  size_t peeked = buf.peek(out, 5);
  REQUIRE(peeked == 5);
  REQUIRE(out[0] == 1);
  REQUIRE(out[4] == 5);
}

TEST_CASE("RingBuffer - advance", "[ringbuffer]") {
  RingBuffer<uint8_t, 64> buf;
  uint8_t data[] = {10, 20, 30, 40, 50};
  buf.push(data, 5);
  buf.advance(3);
  REQUIRE(buf.size() == 2);

  uint8_t out[2];
  buf.peek(out, 2);
  REQUIRE(out[0] == 40);
  REQUIRE(out[1] == 50);
}

TEST_CASE("RingBuffer - full buffer", "[ringbuffer]") {
  RingBuffer<uint8_t, 8> buf;
  uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  REQUIRE(buf.push(data, 8) == true);
  REQUIRE(buf.size() == 8);
  REQUIRE(buf.available() == 0);

  // Push should fail when full
  uint8_t extra = 9;
  REQUIRE(buf.push(&extra, 1) == false);
}

TEST_CASE("RingBuffer - empty buffer operations", "[ringbuffer]") {
  RingBuffer<uint8_t, 16> buf;
  uint8_t out[4];
  size_t peeked = buf.peek(out, 4);
  REQUIRE(peeked == 0);

  // Advance on empty should be safe
  buf.advance(10);
  REQUIRE(buf.size() == 0);
}

TEST_CASE("RingBuffer - wrap around", "[ringbuffer]") {
  RingBuffer<uint8_t, 8> buf;

  // Fill partially
  uint8_t data1[] = {1, 2, 3, 4, 5, 6};
  buf.push(data1, 6);

  // Consume some
  buf.advance(4);
  REQUIRE(buf.size() == 2);

  // Push more (wraps around)
  uint8_t data2[] = {7, 8, 9, 10, 11};
  REQUIRE(buf.push(data2, 5) == true);
  REQUIRE(buf.size() == 7);

  // Read all
  uint8_t out[7];
  size_t peeked = buf.peek(out, 7);
  REQUIRE(peeked == 7);
  REQUIRE(out[0] == 5);
  REQUIRE(out[1] == 6);
  REQUIRE(out[2] == 7);
  REQUIRE(out[6] == 11);
}

TEST_CASE("RingBuffer - clear", "[ringbuffer]") {
  RingBuffer<uint8_t, 32> buf;
  uint8_t data[] = {1, 2, 3};
  buf.push(data, 3);
  REQUIRE(buf.size() == 3);

  buf.clear();
  REQUIRE(buf.size() == 0);
  REQUIRE(buf.empty() == true);
  REQUIRE(buf.available() == 32);
}

TEST_CASE("RingBuffer - view contiguous", "[ringbuffer]") {
  RingBuffer<uint8_t, 64> buf;
  const char* msg = "Hello";
  buf.push(reinterpret_cast<const uint8_t*>(msg), 5);

  auto view = buf.view();
  REQUIRE(view == "Hello");
}

TEST_CASE("RingBuffer - fill_iovec contiguous", "[ringbuffer]") {
  RingBuffer<uint8_t, 64> buf;
  uint8_t data[] = {1, 2, 3, 4, 5};
  buf.push(data, 5);

  struct iovec iov[2];
  size_t count = buf.fill_iovec(iov, 2);
  REQUIRE(count == 1);
  REQUIRE(iov[0].iov_len == 5);
}

TEST_CASE("RingBuffer - fill_iovec wrapped", "[ringbuffer]") {
  RingBuffer<uint8_t, 8> buf;

  // Fill and consume to move read pointer
  uint8_t data1[] = {1, 2, 3, 4, 5, 6};
  buf.push(data1, 6);
  buf.advance(5);  // read_idx = 5

  // Push data that wraps
  uint8_t data2[] = {10, 20, 30, 40, 50, 60};
  buf.push(data2, 6);  // wraps around

  struct iovec iov[2];
  size_t count = buf.fill_iovec(iov, 2);
  REQUIRE(count == 2);  // Two chunks (wrap)

  // Total length should equal buffer size
  size_t total = iov[0].iov_len + iov[1].iov_len;
  REQUIRE(total == buf.size());
}

TEST_CASE("RingBuffer - fill_iovec empty", "[ringbuffer]") {
  RingBuffer<uint8_t, 16> buf;
  struct iovec iov[2];
  size_t count = buf.fill_iovec(iov, 2);
  REQUIRE(count == 0);
}

TEST_CASE("RingBuffer - repeated push/advance cycle", "[ringbuffer]") {
  RingBuffer<uint8_t, 16> buf;

  for (int cycle = 0; cycle < 100; ++cycle) {
    uint8_t data[] = {static_cast<uint8_t>(cycle), static_cast<uint8_t>(cycle + 1)};
    REQUIRE(buf.push(data, 2) == true);

    uint8_t out[2];
    buf.peek(out, 2);
    REQUIRE(out[0] == static_cast<uint8_t>(cycle));
    buf.advance(2);
    REQUIRE(buf.empty() == true);
  }
}

TEST_CASE("RingBuffer - partial peek", "[ringbuffer]") {
  RingBuffer<uint8_t, 32> buf;
  uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
  buf.push(data, 8);

  uint8_t out[3];
  size_t peeked = buf.peek(out, 3);
  REQUIRE(peeked == 3);
  REQUIRE(out[0] == 1);
  REQUIRE(out[2] == 3);

  // Size unchanged (peek doesn't consume)
  REQUIRE(buf.size() == 8);
}

TEST_CASE("RingBuffer - advance more than size", "[ringbuffer]") {
  RingBuffer<uint8_t, 16> buf;
  uint8_t data[] = {1, 2, 3};
  buf.push(data, 3);
  buf.advance(100);  // Should clamp to size
  REQUIRE(buf.size() == 0);
}
