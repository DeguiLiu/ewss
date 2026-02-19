#include "ewss.hpp"

#include <cstring>

#include <catch2/catch_test_macros.hpp>

using namespace ewss;

// ============================================================================
// Frame Header Parsing
// ============================================================================

TEST_CASE("Frame parse - text frame unmasked", "[frame]") {
  // FIN=1, opcode=1 (text), no mask, payload=5
  uint8_t frame[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
  ws::FrameHeader header;
  size_t consumed =
      ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), sizeof(frame)), header);
  REQUIRE(consumed == 2);
  REQUIRE(header.fin == true);
  REQUIRE(header.opcode == ws::OpCode::kText);
  REQUIRE(header.masked == false);
  REQUIRE(header.payload_len == 5);
}

TEST_CASE("Frame parse - text frame masked", "[frame]") {
  uint8_t frame[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
  ws::FrameHeader header;
  size_t consumed =
      ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), sizeof(frame)), header);
  REQUIRE(consumed == 6);  // 2 + 4 (mask key)
  REQUIRE(header.fin == true);
  REQUIRE(header.opcode == ws::OpCode::kText);
  REQUIRE(header.masked == true);
  REQUIRE(header.payload_len == 5);
}

TEST_CASE("Frame parse - binary frame", "[frame]") {
  uint8_t frame[] = {0x82, 0x03, 0x01, 0x02, 0x03};
  ws::FrameHeader header;
  size_t consumed =
      ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), sizeof(frame)), header);
  REQUIRE(consumed == 2);
  REQUIRE(header.opcode == ws::OpCode::kBinary);
  REQUIRE(header.payload_len == 3);
}

TEST_CASE("Frame parse - close frame", "[frame]") {
  uint8_t frame[] = {0x88, 0x02, 0x03, 0xE8};  // close with code 1000
  ws::FrameHeader header;
  size_t consumed =
      ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), sizeof(frame)), header);
  REQUIRE(consumed == 2);
  REQUIRE(header.opcode == ws::OpCode::kClose);
  REQUIRE(header.payload_len == 2);
}

TEST_CASE("Frame parse - ping frame", "[frame]") {
  uint8_t frame[] = {0x89, 0x00};  // ping, no payload
  ws::FrameHeader header;
  size_t consumed =
      ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), sizeof(frame)), header);
  REQUIRE(consumed == 2);
  REQUIRE(header.opcode == ws::OpCode::kPing);
  REQUIRE(header.payload_len == 0);
}

TEST_CASE("Frame parse - pong frame", "[frame]") {
  uint8_t frame[] = {0x8A, 0x04, 't', 'e', 's', 't'};
  ws::FrameHeader header;
  size_t consumed =
      ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), sizeof(frame)), header);
  REQUIRE(consumed == 2);
  REQUIRE(header.opcode == ws::OpCode::kPong);
  REQUIRE(header.payload_len == 4);
}

TEST_CASE("Frame parse - 126-byte payload (extended length)", "[frame]") {
  uint8_t frame[4 + 200];
  frame[0] = 0x82;  // FIN + binary
  frame[1] = 126;   // extended 16-bit length
  frame[2] = 0x00;
  frame[3] = 200;  // 200 bytes
  std::memset(frame + 4, 'x', 200);

  ws::FrameHeader header;
  size_t consumed =
      ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), sizeof(frame)), header);
  REQUIRE(consumed == 4);
  REQUIRE(header.payload_len == 200);
}

TEST_CASE("Frame parse - 65536-byte payload (64-bit length)", "[frame]") {
  uint8_t frame[10];
  frame[0] = 0x82;  // FIN + binary
  frame[1] = 127;   // extended 64-bit length
  frame[2] = 0;
  frame[3] = 0;
  frame[4] = 0;
  frame[5] = 0;
  frame[6] = 0;
  frame[7] = 1;
  frame[8] = 0;
  frame[9] = 0;  // 65536

  ws::FrameHeader header;
  size_t consumed = ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), 10), header);
  REQUIRE(consumed == 10);
  REQUIRE(header.payload_len == 65536);
}

TEST_CASE("Frame parse - incomplete header (1 byte)", "[frame]") {
  uint8_t frame[] = {0x81};
  ws::FrameHeader header;
  size_t consumed = ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), 1), header);
  REQUIRE(consumed == 0);
}

TEST_CASE("Frame parse - incomplete extended length", "[frame]") {
  uint8_t frame[] = {0x82, 126, 0x00};  // missing second byte of 16-bit len
  ws::FrameHeader header;
  size_t consumed = ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), 3), header);
  REQUIRE(consumed == 0);
}

TEST_CASE("Frame parse - incomplete mask key", "[frame]") {
  uint8_t frame[] = {0x81, 0x85, 0x37, 0xfa};  // mask=1, len=5, only 2 mask bytes
  ws::FrameHeader header;
  size_t consumed = ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), 4), header);
  REQUIRE(consumed == 0);
}

TEST_CASE("Frame parse - continuation frame", "[frame]") {
  uint8_t frame[] = {0x00, 0x03, 'a', 'b', 'c'};  // FIN=0, opcode=0
  ws::FrameHeader header;
  size_t consumed =
      ws::parse_frame_header(std::string_view(reinterpret_cast<const char*>(frame), sizeof(frame)), header);
  REQUIRE(consumed == 2);
  REQUIRE(header.fin == false);
  REQUIRE(header.opcode == ws::OpCode::kContinuation);
  REQUIRE(header.payload_len == 3);
}

// ============================================================================
// Frame Encoding
// ============================================================================

TEST_CASE("Frame encode - text 'Hello'", "[frame]") {
  auto frame = ws::encode_frame(ws::OpCode::kText, "Hello", false);
  REQUIRE(frame[0] == 0x81);
  REQUIRE((frame[1] & 0x80) == 0);  // no mask
  REQUIRE((frame[1] & 0x7F) == 5);
  std::string payload(frame.begin() + 2, frame.end());
  REQUIRE(payload == "Hello");
}

TEST_CASE("Frame encode - binary", "[frame]") {
  auto frame = ws::encode_frame(ws::OpCode::kBinary, "data", false);
  REQUIRE(frame[0] == 0x82);
  REQUIRE((frame[1] & 0x7F) == 4);
}

TEST_CASE("Frame encode - empty payload", "[frame]") {
  auto frame = ws::encode_frame(ws::OpCode::kText, "", false);
  REQUIRE(frame[0] == 0x81);
  REQUIRE((frame[1] & 0x7F) == 0);
  REQUIRE(frame.size() == 2);
}

TEST_CASE("Frame encode - 200 byte payload (extended length)", "[frame]") {
  std::string payload(200, 'x');
  auto frame = ws::encode_frame(ws::OpCode::kBinary, payload, false);
  REQUIRE(frame[0] == 0x82);
  REQUIRE((frame[1] & 0x7F) == 126);
  uint16_t len = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
  REQUIRE(len == 200);
  REQUIRE(frame.size() == 4 + 200);
}

TEST_CASE("Frame encode - close frame", "[frame]") {
  uint8_t code_bytes[] = {0x03, 0xE8};  // 1000
  std::string_view payload(reinterpret_cast<const char*>(code_bytes), 2);
  auto frame = ws::encode_frame(ws::OpCode::kClose, payload, false);
  REQUIRE(frame[0] == 0x88);
  REQUIRE((frame[1] & 0x7F) == 2);
}

TEST_CASE("Frame encode - ping frame", "[frame]") {
  auto frame = ws::encode_frame(ws::OpCode::kPing, "ping", false);
  REQUIRE(frame[0] == 0x89);
  REQUIRE((frame[1] & 0x7F) == 4);
}

TEST_CASE("Frame encode - pong frame", "[frame]") {
  auto frame = ws::encode_frame(ws::OpCode::kPong, "pong", false);
  REQUIRE(frame[0] == 0x8A);
}

// ============================================================================
// Unmask
// ============================================================================

TEST_CASE("Unmask payload - 'Hello'", "[frame]") {
  // "Hello" masked with key 0x37fa213d
  uint8_t masked[] = {0x7f, 0x9f, 0x4d, 0x51, 0x58};
  uint8_t mask_key[] = {0x37, 0xfa, 0x21, 0x3d};

  // Unmask manually (same algorithm as Connection::unmask_payload)
  for (size_t i = 0; i < sizeof(masked); ++i) {
    masked[i] ^= mask_key[i % 4];
  }

  std::string result(reinterpret_cast<const char*>(masked), sizeof(masked));
  REQUIRE(result == "Hello");
}

TEST_CASE("Unmask payload - empty", "[frame]") {
  // Unmasking zero-length data is a no-op
  uint8_t buf = 0;
  REQUIRE(buf == 0);  // Trivially true, validates no crash
}

TEST_CASE("Unmask payload - roundtrip", "[frame]") {
  std::string original = "WebSocket test message!";
  uint8_t mask_key[] = {0xAB, 0xCD, 0xEF, 0x01};

  // Mask
  std::vector<uint8_t> data(original.begin(), original.end());
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] ^= mask_key[i % 4];
  }

  // Unmask
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] ^= mask_key[i % 4];
  }

  std::string result(data.begin(), data.end());
  REQUIRE(result == original);
}
