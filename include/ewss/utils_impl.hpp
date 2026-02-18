// Inline implementations for Base64 and SHA1
// (split from header for clarity)

#include "ewss/utils.hpp"
#include <cstring>

namespace ewss {

inline std::string Base64::encode(const uint8_t* data, size_t size) {
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

inline uint32_t SHA1::rol(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

}  // namespace ewss
