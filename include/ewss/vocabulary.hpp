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
 * @file vocabulary.hpp
 * @brief Vocabulary types for EWSS: expected, optional, FixedVector, FixedString,
 *        FixedFunction, function_ref, ScopeGuard.
 *
 * Derived from newosp vocabulary (iceoryx inspired).
 * All types are stack-allocated with zero heap overhead.
 */

#ifndef EWSS_VOCABULARY_HPP_
#define EWSS_VOCABULARY_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

// Assertion macro for embedded systems (no-op in release)
#ifndef EWSS_ASSERT
#define EWSS_ASSERT(cond) ((void)(cond))
#endif

namespace ewss {

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

/**
 * @brief Holds either a success value of type V or an error of type E.
 *
 * Use static factory methods success() and error() to construct.
 */
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

  expected(const expected& other) noexcept : storage_{}, err_(other.err_),
                                             has_value_(other.has_value_) {
    if (has_value_) {
      ::new (&storage_) V(other.value());
    }
  }

  expected& operator=(const expected& other) noexcept {
    if (this != &other) {
      if (has_value_) {
        reinterpret_cast<V*>(&storage_)->~V();
      }
      has_value_ = other.has_value_;
      err_ = other.err_;
      if (has_value_) {
        ::new (&storage_) V(other.value());
      }
    }
    return *this;
  }

  expected(expected&& other) noexcept : storage_{}, err_(other.err_),
                                        has_value_(other.has_value_) {
    if (has_value_) {
      ::new (&storage_) V(static_cast<V&&>(other.value()));
    }
  }

  expected& operator=(expected&& other) noexcept {
    if (this != &other) {
      if (has_value_) {
        reinterpret_cast<V*>(&storage_)->~V();
      }
      has_value_ = other.has_value_;
      err_ = other.err_;
      if (has_value_) {
        ::new (&storage_) V(static_cast<V&&>(other.value()));
      }
    }
    return *this;
  }

  ~expected() {
    if (has_value_) {
      reinterpret_cast<V*>(&storage_)->~V();
    }
  }

  [[nodiscard]] bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }

  V& value() & noexcept {
    EWSS_ASSERT(has_value_);
    return *reinterpret_cast<V*>(&storage_);
  }

  const V& value() const& noexcept {
    EWSS_ASSERT(has_value_);
    return *reinterpret_cast<const V*>(&storage_);
  }

  E get_error() const noexcept {
    EWSS_ASSERT(!has_value_);
    return err_;
  }

  V value_or(const V& default_val) const noexcept {
    return has_value_ ? value() : default_val;
  }

 private:
  expected() noexcept : storage_{}, err_{}, has_value_(false) {}

  typename std::aligned_storage<sizeof(V), alignof(V)>::type storage_{};
  E err_{};
  bool has_value_{false};
};

/**
 * @brief Void specialization - represents success or error with no value.
 */
template <typename E>
class expected<void, E> final {
 public:
  static expected success() noexcept {
    expected e;
    e.has_value_ = true;
    return e;
  }

  static expected error(E err) noexcept {
    expected e;
    e.has_value_ = false;
    e.err_ = err;
    return e;
  }

  [[nodiscard]] bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }

  E get_error() const noexcept {
    EWSS_ASSERT(!has_value_);
    return err_;
  }

 private:
  expected() noexcept : err_{}, has_value_(false) {}

  E err_{};
  bool has_value_{false};
};

// ============================================================================
// optional<T> - Lightweight nullable value
// ============================================================================

/**
 * @brief Holds either a value of type T or nothing.
 */
template <typename T>
class optional final {
 public:
  optional() noexcept : has_value_(false) {}

  optional(const T& val) noexcept : has_value_(true) {  // NOLINT
    ::new (&storage_) T(val);
  }

  optional(T&& val) noexcept : has_value_(true) {  // NOLINT
    ::new (&storage_) T(static_cast<T&&>(val));
  }

  optional(const optional& other) noexcept : has_value_(other.has_value_) {
    if (has_value_) {
      ::new (&storage_) T(other.value());
    }
  }

  optional& operator=(const optional& other) noexcept {
    if (this != &other) {
      reset();
      has_value_ = other.has_value_;
      if (has_value_) {
        ::new (&storage_) T(other.value());
      }
    }
    return *this;
  }

  optional(optional&& other) noexcept : has_value_(other.has_value_) {
    if (has_value_) {
      ::new (&storage_) T(static_cast<T&&>(other.value()));
    }
  }

  optional& operator=(optional&& other) noexcept {
    if (this != &other) {
      reset();
      has_value_ = other.has_value_;
      if (has_value_) {
        ::new (&storage_) T(static_cast<T&&>(other.value()));
      }
    }
    return *this;
  }

  ~optional() { reset(); }

  [[nodiscard]] bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }

  T& value() noexcept {
    EWSS_ASSERT(has_value_);
    return *reinterpret_cast<T*>(&storage_);
  }

  const T& value() const noexcept {
    EWSS_ASSERT(has_value_);
    return *reinterpret_cast<const T*>(&storage_);
  }

  T value_or(const T& default_val) const noexcept {
    return has_value_ ? value() : default_val;
  }

  void reset() noexcept {
    if (has_value_) {
      reinterpret_cast<T*>(&storage_)->~T();
      has_value_ = false;
    }
  }

 private:
  typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
  bool has_value_;
};

// ============================================================================
// FixedString<Capacity> - Stack-allocated fixed-capacity string
// ============================================================================

/**
 * @brief Fixed-capacity, stack-allocated, null-terminated string.
 *
 * @tparam Capacity Maximum number of characters (excluding null terminator)
 */
template <uint32_t Capacity>
class FixedString {
  static_assert(Capacity > 0U, "FixedString capacity must be > 0");

 public:
  constexpr FixedString() noexcept : buf_{'\0'}, size_(0U) {}

  template <uint32_t N,
            typename = typename std::enable_if<(N <= Capacity + 1U)>::type>
  FixedString(const char (&str)[N]) noexcept : size_(N - 1U) {  // NOLINT
    static_assert(N > 0U, "String literal must include null terminator");
    static_assert(N - 1U <= Capacity, "String literal exceeds capacity");
    std::memcpy(buf_, str, N);
  }

  [[nodiscard]] constexpr const char* c_str() const noexcept { return buf_; }
  [[nodiscard]] constexpr uint32_t size() const noexcept { return size_; }
  static constexpr uint32_t capacity() noexcept { return Capacity; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0U; }

  void clear() noexcept {
    size_ = 0U;
    buf_[0] = '\0';
  }

 private:
  char buf_[Capacity + 1U];
  uint32_t size_;
};

// ============================================================================
// FixedVector<T, Capacity> - Stack-allocated fixed-capacity vector
// ============================================================================

/**
 * @brief Fixed-capacity, stack-allocated vector with no heap allocation.
 *
 * @tparam T Element type
 * @tparam Capacity Maximum number of elements
 */
template <typename T, uint32_t Capacity>
class FixedVector final {
  static_assert(Capacity > 0U, "FixedVector capacity must be > 0");

 public:
  using value_type = T;
  using size_type = uint32_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using iterator = T*;
  using const_iterator = const T*;

  FixedVector() noexcept {}  // NOLINT

  ~FixedVector() noexcept { clear(); }

  FixedVector(const FixedVector& other) noexcept {  // NOLINT
    for (uint32_t i = 0U; i < other.size_; ++i) {
      (void)push_back(other[i]);
    }
  }

  FixedVector& operator=(const FixedVector& other) noexcept {
    if (this != &other) {
      clear();
      for (uint32_t i = 0U; i < other.size_; ++i) {
        (void)push_back(other[i]);
      }
    }
    return *this;
  }

  FixedVector(FixedVector&& other) noexcept {  // NOLINT
    for (uint32_t i = 0U; i < other.size_; ++i) {
      (void)emplace_back(static_cast<T&&>(other[i]));
    }
    other.clear();
  }

  FixedVector& operator=(FixedVector&& other) noexcept {
    if (this != &other) {
      clear();
      for (uint32_t i = 0U; i < other.size_; ++i) {
        (void)emplace_back(static_cast<T&&>(other[i]));
      }
      other.clear();
    }
    return *this;
  }

  reference operator[](uint32_t index) noexcept {
    return *reinterpret_cast<T*>(storage_ + index * sizeof(T));
  }

  const_reference operator[](uint32_t index) const noexcept {
    return *reinterpret_cast<const T*>(storage_ + index * sizeof(T));
  }

  reference front() noexcept { return (*this)[0U]; }
  const_reference front() const noexcept { return (*this)[0U]; }
  reference back() noexcept { return (*this)[size_ - 1U]; }
  const_reference back() const noexcept { return (*this)[size_ - 1U]; }

  pointer data() noexcept { return reinterpret_cast<T*>(storage_); }
  const_pointer data() const noexcept {
    return reinterpret_cast<const T*>(storage_);
  }

  iterator begin() noexcept { return data(); }
  const_iterator begin() const noexcept { return data(); }
  iterator end() noexcept { return data() + size_; }
  const_iterator end() const noexcept { return data() + size_; }

  [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
  [[nodiscard]] uint32_t size() const noexcept { return size_; }
  static constexpr uint32_t capacity() noexcept { return Capacity; }
  [[nodiscard]] bool full() const noexcept { return size_ >= Capacity; }

  bool push_back(const T& value) noexcept { return emplace_back(value); }
  bool push_back(T&& value) noexcept { return emplace_back(static_cast<T&&>(value)); }

  template <typename... CtorArgs>
  bool emplace_back(CtorArgs&&... args) noexcept {
    if (size_ >= Capacity) {
      return false;
    }
    ::new (storage_ + size_ * sizeof(T))
        T{static_cast<CtorArgs&&>(args)...};
    ++size_;
    return true;
  }

  bool pop_back() noexcept {
    if (size_ == 0U) {
      return false;
    }
    --size_;
    (*this)[size_].~T();
    return true;
  }

  void clear() noexcept {
    while (size_ > 0U) {
      --size_;
      (*this)[size_].~T();
    }
  }

 private:
  alignas(T) uint8_t storage_[sizeof(T) * Capacity];
  uint32_t size_{0U};
};

// ============================================================================
// FixedFunction<Sig, BufferSize> - SBO callback
// ============================================================================

template <typename Signature, size_t BufferSize = 2 * sizeof(void*)>
class FixedFunction;

/**
 * @brief Fixed-size callable wrapper with small buffer optimization.
 */
template <typename Ret, typename... Args, size_t BufferSize>
class FixedFunction<Ret(Args...), BufferSize> final {
 public:
  FixedFunction() noexcept = default;

  // NOLINTNEXTLINE(google-explicit-constructor)
  FixedFunction(std::nullptr_t) noexcept {}

  template <typename F, typename = typename std::enable_if<
                            !std::is_same<typename std::decay<F>::type,
                                          FixedFunction>::value&&
                                !std::is_same<typename std::decay<F>::type,
                                              std::nullptr_t>::value>::type>
  FixedFunction(F&& f) noexcept {  // NOLINT
    using Decay = typename std::decay<F>::type;
    static_assert(sizeof(Decay) <= BufferSize,
                  "Callable too large for FixedFunction buffer");
    static_assert(alignof(Decay) <= alignof(Storage),
                  "Callable alignment exceeds buffer alignment");
    ::new (&storage_) Decay(static_cast<F&&>(f));
    invoker_ = [](const Storage& s, Args... args) -> Ret {
      return (*reinterpret_cast<const Decay*>(&s))(
          static_cast<Args&&>(args)...);
    };
    destroyer_ = [](Storage& s) {
      reinterpret_cast<Decay*>(&s)->~Decay();
    };
  }

  FixedFunction(FixedFunction&& other) noexcept
      : invoker_(other.invoker_), destroyer_(other.destroyer_) {
    if (other.invoker_) {
      std::memcpy(&storage_, &other.storage_, BufferSize);
      other.invoker_ = nullptr;
      other.destroyer_ = nullptr;
    }
  }

  FixedFunction& operator=(FixedFunction&& other) noexcept {
    if (this != &other) {
      if (destroyer_) {
        destroyer_(storage_);
      }
      invoker_ = other.invoker_;
      destroyer_ = other.destroyer_;
      if (other.invoker_) {
        std::memcpy(&storage_, &other.storage_, BufferSize);
        other.invoker_ = nullptr;
        other.destroyer_ = nullptr;
      }
    }
    return *this;
  }

  FixedFunction& operator=(std::nullptr_t) noexcept {
    if (destroyer_) {
      destroyer_(storage_);
    }
    invoker_ = nullptr;
    destroyer_ = nullptr;
    return *this;
  }

  ~FixedFunction() {
    if (destroyer_) {
      destroyer_(storage_);
    }
  }

  FixedFunction(const FixedFunction&) = delete;
  FixedFunction& operator=(const FixedFunction&) = delete;

  Ret operator()(Args... args) const {
    EWSS_ASSERT(invoker_);
    return invoker_(storage_, static_cast<Args&&>(args)...);
  }

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
// function_ref<Sig> - Non-owning callable reference
// ============================================================================

template <typename Sig>
class function_ref;

/**
 * @brief Non-owning lightweight callable reference (2 pointers).
 *
 * The referenced callable must outlive the function_ref.
 */
template <typename Ret, typename... Args>
class function_ref<Ret(Args...)> final {
 public:
  template <typename F,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<F>::type, function_ref>::value>::type>
  function_ref(F&& f) noexcept  // NOLINT
      : obj_(const_cast<void*>(static_cast<const void*>(&f))),
        invoker_([](void* o, Args... args) -> Ret {
          return (*static_cast<typename std::remove_reference<F>::type*>(o))(
              static_cast<Args&&>(args)...);
        }) {}

  function_ref(Ret (*fn)(Args...)) noexcept  // NOLINT
      : obj_(reinterpret_cast<void*>(fn)),
        invoker_([](void* o, Args... args) -> Ret {
          return reinterpret_cast<Ret (*)(Args...)>(o)(
              static_cast<Args&&>(args)...);
        }) {}

  Ret operator()(Args... args) const {
    return invoker_(obj_, static_cast<Args&&>(args)...);
  }

 private:
  void* obj_;
  Ret (*invoker_)(void*, Args...);
};

// ============================================================================
// ScopeGuard - RAII cleanup guard
// ============================================================================

/**
 * @brief Executes a cleanup callback on scope exit unless released.
 */
class ScopeGuard final {
 public:
  explicit ScopeGuard(FixedFunction<void()> cleanup) noexcept
      : cleanup_(static_cast<FixedFunction<void()>&&>(cleanup)) {}

  ~ScopeGuard() {
    if (active_ && cleanup_) {
      cleanup_();
    }
  }

  void release() noexcept { active_ = false; }

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

  ScopeGuard(ScopeGuard&& other) noexcept
      : cleanup_(static_cast<FixedFunction<void()>&&>(other.cleanup_)),
        active_(other.active_) {
    other.active_ = false;
  }

  ScopeGuard& operator=(ScopeGuard&&) = delete;

 private:
  FixedFunction<void()> cleanup_;
  bool active_{true};
};

}  // namespace ewss

#endif  // EWSS_VOCABULARY_HPP_
