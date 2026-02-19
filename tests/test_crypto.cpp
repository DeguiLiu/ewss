#include "ewss.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ewss;

// ============================================================================
// Base64 Tests (matching Simple-WebSocket-Server crypto_test vectors)
// ============================================================================

TEST_CASE("Base64 encode empty", "[crypto]") {
  std::string encoded = Base64::encode(nullptr, 0);
  REQUIRE(encoded.empty());
}

TEST_CASE("Base64 encode 'f'", "[crypto]") {
  const uint8_t data[] = {'f'};
  REQUIRE(Base64::encode(data, 1) == "Zg==");
}

TEST_CASE("Base64 encode 'fo'", "[crypto]") {
  const uint8_t data[] = {'f', 'o'};
  REQUIRE(Base64::encode(data, 2) == "Zm8=");
}

TEST_CASE("Base64 encode 'foo'", "[crypto]") {
  const uint8_t data[] = {'f', 'o', 'o'};
  REQUIRE(Base64::encode(data, 3) == "Zm9v");
}

TEST_CASE("Base64 encode 'foob'", "[crypto]") {
  const uint8_t data[] = {'f', 'o', 'o', 'b'};
  REQUIRE(Base64::encode(data, 4) == "Zm9vYg==");
}

TEST_CASE("Base64 encode 'fooba'", "[crypto]") {
  const uint8_t data[] = {'f', 'o', 'o', 'b', 'a'};
  REQUIRE(Base64::encode(data, 5) == "Zm9vYmE=");
}

TEST_CASE("Base64 encode 'foobar'", "[crypto]") {
  const uint8_t data[] = {'f', 'o', 'o', 'b', 'a', 'r'};
  REQUIRE(Base64::encode(data, 6) == "Zm9vYmFy");
}

TEST_CASE("Base64 encode long string", "[crypto]") {
  std::string input =
      "The itsy bitsy spider climbed up the waterspout.\r\n"
      "Down came the rain\r\n"
      "and washed the spider out.\r\n"
      "Out came the sun\r\n"
      "and dried up all the rain\r\n"
      "and the itsy bitsy spider climbed up the spout again.";
  std::string expected =
      "VGhlIGl0c3kgYml0c3kgc3BpZGVyIGNsaW1iZWQgdXAgdGhlIHdhdGVyc3BvdXQuDQpE"
      "b3duIGNhbWUgdGhlIHJhaW4NCmFuZCB3YXNoZWQgdGhlIHNwaWRlciBvdXQuDQpPdXQg"
      "Y2FtZSB0aGUgc3VuDQphbmQgZHJpZWQgdXAgYWxsIHRoZSByYWluDQphbmQgdGhlIGl0"
      "c3kgYml0c3kgc3BpZGVyIGNsaW1iZWQgdXAgdGhlIHNwb3V0IGFnYWluLg==";
  auto encoded = Base64::encode(reinterpret_cast<const uint8_t*>(input.data()), input.size());
  REQUIRE(encoded == expected);
}

TEST_CASE("Base64 decode empty", "[crypto]") {
  auto decoded = Base64::decode("");
  REQUIRE(decoded.empty());
}

TEST_CASE("Base64 decode roundtrip", "[crypto]") {
  std::string input = "Hello, WebSocket!";
  std::vector<uint8_t> data(input.begin(), input.end());
  std::string encoded = Base64::encode(data.data(), data.size());
  REQUIRE(encoded == "SGVsbG8sIFdlYlNvY2tldCE=");
  auto decoded = Base64::decode(encoded);
  std::string decoded_str(decoded.begin(), decoded.end());
  REQUIRE(decoded_str == input);
}

TEST_CASE("Base64 decode all test vectors", "[crypto]") {
  // Roundtrip all vectors
  std::vector<std::string> inputs = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
  for (const auto& input : inputs) {
    auto encoded = Base64::encode(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    auto decoded = Base64::decode(encoded);
    std::string result(decoded.begin(), decoded.end());
    REQUIRE(result == input);
  }
}

TEST_CASE("Base64 invalid input", "[crypto]") {
  // Odd-length input should return empty
  auto decoded = Base64::decode("abc");
  REQUIRE(decoded.empty());
}

// ============================================================================
// SHA-1 Tests (matching Simple-WebSocket-Server crypto_test vectors)
// ============================================================================

TEST_CASE("SHA1 empty string", "[crypto]") {
  REQUIRE(SHA1::hex_digest("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST_CASE("SHA1 quick brown fox", "[crypto]") {
  REQUIRE(SHA1::hex_digest("The quick brown fox jumps over the lazy dog") ==
          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST_CASE("SHA1 single iteration", "[crypto]") {
  REQUIRE(SHA1::hex_digest("Test") == "640ab2bae07bedc4c163f679a746f7ab7fb5d1fa");
}

TEST_CASE("SHA1 WebSocket accept key", "[crypto]") {
  // RFC 6455 test vector
  std::string input = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  auto hash = SHA1::compute(reinterpret_cast<const uint8_t*>(input.data()), input.size());
  std::string b64 = Base64::encode(hash.data(), hash.size());
  REQUIRE(b64 == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("SHA1 incremental update", "[crypto]") {
  // Compute in two parts should equal single compute
  SHA1 sha1;
  std::string part1 = "The quick brown fox ";
  std::string part2 = "jumps over the lazy dog";
  sha1.update(reinterpret_cast<const uint8_t*>(part1.data()), part1.size());
  sha1.update(reinterpret_cast<const uint8_t*>(part2.data()), part2.size());
  auto hash = sha1.finalize();

  std::string hex;
  for (auto byte : hash) {
    hex += "0123456789abcdef"[byte >> 4];
    hex += "0123456789abcdef"[byte & 0x0f];
  }
  REQUIRE(hex == "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST_CASE("SHA1 exactly 64 bytes (one block)", "[crypto]") {
  // 64 bytes = exactly one SHA1 block
  std::string input(64, 'a');
  auto hex = SHA1::hex_digest(input);
  REQUIRE(hex.size() == 40);
  // Known value for 64 'a's
  REQUIRE(hex == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
}

TEST_CASE("SHA1 55 bytes (padding boundary)", "[crypto]") {
  // 55 bytes: padding fits in same block
  std::string input(55, 'b');
  auto hex = SHA1::hex_digest(input);
  REQUIRE(hex.size() == 40);
}

TEST_CASE("SHA1 56 bytes (padding overflow)", "[crypto]") {
  // 56 bytes: padding overflows to next block
  std::string input(56, 'c');
  auto hex = SHA1::hex_digest(input);
  REQUIRE(hex.size() == 40);
}
