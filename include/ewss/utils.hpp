#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ewss {

// ============================================================================
// Base64 encoding/decoding
// ============================================================================

class Base64 {
 public:
  static std::string encode(const uint8_t* data, size_t size) {
    static constexpr const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((size + 2) / 3 * 4);

    for (size_t i = 0; i < size; i += 3) {
      uint32_t b = (static_cast<uint32_t>(data[i]) << 16);
      if (i + 1 < size) b |= (static_cast<uint32_t>(data[i + 1]) << 8);
      if (i + 2 < size) b |= static_cast<uint32_t>(data[i + 2]);

      result.push_back(kAlphabet[(b >> 18) & 0x3F]);
      result.push_back(kAlphabet[(b >> 12) & 0x3F]);
      result.push_back(i + 1 < size ? kAlphabet[(b >> 6) & 0x3F] : '=');
      result.push_back(i + 2 < size ? kAlphabet[b & 0x3F] : '=');
    }
    return result;
  }

  static std::vector<uint8_t> decode(std::string_view encoded) {
    static constexpr uint8_t kTable[256] = {
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63, 52, 53, 54,
        55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64, 64, 0,  1,  2,
        3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64, 64, 26, 27, 28, 29, 30,
        31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
        48, 49, 50, 51, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    };

    std::vector<uint8_t> result;
    if (encoded.size() % 4 != 0) return result;
    result.reserve(encoded.size() / 4 * 3);

    for (size_t i = 0; i < encoded.size(); i += 4) {
      uint32_t b = (static_cast<uint32_t>(kTable[static_cast<uint8_t>(
                        encoded[i])]) << 18) |
                   (static_cast<uint32_t>(kTable[static_cast<uint8_t>(
                        encoded[i + 1])]) << 12);
      if (encoded[i + 2] != '=') {
        b |= (static_cast<uint32_t>(kTable[static_cast<uint8_t>(
                  encoded[i + 2])]) << 6);
        if (encoded[i + 3] != '=') {
          b |= static_cast<uint32_t>(
              kTable[static_cast<uint8_t>(encoded[i + 3])]);
          result.push_back((b >> 16) & 0xFF);
          result.push_back((b >> 8) & 0xFF);
          result.push_back(b & 0xFF);
        } else {
          result.push_back((b >> 16) & 0xFF);
          result.push_back((b >> 8) & 0xFF);
        }
      } else {
        result.push_back((b >> 16) & 0xFF);
      }
    }
    return result;
  }
};

// ============================================================================
// SHA-1 hashing (simplified for WebSocket key generation)
// ============================================================================

class SHA1 {
 public:
  static std::array<uint8_t, 20> compute(const uint8_t* data, size_t size) {
    SHA1 sha1;
    sha1.update(data, size);
    return sha1.finalize();
  }

  static std::string hex_digest(std::string_view input) {
    auto hash = compute(reinterpret_cast<const uint8_t*>(input.data()),
                        input.size());
    std::string result;
    result.reserve(40);
    for (auto byte : hash) {
      result += "0123456789abcdef"[byte >> 4];
      result += "0123456789abcdef"[byte & 0x0f];
    }
    return result;
  }

  void update(const uint8_t* data, size_t size) {
    uint32_t r = (h_[0] >> 3) & 0x3F;
    h_[0] += (static_cast<uint32_t>(size) << 3);
    h_[1] += (static_cast<uint32_t>(size) >> 29);

    for (size_t i = 0; i < size; ++i) {
      buffer_[r++] = data[i];
      if (r == 64) {
        process_block(buffer_.data());
        r = 0;
      }
    }
  }

  std::array<uint8_t, 20> finalize() {
    uint32_t r = (h_[0] >> 3) & 0x3F;
    buffer_[r++] = 0x80;
    if (r > 56) {
      for (size_t i = r; i < 64; ++i) buffer_[i] = 0;
      process_block(buffer_.data());
      r = 0;
    }
    for (size_t i = r; i < 56; ++i) buffer_[i] = 0;

    for (int i = 0; i < 8; ++i) {
      buffer_[56 + i] = (h_[1] >> (8 * (7 - i))) & 0xFF;
    }
    process_block(buffer_.data());

    std::array<uint8_t, 20> result;
    for (int i = 0; i < 5; ++i) {
      result[i * 4] = (h_[i] >> 24) & 0xFF;
      result[i * 4 + 1] = (h_[i] >> 16) & 0xFF;
      result[i * 4 + 2] = (h_[i] >> 8) & 0xFF;
      result[i * 4 + 3] = h_[i] & 0xFF;
    }
    return result;
  }

 private:
  std::array<uint32_t, 5> h_ = {0x67452301, 0xefcdab89, 0x98badcfe,
                                 0x10325476, 0xc3d2e1f0};
  std::array<uint8_t, 64> buffer_{};

  static uint32_t rol(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
  }

  void process_block(const uint8_t* block) {
    std::array<uint32_t, 80> w;
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];

    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5a827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ed9eba1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8f1bbcdc;
      } else {
        f = b ^ c ^ d;
        k = 0xca62c1d6;
      }

      uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = temp;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
  }
};

// ============================================================================
// WebSocket utilities
// ============================================================================

namespace ws {

// Frame types
enum class OpCode : uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA
};

struct FrameHeader {
  bool fin;
  OpCode opcode;
  bool masked;
  uint64_t payload_len;
};

// Parse WebSocket frame header from buffer
// Returns bytes consumed, or 0 if incomplete
inline size_t parse_frame_header(std::string_view data, FrameHeader& header) {
  if (data.size() < 2) return 0;

  uint8_t byte0 = data[0];
  uint8_t byte1 = data[1];

  header.fin = (byte0 & 0x80) != 0;
  header.opcode = static_cast<OpCode>(byte0 & 0x0F);
  header.masked = (byte1 & 0x80) != 0;

  uint64_t len = byte1 & 0x7F;
  size_t header_size = 2;

  if (len == 126) {
    if (data.size() < 4) return 0;
    len = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    header_size = 4;
  } else if (len == 127) {
    if (data.size() < 10) return 0;
    len = (static_cast<uint64_t>(data[2]) << 56) |
          (static_cast<uint64_t>(data[3]) << 48) |
          (static_cast<uint64_t>(data[4]) << 40) |
          (static_cast<uint64_t>(data[5]) << 32) |
          (static_cast<uint64_t>(data[6]) << 24) |
          (static_cast<uint64_t>(data[7]) << 16) |
          (static_cast<uint64_t>(data[8]) << 8) | data[9];
    header_size = 10;
  }

  header.payload_len = len;

  if (header.masked) {
    if (data.size() < header_size + 4) return 0;
    header_size += 4;
  }

  return header_size;
}

// Encode WebSocket frame
inline std::vector<uint8_t> encode_frame(OpCode opcode,
                                          std::string_view payload,
                                          bool mask = false) {
  std::vector<uint8_t> frame;
  frame.push_back(0x80 | static_cast<uint8_t>(opcode));

  size_t len = payload.size();
  if (len < 126) {
    frame.push_back((mask ? 0x80 : 0x00) | len);
  } else if (len < 65536) {
    frame.push_back((mask ? 0x80 : 0x00) | 126);
    frame.push_back((len >> 8) & 0xFF);
    frame.push_back(len & 0xFF);
  } else {
    frame.push_back((mask ? 0x80 : 0x00) | 127);
    frame.push_back((len >> 56) & 0xFF);
    frame.push_back((len >> 48) & 0xFF);
    frame.push_back((len >> 40) & 0xFF);
    frame.push_back((len >> 32) & 0xFF);
    frame.push_back((len >> 24) & 0xFF);
    frame.push_back((len >> 16) & 0xFF);
    frame.push_back((len >> 8) & 0xFF);
    frame.push_back(len & 0xFF);
  }

  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

}  // namespace ws

}  // namespace ewss
