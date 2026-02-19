/**
 * MIT License
 *
 * Copyright (c) 2026 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file ewss.hpp
 * @brief EWSS - Embedded WebSocket Server (single-header library)
 *
 * A lightweight WebSocket server for embedded Linux (ARM/x86).
 * Replaces ASIO with poll() reactor, dynamic buffers with fixed RingBuffer,
 * implicit handler chains with function-pointer state machine.
 * 67KB binary, 12KB/connection, zero heap allocation on hot path.
 *
 * Usage:
 *   #include "ewss.hpp"
 *
 *   int main() {
 *     ewss::Server server(8080);
 *     server.on_message = [](const auto& conn, std::string_view msg) {
 *       conn->send(msg);
 *     };
 *     server.run();
 *   }
 *
 * @see https://github.com/DeguiLiu/ewss
 * @see RFC 6455: The WebSocket Protocol
 */

#ifndef EWSS_HPP_
#define EWSS_HPP_

// Exception support for -fno-exceptions builds
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#define EWSS_THROW(ex) throw(ex)
#else
#include <cstdio>
#include <cstdlib>
#define EWSS_THROW(ex)            \
  do {                            \
    std::fputs(#ex "\n", stderr); \
    std::abort();                 \
  } while (0)
#endif

#ifndef EWSS_ASSERT
#define EWSS_ASSERT(cond) ((void)(cond))
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sockpp/tcp_socket.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace ewss {

static constexpr size_t kCacheLine = 64;

// ============================================================================
// Error Types
// ============================================================================

enum class ErrorCode : uint8_t {
  kOk = 0,
  kBufferFull = 1,
  kBufferEmpty = 2,
  kHandshakeFailed = 3,
  kFrameParseError = 4,
  kConnectionClosed = 5,
  kInvalidState = 6,
  kSocketError = 7,
  kTimeout = 8,
  kMaxConnectionsExceeded = 9,
  kInternalError = 255
};

// ============================================================================
// expected<V, E> - Lightweight error-or-value type
// ============================================================================

template <typename V, typename E>
class expected final {
 public:
  static expected success(const V& val) noexcept {
    expected e;
    e.has_value_ = true;
    ::new (&e.storage_) V(val);
    return e;
  }
  static expected success(V&& val) noexcept {
    expected e;
    e.has_value_ = true;
    ::new (&e.storage_) V(static_cast<V&&>(val));
    return e;
  }
  static expected error(E err) noexcept {
    expected e;
    e.has_value_ = false;
    e.err_ = err;
    return e;
  }

  expected(const expected& o) noexcept : storage_{}, err_(o.err_), has_value_(o.has_value_) {
    if (has_value_) ::new (&storage_) V(o.value());
  }
  expected& operator=(const expected& o) noexcept {
    if (this != &o) {
      if (has_value_) reinterpret_cast<V*>(&storage_)->~V();
      has_value_ = o.has_value_;
      err_ = o.err_;
      if (has_value_) ::new (&storage_) V(o.value());
    }
    return *this;
  }
  expected(expected&& o) noexcept : storage_{}, err_(o.err_), has_value_(o.has_value_) {
    if (has_value_) ::new (&storage_) V(static_cast<V&&>(o.value()));
  }
  expected& operator=(expected&& o) noexcept {
    if (this != &o) {
      if (has_value_) reinterpret_cast<V*>(&storage_)->~V();
      has_value_ = o.has_value_;
      err_ = o.err_;
      if (has_value_) ::new (&storage_) V(static_cast<V&&>(o.value()));
    }
    return *this;
  }
  ~expected() {
    if (has_value_) reinterpret_cast<V*>(&storage_)->~V();
  }

  [[nodiscard]] bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }
  V& value() & noexcept { return *reinterpret_cast<V*>(&storage_); }
  const V& value() const& noexcept { return *reinterpret_cast<const V*>(&storage_); }
  E get_error() const noexcept { return err_; }
  V value_or(const V& d) const noexcept { return has_value_ ? value() : d; }

 private:
  expected() noexcept : storage_{}, err_{}, has_value_(false) {}
  typename std::aligned_storage<sizeof(V), alignof(V)>::type storage_{};
  E err_{};
  bool has_value_{false};
};

// Void specialization
template <typename E>
class expected<void, E> final {
 public:
  static expected success() noexcept { expected e; e.has_value_ = true; return e; }
  static expected error(E err) noexcept { expected e; e.err_ = err; return e; }
  [[nodiscard]] bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }
  E get_error() const noexcept { return err_; }
 private:
  expected() noexcept : err_{}, has_value_(false) {}
  E err_{};
  bool has_value_{false};
};

// ============================================================================
// FixedVector<T, Capacity> - Stack-allocated fixed-capacity vector
// ============================================================================

template <typename T, uint32_t Capacity>
class FixedVector final {
  static_assert(Capacity > 0U, "FixedVector capacity must be > 0");
 public:
  using iterator = T*;
  using const_iterator = const T*;

  FixedVector() noexcept {}  // NOLINT
  ~FixedVector() noexcept { clear(); }

  FixedVector(const FixedVector& o) noexcept {
    for (uint32_t i = 0U; i < o.size_; ++i) (void)push_back(o[i]);
  }
  FixedVector& operator=(const FixedVector& o) noexcept {
    if (this != &o) { clear(); for (uint32_t i = 0U; i < o.size_; ++i) (void)push_back(o[i]); }
    return *this;
  }
  FixedVector(FixedVector&& o) noexcept {
    for (uint32_t i = 0U; i < o.size_; ++i) (void)emplace_back(static_cast<T&&>(o[i]));
    o.clear();
  }
  FixedVector& operator=(FixedVector&& o) noexcept {
    if (this != &o) {
      clear();
      for (uint32_t i = 0U; i < o.size_; ++i) (void)emplace_back(static_cast<T&&>(o[i]));
      o.clear();
    }
    return *this;
  }

  T& operator[](uint32_t i) noexcept { return *reinterpret_cast<T*>(storage_ + i * sizeof(T)); }
  const T& operator[](uint32_t i) const noexcept { return *reinterpret_cast<const T*>(storage_ + i * sizeof(T)); }

  T& front() noexcept { return (*this)[0U]; }
  const T& front() const noexcept { return (*this)[0U]; }
  T& back() noexcept { return (*this)[size_ - 1U]; }
  const T& back() const noexcept { return (*this)[size_ - 1U]; }

  T* data() noexcept { return reinterpret_cast<T*>(storage_); }
  const T* data() const noexcept { return reinterpret_cast<const T*>(storage_); }
  iterator begin() noexcept { return data(); }
  const_iterator begin() const noexcept { return data(); }
  iterator end() noexcept { return data() + size_; }
  const_iterator end() const noexcept { return data() + size_; }

  [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
  [[nodiscard]] uint32_t size() const noexcept { return size_; }
  static constexpr uint32_t capacity() noexcept { return Capacity; }
  [[nodiscard]] bool full() const noexcept { return size_ >= Capacity; }

  bool push_back(const T& v) noexcept { return emplace_back(v); }
  bool push_back(T&& v) noexcept { return emplace_back(static_cast<T&&>(v)); }

  template <typename... Args>
  bool emplace_back(Args&&... args) noexcept {
    if (size_ >= Capacity) return false;
    ::new (storage_ + size_ * sizeof(T)) T{static_cast<Args&&>(args)...};
    ++size_;
    return true;
  }

  bool pop_back() noexcept {
    if (size_ == 0U) return false;
    --size_;
    (*this)[size_].~T();
    return true;
  }

  void clear() noexcept {
    while (size_ > 0U) { --size_; (*this)[size_].~T(); }
  }

 private:
  alignas(T) uint8_t storage_[sizeof(T) * Capacity];
  uint32_t size_{0U};
};

// ============================================================================
// optional<T> - Lightweight nullable value
// ============================================================================

template <typename T>
class optional final {
 public:
  optional() noexcept : has_value_(false) {}
  optional(const T& val) noexcept : has_value_(true) { ::new (&storage_) T(val); }  // NOLINT
  optional(T&& val) noexcept : has_value_(true) { ::new (&storage_) T(static_cast<T&&>(val)); }  // NOLINT

  optional(const optional& o) noexcept : has_value_(o.has_value_) {
    if (has_value_) ::new (&storage_) T(o.value());
  }
  optional& operator=(const optional& o) noexcept {
    if (this != &o) { reset(); has_value_ = o.has_value_; if (has_value_) ::new (&storage_) T(o.value()); }
    return *this;
  }
  optional(optional&& o) noexcept : has_value_(o.has_value_) {
    if (has_value_) ::new (&storage_) T(static_cast<T&&>(o.value()));
  }
  optional& operator=(optional&& o) noexcept {
    if (this != &o) { reset(); has_value_ = o.has_value_; if (has_value_) ::new (&storage_) T(static_cast<T&&>(o.value())); }
    return *this;
  }
  ~optional() { reset(); }

  [[nodiscard]] bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }
  T& value() noexcept { return *reinterpret_cast<T*>(&storage_); }
  const T& value() const noexcept { return *reinterpret_cast<const T*>(&storage_); }
  T value_or(const T& d) const noexcept { return has_value_ ? value() : d; }

  void reset() noexcept {
    if (has_value_) { reinterpret_cast<T*>(&storage_)->~T(); has_value_ = false; }
  }

 private:
  typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
  bool has_value_;
};

// ============================================================================
// FixedString<Capacity> - Stack-allocated fixed-capacity string
// ============================================================================

template <uint32_t Capacity>
class FixedString {
  static_assert(Capacity > 0U, "FixedString capacity must be > 0");
 public:
  constexpr FixedString() noexcept : buf_{'\0'}, size_(0U) {}

  template <uint32_t N, typename = typename std::enable_if<(N <= Capacity + 1U)>::type>
  FixedString(const char (&str)[N]) noexcept : size_(N - 1U) {  // NOLINT
    static_assert(N > 0U, "String literal must include null terminator");
    static_assert(N - 1U <= Capacity, "String literal exceeds capacity");
    std::memcpy(buf_, str, N);
  }

  [[nodiscard]] constexpr const char* c_str() const noexcept { return buf_; }
  [[nodiscard]] constexpr uint32_t size() const noexcept { return size_; }
  static constexpr uint32_t capacity() noexcept { return Capacity; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0U; }
  void clear() noexcept { size_ = 0U; buf_[0] = '\0'; }

 private:
  char buf_[Capacity + 1U];
  uint32_t size_;
};

// ============================================================================
// FixedFunction<Sig, BufferSize> - SBO callback (move-only)
// ============================================================================

template <typename Signature, size_t BufferSize = 2 * sizeof(void*)>
class FixedFunction;

template <typename Ret, typename... Args, size_t BufferSize>
class FixedFunction<Ret(Args...), BufferSize> final {
 public:
  FixedFunction() noexcept = default;
  FixedFunction(std::nullptr_t) noexcept {}  // NOLINT

  template <typename F,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<F>::type, FixedFunction>::value &&
                !std::is_same<typename std::decay<F>::type, std::nullptr_t>::value>::type>
  FixedFunction(F&& f) noexcept {  // NOLINT
    using Decay = typename std::decay<F>::type;
    static_assert(sizeof(Decay) <= BufferSize, "Callable too large for FixedFunction buffer");
    static_assert(alignof(Decay) <= alignof(Storage), "Callable alignment exceeds buffer alignment");
    ::new (&storage_) Decay(static_cast<F&&>(f));
    invoker_ = [](const Storage& s, Args... args) -> Ret {
      return (*reinterpret_cast<const Decay*>(&s))(static_cast<Args&&>(args)...);
    };
    destroyer_ = [](Storage& s) { reinterpret_cast<Decay*>(&s)->~Decay(); };
  }

  FixedFunction(FixedFunction&& o) noexcept : invoker_(o.invoker_), destroyer_(o.destroyer_) {
    if (o.invoker_) { std::memcpy(&storage_, &o.storage_, BufferSize); o.invoker_ = nullptr; o.destroyer_ = nullptr; }
  }
  FixedFunction& operator=(FixedFunction&& o) noexcept {
    if (this != &o) {
      if (destroyer_) destroyer_(storage_);
      invoker_ = o.invoker_; destroyer_ = o.destroyer_;
      if (o.invoker_) { std::memcpy(&storage_, &o.storage_, BufferSize); o.invoker_ = nullptr; o.destroyer_ = nullptr; }
    }
    return *this;
  }
  FixedFunction& operator=(std::nullptr_t) noexcept {
    if (destroyer_) destroyer_(storage_);
    invoker_ = nullptr; destroyer_ = nullptr;
    return *this;
  }
  ~FixedFunction() { if (destroyer_) destroyer_(storage_); }

  FixedFunction(const FixedFunction&) = delete;
  FixedFunction& operator=(const FixedFunction&) = delete;

  Ret operator()(Args... args) const { return invoker_(storage_, static_cast<Args&&>(args)...); }
  explicit operator bool() const noexcept { return invoker_ != nullptr; }

 private:
  using Storage = typename std::aligned_storage<BufferSize, alignof(void*)>::type;
  using Invoker = Ret (*)(const Storage&, Args...);
  using Destroyer = void (*)(Storage&);
  Storage storage_{};
  Invoker invoker_ = nullptr;
  Destroyer destroyer_ = nullptr;
};

// ============================================================================
// function_ref<Sig> - Non-owning callable reference (2 pointers)
// ============================================================================

template <typename Sig>
class function_ref;

template <typename Ret, typename... Args>
class function_ref<Ret(Args...)> final {
 public:
  template <typename F,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<F>::type, function_ref>::value>::type>
  function_ref(F&& f) noexcept  // NOLINT
      : obj_(const_cast<void*>(static_cast<const void*>(&f))),
        invoker_([](void* o, Args... args) -> Ret {
          return (*static_cast<typename std::remove_reference<F>::type*>(o))(static_cast<Args&&>(args)...);
        }) {}

  function_ref(Ret (*fn)(Args...)) noexcept  // NOLINT
      : obj_(reinterpret_cast<void*>(fn)),
        invoker_([](void* o, Args... args) -> Ret {
          return reinterpret_cast<Ret (*)(Args...)>(o)(static_cast<Args&&>(args)...);
        }) {}

  Ret operator()(Args... args) const { return invoker_(obj_, static_cast<Args&&>(args)...); }

 private:
  void* obj_;
  Ret (*invoker_)(void*, Args...);
};

// ============================================================================
// ScopeGuard - RAII cleanup guard
// ============================================================================

class ScopeGuard final {
 public:
  explicit ScopeGuard(FixedFunction<void()> cleanup) noexcept
      : cleanup_(static_cast<FixedFunction<void()>&&>(cleanup)) {}

  ~ScopeGuard() { if (active_ && cleanup_) cleanup_(); }

  void release() noexcept { active_ = false; }

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

  ScopeGuard(ScopeGuard&& o) noexcept
      : cleanup_(static_cast<FixedFunction<void()>&&>(o.cleanup_)), active_(o.active_) {
    o.active_ = false;
  }
  ScopeGuard& operator=(ScopeGuard&&) = delete;

 private:
  FixedFunction<void()> cleanup_;
  bool active_{true};
};

// ============================================================================
// Base64 encoding/decoding
// ============================================================================

class Base64 {
 public:
  static inline std::string encode(const uint8_t* data, size_t size) {
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

  static inline std::vector<uint8_t> decode(std::string_view encoded) {
    static constexpr uint8_t kTable[256] = {
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
        64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
        64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    };
    std::vector<uint8_t> result;
    if (encoded.size() % 4 != 0) return result;
    result.reserve(encoded.size() / 4 * 3);
    for (size_t i = 0; i < encoded.size(); i += 4) {
      uint32_t b =
          (static_cast<uint32_t>(kTable[static_cast<uint8_t>(encoded[i])]) << 18) |
          (static_cast<uint32_t>(kTable[static_cast<uint8_t>(encoded[i + 1])]) << 12);
      if (encoded[i + 2] != '=') {
        b |= (static_cast<uint32_t>(kTable[static_cast<uint8_t>(encoded[i + 2])]) << 6);
        if (encoded[i + 3] != '=') {
          b |= static_cast<uint32_t>(kTable[static_cast<uint8_t>(encoded[i + 3])]);
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
// SHA-1 hashing (for WebSocket accept key generation)
// ============================================================================

class SHA1 {
 public:
  static inline std::array<uint8_t, 20> compute(const uint8_t* data, size_t size) {
    SHA1 sha1;
    sha1.update(data, size);
    return sha1.finalize();
  }

  static inline std::string hex_digest(std::string_view input) {
    auto hash = compute(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    std::string result;
    result.reserve(40);
    for (auto byte : hash) {
      result += "0123456789abcdef"[byte >> 4];
      result += "0123456789abcdef"[byte & 0x0f];
    }
    return result;
  }

  void update(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      buffer_[buf_pos_++] = data[i];
      ++total_bytes_;
      if (buf_pos_ == 64) { process_block(buffer_.data()); buf_pos_ = 0; }
    }
  }

  std::array<uint8_t, 20> finalize() {
    buffer_[buf_pos_++] = 0x80;
    if (buf_pos_ > 56) {
      while (buf_pos_ < 64) buffer_[buf_pos_++] = 0;
      process_block(buffer_.data());
      buf_pos_ = 0;
    }
    while (buf_pos_ < 56) buffer_[buf_pos_++] = 0;
    uint64_t total_bits = total_bytes_ * 8;
    for (int i = 7; i >= 0; --i)
      buffer_[56 + (7 - i)] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
    process_block(buffer_.data());
    std::array<uint8_t, 20> result;
    for (int i = 0; i < 5; ++i) {
      result[i * 4]     = static_cast<uint8_t>((h_[i] >> 24) & 0xFF);
      result[i * 4 + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xFF);
      result[i * 4 + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xFF);
      result[i * 4 + 3] = static_cast<uint8_t>(h_[i] & 0xFF);
    }
    return result;
  }

 private:
  std::array<uint32_t, 5> h_ = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::array<uint8_t, 64> buffer_{};
  uint32_t buf_pos_ = 0;
  uint64_t total_bytes_ = 0;

  static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

  void process_block(const uint8_t* block) {
    std::array<uint32_t, 80> w;
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<uint32_t>(block[i * 4 + 3]);
    for (int i = 16; i < 80; ++i)
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f = 0, k = 0;
      if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else              { f = b ^ c ^ d;             k = 0xCA62C1D6; }
      uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
  }
};

// ============================================================================
// WebSocket frame utilities
// ============================================================================

namespace ws {

enum class OpCode : uint8_t {
  kContinuation = 0x0, kText = 0x1, kBinary = 0x2,
  kClose = 0x8, kPing = 0x9, kPong = 0xA
};

struct FrameHeader {
  bool fin;
  OpCode opcode;
  bool masked;
  uint64_t payload_len;
};

inline size_t parse_frame_header(std::string_view data, FrameHeader& header) {
  if (data.size() < 2) return 0;
  header.fin = (data[0] & 0x80) != 0;
  header.opcode = static_cast<OpCode>(data[0] & 0x0F);
  header.masked = (data[1] & 0x80) != 0;
  uint64_t len = data[1] & 0x7F;
  size_t header_size = 2;
  if (len == 126) {
    if (data.size() < 4) return 0;
    len = (static_cast<uint64_t>(static_cast<uint8_t>(data[2])) << 8) |
          static_cast<uint64_t>(static_cast<uint8_t>(data[3]));
    header_size = 4;
  } else if (len == 127) {
    if (data.size() < 10) return 0;
    len = 0;
    for (int i = 2; i < 10; ++i)
      len = (len << 8) | static_cast<uint64_t>(static_cast<uint8_t>(data[i]));
    header_size = 10;
  }
  header.payload_len = len;
  if (header.masked) {
    if (data.size() < header_size + 4) return 0;
    header_size += 4;
  }
  return header_size;
}

inline std::vector<uint8_t> encode_frame(OpCode opcode, std::string_view payload, bool mask = false) {
  std::vector<uint8_t> frame;
  frame.push_back(0x80 | static_cast<uint8_t>(opcode));
  size_t len = payload.size();
  if (len < 126) {
    frame.push_back(static_cast<uint8_t>((mask ? 0x80 : 0x00) | len));
  } else if (len < 65536) {
    frame.push_back(static_cast<uint8_t>((mask ? 0x80 : 0x00) | 126));
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
  } else {
    frame.push_back(static_cast<uint8_t>((mask ? 0x80 : 0x00) | 127));
    for (int i = 7; i >= 0; --i)
      frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

inline size_t encode_frame_header(uint8_t* buf, OpCode opcode, size_t payload_len, bool mask = false) {
  size_t pos = 0;
  buf[pos++] = 0x80 | static_cast<uint8_t>(opcode);
  if (payload_len < 126) {
    buf[pos++] = static_cast<uint8_t>((mask ? 0x80 : 0x00) | payload_len);
  } else if (payload_len < 65536) {
    buf[pos++] = static_cast<uint8_t>((mask ? 0x80 : 0x00) | 126);
    buf[pos++] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>(payload_len & 0xFF);
  } else {
    buf[pos++] = static_cast<uint8_t>((mask ? 0x80 : 0x00) | 127);
    for (int i = 7; i >= 0; --i)
      buf[pos++] = static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF);
  }
  return pos;
}

}  // namespace ws

// ============================================================================
// ObjectPool - O(1) acquire/release, zero heap allocation at runtime
// ============================================================================

template <typename T, size_t MaxSlots>
class alignas(kCacheLine) ObjectPool {
 public:
  ObjectPool() { reset(); }

  void reset() {
    for (size_t i = 0; i < MaxSlots; ++i) { free_list_[i] = i; slot_active_[i] = false; }
    free_count_ = MaxSlots;
  }

  int32_t acquire() {
    if (free_count_ == 0) return -1;
    --free_count_;
    int32_t idx = static_cast<int32_t>(free_list_[free_count_]);
    slot_active_[idx] = true;
    return idx;
  }

  void release(int32_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= MaxSlots) return;
    if (!slot_active_[idx]) return;
    slot_active_[idx] = false;
    free_list_[free_count_] = static_cast<size_t>(idx);
    ++free_count_;
  }

  void* storage(int32_t idx) { return &slots_[static_cast<size_t>(idx) * sizeof(T)]; }
  T* get(int32_t idx) { return reinterpret_cast<T*>(storage(idx)); }

  size_t available() const { return free_count_; }
  size_t capacity() const { return MaxSlots; }
  size_t in_use() const { return MaxSlots - free_count_; }
  bool is_active(int32_t idx) const {
    return idx >= 0 && static_cast<size_t>(idx) < MaxSlots && slot_active_[idx];
  }

 private:
  alignas(kCacheLine) uint8_t slots_[MaxSlots * sizeof(T)]{};
  std::array<size_t, MaxSlots> free_list_{};
  size_t free_count_ = 0;
  std::array<bool, MaxSlots> slot_active_{};
};

// ============================================================================
// ServerStats - Atomic performance counters
// ============================================================================

struct alignas(kCacheLine) ServerStats {
  std::atomic<uint64_t> total_messages_in{0};
  std::atomic<uint64_t> total_messages_out{0};
  std::atomic<uint64_t> total_bytes_in{0};
  std::atomic<uint64_t> total_bytes_out{0};
  std::atomic<uint64_t> total_connections{0};
  std::atomic<uint64_t> active_connections{0};
  std::atomic<uint64_t> rejected_connections{0};
  std::atomic<uint64_t> handshake_errors{0};
  std::atomic<uint64_t> socket_errors{0};
  std::atomic<uint64_t> buffer_overflows{0};
  std::atomic<uint64_t> last_poll_latency_us{0};
  std::atomic<uint64_t> max_poll_latency_us{0};
  std::atomic<uint64_t> pool_acquires{0};
  std::atomic<uint64_t> pool_releases{0};
  std::atomic<uint64_t> pool_exhausted{0};

  void reset() {
    total_messages_in = 0; total_messages_out = 0;
    total_bytes_in = 0; total_bytes_out = 0;
    total_connections = 0; active_connections = 0; rejected_connections = 0;
    handshake_errors = 0; socket_errors = 0; buffer_overflows = 0;
    last_poll_latency_us = 0; max_poll_latency_us = 0;
    pool_acquires = 0; pool_releases = 0; pool_exhausted = 0;
  }

  bool is_overloaded(size_t pool_capacity) const {
    uint64_t active = active_connections.load(std::memory_order_relaxed);
    return active > (pool_capacity * 9 / 10);
  }
};

// ============================================================================
// RingBuffer - Fixed-size circular buffer with zero-copy iovec I/O
// ============================================================================

template <typename T, size_t Size>
class alignas(kCacheLine) RingBuffer {
 public:
  static constexpr size_t kCapacity = Size;
  RingBuffer() = default;

  bool push(const T* data, size_t len) {
    if (available() < len) return false;
    for (size_t i = 0; i < len; ++i) {
      buffer_[write_idx_] = data[i];
      write_idx_ = (write_idx_ + 1) % kCapacity;
    }
    count_ += len;
    return true;
  }

  size_t peek(T* data, size_t max_len) const {
    size_t len = std::min(max_len, count_);
    size_t idx = read_idx_;
    for (size_t i = 0; i < len; ++i) {
      data[i] = buffer_[idx];
      idx = (idx + 1) % kCapacity;
    }
    return len;
  }

  void advance(size_t len) {
    if (len > count_) len = count_;
    read_idx_ = (read_idx_ + len) % kCapacity;
    count_ -= len;
  }

  size_t size() const { return count_; }
  size_t available() const { return kCapacity - count_; }
  bool empty() const { return count_ == 0; }

  void clear() { read_idx_ = 0; write_idx_ = 0; count_ = 0; }

  std::string_view view() const {
    if (empty()) return {};
    return std::string_view(reinterpret_cast<const char*>(buffer_.data() + read_idx_), count_);
  }

  // Fill iovec for writev (zero-copy send from read side)
  size_t fill_iovec(struct iovec* iov, size_t max_iov) const {
    if (empty() || max_iov == 0) return 0;
    size_t contiguous = kCapacity - read_idx_;
    if (contiguous >= count_) {
      iov[0].iov_base = const_cast<T*>(buffer_.data() + read_idx_);
      iov[0].iov_len = count_;
      return 1;
    }
    if (max_iov < 2) {
      iov[0].iov_base = const_cast<T*>(buffer_.data() + read_idx_);
      iov[0].iov_len = contiguous;
      return 1;
    }
    iov[0].iov_base = const_cast<T*>(buffer_.data() + read_idx_);
    iov[0].iov_len = contiguous;
    iov[1].iov_base = const_cast<T*>(buffer_.data());
    iov[1].iov_len = count_ - contiguous;
    return 2;
  }

  // Fill iovec for readv (zero-copy receive into write side)
  size_t fill_iovec_write(struct iovec* iov, size_t max_iov) const {
    size_t avail = available();
    if (avail == 0 || max_iov == 0) return 0;
    size_t contiguous = kCapacity - write_idx_;
    if (contiguous >= avail) {
      iov[0].iov_base = const_cast<T*>(buffer_.data() + write_idx_);
      iov[0].iov_len = avail;
      return 1;
    }
    if (max_iov < 2) {
      iov[0].iov_base = const_cast<T*>(buffer_.data() + write_idx_);
      iov[0].iov_len = contiguous;
      return 1;
    }
    iov[0].iov_base = const_cast<T*>(buffer_.data() + write_idx_);
    iov[0].iov_len = contiguous;
    iov[1].iov_base = const_cast<T*>(buffer_.data());
    iov[1].iov_len = avail - contiguous;
    return 2;
  }

  void commit_write(size_t len) {
    if (len > available()) len = available();
    write_idx_ = (write_idx_ + len) % kCapacity;
    count_ += len;
  }

  const T* read_ptr(size_t* out_len) const {
    if (empty()) { *out_len = 0; return nullptr; }
    size_t contiguous = kCapacity - read_idx_;
    *out_len = (contiguous >= count_) ? count_ : contiguous;
    return buffer_.data() + read_idx_;
  }

 private:
  alignas(kCacheLine) std::array<T, kCapacity> buffer_{};
  size_t read_idx_ = 0;
  size_t write_idx_ = 0;
  size_t count_ = 0;
};

// ============================================================================
// Connection state (function-pointer state machine, no virtual)
// ============================================================================

enum class ConnectionState : uint8_t {
  kHandshaking,  // Waiting for HTTP upgrade request
  kOpen,         // WebSocket connection established
  kClosing,      // Close handshake in progress
  kClosed        // Connection closed
};

class Connection;  // Forward declaration

// State handler function signatures
using StateDataHandler = expected<void, ErrorCode> (*)(Connection& conn);
using StateSendHandler = expected<void, ErrorCode> (*)(Connection& conn, std::string_view payload);
using StateCloseHandler = expected<void, ErrorCode> (*)(Connection& conn, uint16_t code);

// Function pointer table (replaces virtual ProtocolHandler)
struct StateOps {
  ConnectionState state;
  StateDataHandler on_data;
  StateSendHandler on_send;
  StateCloseHandler on_close;
};

// ============================================================================
// Connection - Manages socket, buffers, and protocol state
// ============================================================================

class alignas(kCacheLine) Connection : public std::enable_shared_from_this<Connection> {
 public:
  static constexpr size_t kRxBufferSize = 4096;
  static constexpr size_t kTxBufferSize = 8192;
  static constexpr size_t kTempReadSize = 512;
  static constexpr size_t kHandshakeTimeout = 5000;                  // ms
  static constexpr size_t kCloseTimeout = 5000;                      // ms
  static constexpr size_t kTxHighWatermark = kTxBufferSize * 3 / 4;  // 75%
  static constexpr size_t kTxLowWatermark = kTxBufferSize / 4;       // 25%

  using ConnPtr = std::shared_ptr<Connection>;

  // Construction (implementation below)
  explicit Connection(sockpp::tcp_socket&& sock);
  explicit Connection(int fd);
  ~Connection();

  // Reactor I/O
  expected<void, ErrorCode> handle_read();
  expected<void, ErrorCode> handle_write();
  expected<void, ErrorCode> handle_write_vectored();

  // User API
  void send(std::string_view payload) { send_impl(payload, false); }
  void send_binary(std::string_view payload) { send_impl(payload, true); }
  void close(uint16_t code = 1000);
  bool is_closed() const;
  bool has_data_to_send() const { return !tx_buffer_.empty(); }
  int get_fd() const { return socket_.handle(); }
  uint64_t get_id() const { return id_; }

  // Backpressure
  bool is_write_paused() const { return write_paused_; }
  size_t tx_buffer_usage() const { return tx_buffer_.size(); }

  // Timeout checks
  bool is_handshake_timed_out() const {
    if (get_state() != ConnectionState::kHandshaking) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        SteadyClock::now() - created_at_).count();
    return static_cast<size_t>(elapsed) > kHandshakeTimeout;
  }

  bool is_close_timed_out() const {
    if (get_state() != ConnectionState::kClosing) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        SteadyClock::now() - closing_at_).count();
    return static_cast<size_t>(elapsed) > kCloseTimeout;
  }

  void touch_activity() { last_activity_ = SteadyClock::now(); }
  uint64_t idle_ms() const {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        SteadyClock::now() - last_activity_).count());
  }

  // Callbacks
  std::function<void(const ConnPtr&)> on_open;
  std::function<void(const ConnPtr&, std::string_view)> on_message;
  std::function<void(const ConnPtr&, bool)> on_close;
  std::function<void(const ConnPtr&)> on_error;
  std::function<void(const ConnPtr&)> on_backpressure;
  std::function<void(const ConnPtr&)> on_drain;

  // State query
  ConnectionState get_state() const { return ops_->state; }
  ErrorCode get_last_error() const { return last_error_code_; }

  // Internal API (public to avoid friend, used by state handlers)
  void transition_to_state(ConnectionState state);
  expected<void, ErrorCode> parse_handshake();
  void parse_frames();
  void write_frame(std::string_view payload, ws::OpCode opcode);
  void write_close_frame(uint16_t code);
  void check_high_watermark();
  void check_low_watermark();

  sockpp::tcp_socket& socket() { return socket_; }
  RingBuffer<uint8_t, kRxBufferSize>& rx_buffer() { return rx_buffer_; }
  RingBuffer<uint8_t, kTxBufferSize>& tx_buffer() { return tx_buffer_; }

 private:
  uint64_t id_;
  sockpp::tcp_socket socket_;
  RingBuffer<uint8_t, kRxBufferSize> rx_buffer_;
  RingBuffer<uint8_t, kTxBufferSize> tx_buffer_;
  const StateOps* ops_ = nullptr;
  bool handshake_completed_ = false;
  std::string sec_websocket_key_;
  ErrorCode last_error_code_ = ErrorCode::kOk;
  bool write_paused_ = false;

  using SteadyClock = std::chrono::steady_clock;
  using TimePoint = SteadyClock::time_point;
  TimePoint created_at_ = SteadyClock::now();
  TimePoint closing_at_{};
  TimePoint last_activity_ = SteadyClock::now();

  void send_impl(std::string_view payload, bool binary);
  static std::string generate_accept_key(std::string_view client_key);
  static void unmask_payload(uint8_t* payload, size_t len, const uint8_t* mask_key);
  void log_error(const std::string& msg);
};

// ============================================================================
// TCP Tuning Configuration
// ============================================================================

struct TcpTuning {
  bool tcp_nodelay = false;
  bool tcp_quickack = false;
  bool so_keepalive = false;
  int keepalive_idle_s = 60;
  int keepalive_interval_s = 10;
  int keepalive_count = 5;
};

// ============================================================================
// TLS Configuration (optional mbedTLS, placeholder)
// ============================================================================

struct TlsConfig {
  std::string cert_path;
  std::string key_path;
  std::string ca_path;
  bool require_client_cert = false;
  int min_tls_version = 0;
};

// ============================================================================
// Server - poll() Reactor with zero-copy I/O
// ============================================================================

class Server {
 public:
  using ConnPtr = std::shared_ptr<Connection>;

  explicit Server(uint16_t port, const std::string& bind_addr = "");
  ~Server();

  void run();
  void stop() { is_running_ = false; }

  Server& set_max_connections(size_t max) { max_connections_ = max; return *this; }
  Server& set_poll_timeout_ms(int t) { poll_timeout_ms_ = t; return *this; }
  Server& set_tcp_tuning(const TcpTuning& t) { tcp_tuning_ = t; return *this; }
  Server& set_use_writev(bool e) { use_writev_ = e; return *this; }

  // Callbacks
  std::function<void(const ConnPtr&)> on_connect;
  std::function<void(const ConnPtr&, std::string_view)> on_message;
  std::function<void(const ConnPtr&, bool)> on_close;
  std::function<void(const ConnPtr&)> on_error;
  std::function<void(const ConnPtr&)> on_backpressure;
  std::function<void(const ConnPtr&)> on_drain;

  // Status
  size_t get_connection_count() const { return connections_.size(); }
  const ServerStats& stats() const { return stats_; }
  void reset_stats() { stats_.reset(); }
  uint64_t get_total_socket_errors() const { return stats_.socket_errors.load(); }
  uint64_t get_total_handshake_errors() const { return stats_.handshake_errors.load(); }

  static constexpr size_t kMaxConnections = 64;

 private:
  uint16_t port_;
  std::string bind_addr_;
  int server_sock_ = -1;
  bool is_running_ = false;
  bool use_writev_ = true;
  FixedVector<ConnPtr, kMaxConnections> connections_;
  size_t max_connections_ = 50;
  int poll_timeout_ms_ = 1000;
  TcpTuning tcp_tuning_;
  uint64_t next_conn_id_ = 1;
  std::array<pollfd, kMaxConnections + 1> poll_fds_{};
  ServerStats stats_;

  expected<void, ErrorCode> accept_connection();
  void handle_connection_io(ConnPtr& conn, const pollfd& pfd);
  void remove_closed_connections();
  void apply_tcp_tuning(int fd);
  void log_info(const std::string& msg);
  void log_error(const std::string& msg);
};

// ============================================================================
// Inline implementations
// ============================================================================

// --- State handler functions (replace virtual ProtocolHandler) ---

namespace detail {

inline expected<void, ErrorCode> handshake_on_data(Connection& conn) {
  auto result = conn.parse_handshake();
  if (result.has_value()) {
    conn.transition_to_state(ConnectionState::kOpen);
    return expected<void, ErrorCode>::success();
  }
  return result;
}

inline expected<void, ErrorCode> handshake_on_send(Connection&, std::string_view) {
  return expected<void, ErrorCode>::error(ErrorCode::kInvalidState);
}

inline expected<void, ErrorCode> handshake_on_close(Connection& conn, uint16_t) {
  conn.socket().close();
  conn.transition_to_state(ConnectionState::kClosed);
  return expected<void, ErrorCode>::success();
}

inline expected<void, ErrorCode> open_on_data(Connection& conn) {
  conn.parse_frames();
  return expected<void, ErrorCode>::success();
}

inline expected<void, ErrorCode> open_on_send(Connection& conn, std::string_view payload) {
  conn.write_frame(payload, ws::OpCode::kText);
  return expected<void, ErrorCode>::success();
}

inline expected<void, ErrorCode> open_on_close(Connection& conn, uint16_t code) {
  conn.write_close_frame(code);
  conn.transition_to_state(ConnectionState::kClosing);
  return expected<void, ErrorCode>::success();
}

inline expected<void, ErrorCode> closing_on_data(Connection& conn) {
  uint8_t temp[1024];
  size_t len = conn.rx_buffer().peek(temp, sizeof(temp));
  if (len > 0) {
    std::string_view data(reinterpret_cast<const char*>(temp), len);
    ws::FrameHeader header;
    size_t header_size = ws::parse_frame_header(data, header);
    if (header_size > 0 && header.opcode == ws::OpCode::kClose) {
      conn.transition_to_state(ConnectionState::kClosed);
      conn.socket().close();
    }
  }
  return expected<void, ErrorCode>::success();
}

inline expected<void, ErrorCode> closing_on_send(Connection&, std::string_view) {
  return expected<void, ErrorCode>::error(ErrorCode::kInvalidState);
}

inline expected<void, ErrorCode> closing_on_close(Connection&, uint16_t) {
  return expected<void, ErrorCode>::success();
}

inline expected<void, ErrorCode> closed_on_data(Connection&) {
  return expected<void, ErrorCode>::error(ErrorCode::kConnectionClosed);
}

inline expected<void, ErrorCode> closed_on_send(Connection&, std::string_view) {
  return expected<void, ErrorCode>::error(ErrorCode::kConnectionClosed);
}

inline expected<void, ErrorCode> closed_on_close(Connection&, uint16_t) {
  return expected<void, ErrorCode>::error(ErrorCode::kConnectionClosed);
}

}  // namespace detail

// State operation tables (const, zero allocation)
inline const StateOps kHandshakeOps = {
    ConnectionState::kHandshaking,
    detail::handshake_on_data, detail::handshake_on_send, detail::handshake_on_close};
inline const StateOps kOpenOps = {
    ConnectionState::kOpen,
    detail::open_on_data, detail::open_on_send, detail::open_on_close};
inline const StateOps kClosingOps = {
    ConnectionState::kClosing,
    detail::closing_on_data, detail::closing_on_send, detail::closing_on_close};
inline const StateOps kClosedOps = {
    ConnectionState::kClosed,
    detail::closed_on_data, detail::closed_on_send, detail::closed_on_close};

// --- Connection implementation ---

inline uint64_t& ewss_next_conn_id() {
  static uint64_t id = 1;
  return id;
}

inline Connection::Connection(sockpp::tcp_socket&& sock)
    : id_(ewss_next_conn_id()++), socket_(std::move(sock)) {
  socket_.set_non_blocking(true);
  ops_ = &kHandshakeOps;
}

inline Connection::Connection(int fd)
    : id_(ewss_next_conn_id()++), socket_(fd) {
  socket_.set_non_blocking(true);
  ops_ = &kHandshakeOps;
}

inline Connection::~Connection() {
  if (socket_.is_open()) socket_.close();
}

inline expected<void, ErrorCode> Connection::handle_read() {
  struct iovec iov[2];
  size_t iov_count = rx_buffer_.fill_iovec_write(iov, 2);
  if (iov_count == 0) {
    last_error_code_ = ErrorCode::kBufferFull;
    return expected<void, ErrorCode>::error(ErrorCode::kBufferFull);
  }
  ssize_t n = ::readv(socket_.handle(), iov, static_cast<int>(iov_count));
  if (n > 0) {
    rx_buffer_.commit_write(static_cast<size_t>(n));
    touch_activity();
    ops_->on_data(*this);
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  } else if (n == 0) {
    last_error_code_ = ErrorCode::kConnectionClosed;
    return expected<void, ErrorCode>::error(ErrorCode::kConnectionClosed);
  } else {
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      last_error_code_ = ErrorCode::kSocketError;
      return expected<void, ErrorCode>::error(ErrorCode::kSocketError);
    }
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  }
}

inline expected<void, ErrorCode> Connection::handle_write() {
  if (tx_buffer_.empty()) {
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  }
  uint8_t temp[kTempReadSize];
  size_t len = tx_buffer_.peek(temp, sizeof(temp));
  auto res = socket_.write(temp, len);
  if (res) {
    tx_buffer_.advance(res.value());
    check_low_watermark();
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  } else {
    int err = res.error().value();
    if (err != EAGAIN && err != EWOULDBLOCK) {
      last_error_code_ = ErrorCode::kSocketError;
      return expected<void, ErrorCode>::error(ErrorCode::kSocketError);
    }
  }
  last_error_code_ = ErrorCode::kOk;
  return expected<void, ErrorCode>::success();
}

inline expected<void, ErrorCode> Connection::handle_write_vectored() {
  if (tx_buffer_.empty()) {
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  }
  struct iovec iov[2];
  size_t iov_count = tx_buffer_.fill_iovec(iov, 2);
  if (iov_count == 0) {
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  }
  ssize_t n = ::writev(socket_.handle(), iov, static_cast<int>(iov_count));
  if (n > 0) {
    tx_buffer_.advance(static_cast<size_t>(n));
    check_low_watermark();
    last_error_code_ = ErrorCode::kOk;
    return expected<void, ErrorCode>::success();
  } else if (n < 0) {
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      last_error_code_ = ErrorCode::kSocketError;
      return expected<void, ErrorCode>::error(ErrorCode::kSocketError);
    }
  }
  last_error_code_ = ErrorCode::kOk;
  return expected<void, ErrorCode>::success();
}

inline void Connection::send_impl(std::string_view payload, bool binary) {
  if (get_state() != ConnectionState::kOpen) return;
  ws::OpCode opcode = binary ? ws::OpCode::kBinary : ws::OpCode::kText;
  write_frame(payload, opcode);
  check_high_watermark();
}

inline void Connection::close(uint16_t code) {
  if (get_state() == ConnectionState::kClosed) return;
  if (get_state() == ConnectionState::kOpen) {
    write_close_frame(code);
    transition_to_state(ConnectionState::kClosing);
  } else {
    transition_to_state(ConnectionState::kClosed);
    socket_.close();
  }
}

inline bool Connection::is_closed() const {
  return !socket_.is_open() || get_state() == ConnectionState::kClosed;
}

inline void Connection::transition_to_state(ConnectionState state) {
  switch (state) {
    case ConnectionState::kHandshaking: ops_ = &kHandshakeOps; break;
    case ConnectionState::kOpen:
      ops_ = &kOpenOps;
      if (on_open) on_open(shared_from_this());
      break;
    case ConnectionState::kClosing:
      ops_ = &kClosingOps;
      closing_at_ = SteadyClock::now();
      break;
    case ConnectionState::kClosed:
      ops_ = &kClosedOps;
      if (on_close) on_close(shared_from_this(), true);
      break;
  }
}

inline expected<void, ErrorCode> Connection::parse_handshake() {
  uint8_t temp[1024];
  size_t len = rx_buffer_.peek(temp, sizeof(temp));
  if (len == 0)
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);

  std::string_view data(reinterpret_cast<const char*>(temp), len);
  size_t end_pos = data.find("\r\n\r\n");
  if (end_pos == std::string_view::npos)
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);

  size_t handshake_size = end_pos + 4;

  if (data.substr(0, 4) != "GET ") {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  // Extract Sec-WebSocket-Key
  constexpr std::string_view kKeyHeader = "Sec-WebSocket-Key: ";
  size_t key_pos = data.find(kKeyHeader);
  if (key_pos == std::string_view::npos) {
    constexpr std::string_view kKeyHeaderLower = "sec-websocket-key: ";
    key_pos = data.find(kKeyHeaderLower);
  }
  if (key_pos == std::string_view::npos) {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  size_t value_start = key_pos + kKeyHeader.size();
  size_t value_end = data.find("\r\n", value_start);
  if (value_end == std::string_view::npos) {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  std::string_view ws_key = data.substr(value_start, value_end - value_start);
  while (!ws_key.empty() && (ws_key.back() == ' ' || ws_key.back() == '\t'))
    ws_key.remove_suffix(1);

  if (ws_key.empty()) {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  rx_buffer_.advance(handshake_size);
  std::string accept_key = generate_accept_key(ws_key);

  char response_buf[256];
  int response_len = snprintf(response_buf, sizeof(response_buf),
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n"
      "\r\n",
      accept_key.c_str());

  if (response_len <= 0 || static_cast<size_t>(response_len) >= sizeof(response_buf)) {
    last_error_code_ = ErrorCode::kHandshakeFailed;
    return expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  }

  if (!tx_buffer_.push(reinterpret_cast<const uint8_t*>(response_buf),
                        static_cast<size_t>(response_len))) {
    last_error_code_ = ErrorCode::kBufferFull;
    return expected<void, ErrorCode>::error(ErrorCode::kBufferFull);
  }

  handshake_completed_ = true;
  sec_websocket_key_.clear();
  last_error_code_ = ErrorCode::kOk;
  return expected<void, ErrorCode>::success();
}

inline void Connection::parse_frames() {
  while (true) {
    uint8_t temp[4096];
    size_t len = rx_buffer_.peek(temp, sizeof(temp));
    if (len == 0) break;

    std::string_view data(reinterpret_cast<const char*>(temp), len);
    ws::FrameHeader header;
    size_t header_size = ws::parse_frame_header(data, header);
    if (header_size == 0) break;

    size_t total_frame_size = header_size + header.payload_len;
    if (len < total_frame_size) break;

    const uint8_t* mask_key = nullptr;
    if (header.masked) mask_key = temp + (header_size - 4);

    uint8_t* payload = temp + header_size;
    size_t payload_len = header.payload_len;

    if (header.masked) unmask_payload(payload, payload_len, mask_key);

    switch (header.opcode) {
      case ws::OpCode::kText:
      case ws::OpCode::kBinary:
        if (on_message)
          on_message(shared_from_this(),
                     std::string_view(reinterpret_cast<const char*>(payload), payload_len));
        break;
      case ws::OpCode::kClose:
        if (on_close) on_close(shared_from_this(), false);
        transition_to_state(ConnectionState::kClosed);
        socket_.close();
        return;
      case ws::OpCode::kPing:
        write_frame(std::string_view(reinterpret_cast<const char*>(payload), payload_len),
                    ws::OpCode::kPong);
        break;
      case ws::OpCode::kPong:
        break;
      default:
        break;
    }
    rx_buffer_.advance(total_frame_size);
  }
}

inline void Connection::write_frame(std::string_view payload, ws::OpCode opcode) {
  uint8_t header_buf[14];
  size_t header_len = ws::encode_frame_header(header_buf, opcode, payload.size(), false);
  if (!tx_buffer_.push(header_buf, header_len)) return;
  if (!payload.empty())
    tx_buffer_.push(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

inline void Connection::write_close_frame(uint16_t code) {
  uint8_t close_payload[2];
  close_payload[0] = static_cast<uint8_t>((code >> 8) & 0xFF);
  close_payload[1] = static_cast<uint8_t>(code & 0xFF);
  uint8_t header_buf[14];
  size_t header_len = ws::encode_frame_header(header_buf, ws::OpCode::kClose, 2, false);
  if (!tx_buffer_.push(header_buf, header_len)) return;
  tx_buffer_.push(close_payload, 2);
}

inline std::string Connection::generate_accept_key(std::string_view client_key) {
  constexpr std::string_view kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string key(client_key);
  key.append(kMagic);
  auto hash = SHA1::compute(reinterpret_cast<const uint8_t*>(key.data()), key.size());
  return Base64::encode(hash.data(), hash.size());
}

inline void Connection::unmask_payload(uint8_t* payload, size_t len, const uint8_t* mask_key) {
  for (size_t i = 0; i < len; ++i) payload[i] ^= mask_key[i % 4];
}

inline void Connection::check_high_watermark() {
  if (!write_paused_ && tx_buffer_.size() > kTxHighWatermark) {
    write_paused_ = true;
    if (on_backpressure) on_backpressure(shared_from_this());
  }
}

inline void Connection::check_low_watermark() {
  if (write_paused_ && tx_buffer_.size() < kTxLowWatermark) {
    write_paused_ = false;
    if (on_drain) on_drain(shared_from_this());
  }
}

inline void Connection::log_error(const std::string&) {}

// --- Server implementation ---

inline Server::Server(uint16_t port, const std::string& bind_addr)
    : port_(port), bind_addr_(bind_addr) {
  server_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (server_sock_ < 0)
    EWSS_THROW(std::runtime_error("Failed to create socket"));

  int reuse = 1;
  setsockopt(server_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = bind_addr_.empty() ? htonl(INADDR_ANY) : inet_addr(bind_addr_.c_str());

  if (bind(server_sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    int err = errno;
    ::close(server_sock_);
    EWSS_THROW(std::runtime_error(
        "Failed to bind port " + std::to_string(port_) + ": " + strerror(err)));
  }

  if (listen(server_sock_, 128) < 0) {
    ::close(server_sock_);
    EWSS_THROW(std::runtime_error("Failed to listen"));
  }

  fcntl(server_sock_, F_SETFL, O_NONBLOCK);
  log_info("Server initialized on " + bind_addr_ + ":" + std::to_string(port_));
}

inline Server::~Server() {
  if (server_sock_ >= 0) ::close(server_sock_);
}

inline void Server::run() {
  is_running_ = true;
  stats_.reset();

  while (is_running_) {
    size_t nfds = 0;
    poll_fds_[nfds++] = {server_sock_, POLLIN, 0};

    for (uint32_t i = 0; i < connections_.size(); ++i) {
      short events = POLLIN;
      if (connections_[i]->has_data_to_send()) events |= POLLOUT;
      poll_fds_[nfds++] = {static_cast<int>(connections_[i]->get_fd()), events, 0};
    }

    auto poll_start = std::chrono::steady_clock::now();
    int ret = ::poll(poll_fds_.data(), static_cast<nfds_t>(nfds), poll_timeout_ms_);
    auto poll_end = std::chrono::steady_clock::now();

    uint64_t poll_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(poll_end - poll_start).count());
    stats_.last_poll_latency_us.store(poll_us, std::memory_order_relaxed);
    uint64_t prev_max = stats_.max_poll_latency_us.load(std::memory_order_relaxed);
    if (poll_us > prev_max)
      stats_.max_poll_latency_us.store(poll_us, std::memory_order_relaxed);

    if (ret < 0) break;
    if (ret == 0) continue;

    // Handle new connections (with overload protection)
    if (poll_fds_[0].revents & POLLIN) {
      if (stats_.is_overloaded(max_connections_)) {
        stats_.rejected_connections.fetch_add(1, std::memory_order_relaxed);
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int reject_sock = accept(server_sock_,
            reinterpret_cast<struct sockaddr*>(&client_addr), &client_addr_len);
        if (reject_sock >= 0) ::close(reject_sock);
      } else {
        accept_connection();
      }
    }

    // Handle client I/O
    for (size_t i = 1; i < nfds; ++i) {
      if (i - 1 >= connections_.size()) break;
      handle_connection_io(connections_[i - 1], poll_fds_[i]);
    }

    // Enforce timeouts
    for (uint32_t i = 0; i < connections_.size(); ++i) {
      auto& conn = connections_[i];
      if (conn->is_handshake_timed_out() || conn->is_close_timed_out())
        conn->close();
    }

    remove_closed_connections();
  }
}

inline expected<void, ErrorCode> Server::accept_connection() {
  if (connections_.size() >= max_connections_ || connections_.full()) {
    stats_.rejected_connections.fetch_add(1, std::memory_order_relaxed);
    return expected<void, ErrorCode>::error(ErrorCode::kMaxConnectionsExceeded);
  }

  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  int client_sock = accept(server_sock_,
      reinterpret_cast<struct sockaddr*>(&client_addr), &client_addr_len);

  if (client_sock < 0) {
    int err = errno;
    if (err != EAGAIN && err != EWOULDBLOCK) {
      stats_.socket_errors.fetch_add(1, std::memory_order_relaxed);
      return expected<void, ErrorCode>::error(ErrorCode::kSocketError);
    }
    return expected<void, ErrorCode>::success();
  }

  fcntl(client_sock, F_SETFL, O_NONBLOCK);
  apply_tcp_tuning(client_sock);

  auto conn = std::make_shared<Connection>(client_sock);
  conn->on_open = on_connect;
  conn->on_message = on_message;
  conn->on_close = on_close;
  conn->on_error = on_error;
  conn->on_backpressure = on_backpressure;
  conn->on_drain = on_drain;

  connections_.push_back(conn);
  stats_.total_connections.fetch_add(1, std::memory_order_relaxed);
  stats_.active_connections.fetch_add(1, std::memory_order_relaxed);
  return expected<void, ErrorCode>::success();
}

inline void Server::handle_connection_io(ConnPtr& conn, const pollfd& pfd) {
  if (pfd.revents & POLLIN) {
    if (!conn->handle_read().has_value()) conn->close();
  }
  if (pfd.revents & POLLOUT) {
    auto result = use_writev_ ? conn->handle_write_vectored() : conn->handle_write();
    if (!result.has_value()) conn->close();
  }
  if (pfd.revents & (POLLERR | POLLHUP)) conn->close();
}

inline void Server::remove_closed_connections() {
  uint32_t removed = 0;
  uint32_t i = 0;
  while (i < connections_.size()) {
    if (connections_[i]->is_closed()) {
      if (i < connections_.size() - 1)
        connections_[i] = static_cast<ConnPtr&&>(connections_[connections_.size() - 1]);
      connections_.pop_back();
      ++removed;
    } else {
      ++i;
    }
  }
  if (removed > 0)
    stats_.active_connections.fetch_sub(removed, std::memory_order_relaxed);
}

inline void Server::apply_tcp_tuning(int fd) {
  int opt = 1;
  if (tcp_tuning_.tcp_nodelay)
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef TCP_QUICKACK
  if (tcp_tuning_.tcp_quickack)
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt));
#endif
  if (tcp_tuning_.so_keepalive) {
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
#ifdef TCP_KEEPIDLE
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,
               &tcp_tuning_.keepalive_idle_s, sizeof(tcp_tuning_.keepalive_idle_s));
#endif
#ifdef TCP_KEEPINTVL
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL,
               &tcp_tuning_.keepalive_interval_s, sizeof(tcp_tuning_.keepalive_interval_s));
#endif
#ifdef TCP_KEEPCNT
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,
               &tcp_tuning_.keepalive_count, sizeof(tcp_tuning_.keepalive_count));
#endif
  }
}

inline void Server::log_info(const std::string& msg) {
  std::cout << "[EWSS INFO] " << msg << std::endl;
}

inline void Server::log_error(const std::string& msg) {
  std::cerr << "[EWSS ERROR] " << msg << std::endl;
}

}  // namespace ewss

#endif  // EWSS_HPP_
