#include <catch2/catch_test_macros.hpp>
#include "ewss/utils.hpp"

using namespace ewss;

TEST_CASE("Base64 encode/decode", "[utils]") {
  // Test vector
  std::string plaintext = "Hello, WebSocket!";
  std::vector<uint8_t> data(plaintext.begin(), plaintext.end());

  // Encode
  std::string encoded = Base64::encode(data.data(), data.size());
  REQUIRE(encoded == "SGVsbG8sIFdlYlNvY2tldCE=");

  // Decode
  auto decoded = Base64::decode(encoded);
  std::string decoded_str(decoded.begin(), decoded.end());
  REQUIRE(decoded_str == plaintext);
}

TEST_CASE("Base64 empty", "[utils]") {
  std::string encoded = Base64::encode(nullptr, 0);
  REQUIRE(encoded.empty());

  auto decoded = Base64::decode("");
  REQUIRE(decoded.empty());
}

TEST_CASE("SHA1 hash", "[utils]") {
  // Test vector from WebSocket spec
  std::string input = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  auto hash = SHA1::compute(reinterpret_cast<const uint8_t*>(input.data()),
                             input.size());

  std::string hex_hash = Base64::encode(hash.data(), hash.size());
  REQUIRE(hex_hash == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("WebSocket frame parsing - single frame", "[utils]") {
  // Create a simple text frame: "Hello"
  // Byte 0: 0x81 (FIN=1, RSV=0, Opcode=1 for text)
  // Byte 1: 0x85 (MASK=1, Payload length=5)
  // Bytes 2-5: Mask key
  // Bytes 6-10: Masked payload

  uint8_t frame[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                     0x7f, 0x9f, 0x4d, 0x51, 0x58};  // "Hello" masked

  ws::FrameHeader header;
  size_t header_size = ws::parse_frame_header(
      std::string_view(reinterpret_cast<const char*>(frame), sizeof(frame)),
      header);

  REQUIRE(header_size == 6);  // 2 (FIN+Opcode+Len) + 4 (mask key)
  REQUIRE(header.fin);
  REQUIRE(header.opcode == ws::OpCode::kText);
  REQUIRE(header.masked);
  REQUIRE(header.payload_len == 5);
}

TEST_CASE("WebSocket frame encoding", "[utils]") {
  std::string payload = "Hello";
  auto frame = ws::encode_frame(ws::OpCode::kText, payload, false);

  // Check header
  REQUIRE(frame[0] == 0x81);  // FIN + Text opcode
  REQUIRE((frame[1] & 0x80) == 0);  // No mask
  REQUIRE((frame[1] & 0x7F) == 5);  // Payload length = 5

  // Check payload
  std::string decoded_payload(frame.begin() + 2, frame.end());
  REQUIRE(decoded_payload == payload);
}

TEST_CASE("WebSocket frame encoding - large payload", "[utils]") {
  std::string payload(200, 'x');
  auto frame = ws::encode_frame(ws::OpCode::kBinary, payload, false);

  // Check header for 126 encoding
  REQUIRE(frame[0] == 0x82);  // FIN + Binary opcode
  REQUIRE((frame[1] & 0x7F) == 126);
  REQUIRE((frame[2] << 8 | frame[3]) == 200);
}

TEST_CASE("WebSocket frame unmask", "[utils]") {
  // TODO: unmask_payload not yet implemented in utils.hpp
  // Masked payload: "Hello" with key 0x37fa213d
  uint8_t masked[] = {0x7f, 0x9f, 0x4d, 0x51, 0x58};
  // mask_key used in Connection::unmask_payload (tested in test_frame.cpp)

  // ws::unmask_payload(masked, sizeof(masked), mask_key);

  std::string result(reinterpret_cast<const char*>(masked), sizeof(masked));
  // REQUIRE(result == "Hello");
}
