#include "ewss/vocabulary.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ewss;

// ============================================================================
// expected<V, E>
// ============================================================================

TEST_CASE("expected - success with value", "[vocabulary]") {
  auto result = expected<int, ErrorCode>::success(42);
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 42);
}

TEST_CASE("expected - error", "[vocabulary]") {
  auto result = expected<int, ErrorCode>::error(ErrorCode::kBufferFull);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == ErrorCode::kBufferFull);
}

TEST_CASE("expected - bool conversion", "[vocabulary]") {
  auto ok = expected<int, ErrorCode>::success(1);
  auto err = expected<int, ErrorCode>::error(ErrorCode::kSocketError);
  REQUIRE(static_cast<bool>(ok) == true);
  REQUIRE(static_cast<bool>(err) == false);
}

TEST_CASE("expected - value_or", "[vocabulary]") {
  auto ok = expected<int, ErrorCode>::success(10);
  auto err = expected<int, ErrorCode>::error(ErrorCode::kTimeout);
  REQUIRE(ok.value_or(99) == 10);
  REQUIRE(err.value_or(99) == 99);
}

TEST_CASE("expected - copy", "[vocabulary]") {
  auto original = expected<int, ErrorCode>::success(7);
  auto copy = original;
  REQUIRE(copy.has_value());
  REQUIRE(copy.value() == 7);
}

TEST_CASE("expected - move", "[vocabulary]") {
  auto original = expected<int, ErrorCode>::success(7);
  auto moved = static_cast<expected<int, ErrorCode>&&>(original);
  REQUIRE(moved.has_value());
  REQUIRE(moved.value() == 7);
}

TEST_CASE("expected<void> - success", "[vocabulary]") {
  auto result = expected<void, ErrorCode>::success();
  REQUIRE(result.has_value());
}

TEST_CASE("expected<void> - error", "[vocabulary]") {
  auto result = expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == ErrorCode::kHandshakeFailed);
}

TEST_CASE("expected - all error codes", "[vocabulary]") {
  auto e1 = expected<void, ErrorCode>::error(ErrorCode::kOk);
  auto e2 = expected<void, ErrorCode>::error(ErrorCode::kBufferFull);
  auto e3 = expected<void, ErrorCode>::error(ErrorCode::kBufferEmpty);
  auto e4 = expected<void, ErrorCode>::error(ErrorCode::kHandshakeFailed);
  auto e5 = expected<void, ErrorCode>::error(ErrorCode::kFrameParseError);
  auto e6 = expected<void, ErrorCode>::error(ErrorCode::kConnectionClosed);
  auto e7 = expected<void, ErrorCode>::error(ErrorCode::kInvalidState);
  auto e8 = expected<void, ErrorCode>::error(ErrorCode::kSocketError);
  auto e9 = expected<void, ErrorCode>::error(ErrorCode::kTimeout);
  auto e10 = expected<void, ErrorCode>::error(ErrorCode::kMaxConnectionsExceeded);
  auto e11 = expected<void, ErrorCode>::error(ErrorCode::kInternalError);

  REQUIRE(e1.get_error() == ErrorCode::kOk);
  REQUIRE(e2.get_error() == ErrorCode::kBufferFull);
  REQUIRE(e3.get_error() == ErrorCode::kBufferEmpty);
  REQUIRE(e4.get_error() == ErrorCode::kHandshakeFailed);
  REQUIRE(e5.get_error() == ErrorCode::kFrameParseError);
  REQUIRE(e6.get_error() == ErrorCode::kConnectionClosed);
  REQUIRE(e7.get_error() == ErrorCode::kInvalidState);
  REQUIRE(e8.get_error() == ErrorCode::kSocketError);
  REQUIRE(e9.get_error() == ErrorCode::kTimeout);
  REQUIRE(e10.get_error() == ErrorCode::kMaxConnectionsExceeded);
  REQUIRE(e11.get_error() == ErrorCode::kInternalError);
}

// ============================================================================
// optional<T>
// ============================================================================

TEST_CASE("optional - empty", "[vocabulary]") {
  optional<int> opt;
  REQUIRE(!opt.has_value());
}

TEST_CASE("optional - with value", "[vocabulary]") {
  optional<int> opt(42);
  REQUIRE(opt.has_value());
  REQUIRE(opt.value() == 42);
}

TEST_CASE("optional - value_or", "[vocabulary]") {
  optional<int> empty;
  optional<int> full(10);
  REQUIRE(empty.value_or(99) == 99);
  REQUIRE(full.value_or(99) == 10);
}

TEST_CASE("optional - reset", "[vocabulary]") {
  optional<int> opt(5);
  REQUIRE(opt.has_value());
  opt.reset();
  REQUIRE(!opt.has_value());
}

TEST_CASE("optional - copy", "[vocabulary]") {
  optional<int> a(7);
  optional<int> b = a;
  REQUIRE(b.has_value());
  REQUIRE(b.value() == 7);
}

TEST_CASE("optional - move", "[vocabulary]") {
  optional<int> a(7);
  optional<int> b = static_cast<optional<int>&&>(a);
  REQUIRE(b.has_value());
  REQUIRE(b.value() == 7);
}

TEST_CASE("optional - bool conversion", "[vocabulary]") {
  optional<int> empty;
  optional<int> full(1);
  REQUIRE(static_cast<bool>(empty) == false);
  REQUIRE(static_cast<bool>(full) == true);
}

// ============================================================================
// FixedString<N>
// ============================================================================

TEST_CASE("FixedString - default empty", "[vocabulary]") {
  FixedString<32> s;
  REQUIRE(s.empty());
  REQUIRE(s.size() == 0);
  REQUIRE(s.capacity() == 32);
}

TEST_CASE("FixedString - from literal", "[vocabulary]") {
  FixedString<32> s("hello");
  REQUIRE(s.size() == 5);
  REQUIRE(std::string(s.c_str()) == "hello");
}

TEST_CASE("FixedString - clear", "[vocabulary]") {
  FixedString<16> s("test");
  REQUIRE(!s.empty());
  s.clear();
  REQUIRE(s.empty());
  REQUIRE(s.size() == 0);
}

// ============================================================================
// FixedVector<T, N>
// ============================================================================

TEST_CASE("FixedVector - initial empty", "[vocabulary]") {
  FixedVector<int, 8> v;
  REQUIRE(v.empty());
  REQUIRE(v.size() == 0);
  REQUIRE(v.capacity() == 8);
}

TEST_CASE("FixedVector - push_back", "[vocabulary]") {
  FixedVector<int, 4> v;
  REQUIRE(v.push_back(10));
  REQUIRE(v.push_back(20));
  REQUIRE(v.push_back(30));
  REQUIRE(v.size() == 3);
  REQUIRE(v[0] == 10);
  REQUIRE(v[1] == 20);
  REQUIRE(v[2] == 30);
}

TEST_CASE("FixedVector - full", "[vocabulary]") {
  FixedVector<int, 2> v;
  REQUIRE(v.push_back(1));
  REQUIRE(v.push_back(2));
  REQUIRE(v.full());
  REQUIRE(!v.push_back(3));  // Should fail
}

TEST_CASE("FixedVector - pop_back", "[vocabulary]") {
  FixedVector<int, 4> v;
  v.push_back(1);
  v.push_back(2);
  REQUIRE(v.pop_back());
  REQUIRE(v.size() == 1);
  REQUIRE(v[0] == 1);
}

TEST_CASE("FixedVector - pop_back empty", "[vocabulary]") {
  FixedVector<int, 4> v;
  REQUIRE(!v.pop_back());
}

TEST_CASE("FixedVector - clear", "[vocabulary]") {
  FixedVector<int, 8> v;
  v.push_back(1);
  v.push_back(2);
  v.push_back(3);
  v.clear();
  REQUIRE(v.empty());
  REQUIRE(v.size() == 0);
}

TEST_CASE("FixedVector - front and back", "[vocabulary]") {
  FixedVector<int, 4> v;
  v.push_back(10);
  v.push_back(20);
  v.push_back(30);
  REQUIRE(v.front() == 10);
  REQUIRE(v.back() == 30);
}

TEST_CASE("FixedVector - iterator", "[vocabulary]") {
  FixedVector<int, 4> v;
  v.push_back(1);
  v.push_back(2);
  v.push_back(3);

  int sum = 0;
  for (auto it = v.begin(); it != v.end(); ++it) {
    sum += *it;
  }
  REQUIRE(sum == 6);
}

TEST_CASE("FixedVector - copy", "[vocabulary]") {
  FixedVector<int, 4> a;
  a.push_back(1);
  a.push_back(2);
  FixedVector<int, 4> b = a;
  REQUIRE(b.size() == 2);
  REQUIRE(b[0] == 1);
  REQUIRE(b[1] == 2);
}

TEST_CASE("FixedVector - move", "[vocabulary]") {
  FixedVector<int, 4> a;
  a.push_back(1);
  a.push_back(2);
  FixedVector<int, 4> b = static_cast<FixedVector<int, 4>&&>(a);
  REQUIRE(b.size() == 2);
  REQUIRE(a.empty());
}

// ============================================================================
// FixedFunction
// ============================================================================

TEST_CASE("FixedFunction - empty", "[vocabulary]") {
  FixedFunction<void()> fn;
  REQUIRE(!static_cast<bool>(fn));
}

TEST_CASE("FixedFunction - nullptr", "[vocabulary]") {
  FixedFunction<void()> fn(nullptr);
  REQUIRE(!static_cast<bool>(fn));
}

TEST_CASE("FixedFunction - lambda", "[vocabulary]") {
  int called = 0;
  FixedFunction<void()> fn([&called]() { ++called; });
  REQUIRE(static_cast<bool>(fn));
  fn();
  REQUIRE(called == 1);
}

TEST_CASE("FixedFunction - with return value", "[vocabulary]") {
  FixedFunction<int(int, int)> fn([](int a, int b) { return a + b; });
  REQUIRE(fn(3, 4) == 7);
}

TEST_CASE("FixedFunction - move", "[vocabulary]") {
  int called = 0;
  FixedFunction<void()> fn1([&called]() { ++called; });
  FixedFunction<void()> fn2 = static_cast<FixedFunction<void()>&&>(fn1);
  REQUIRE(!static_cast<bool>(fn1));
  REQUIRE(static_cast<bool>(fn2));
  fn2();
  REQUIRE(called == 1);
}

TEST_CASE("FixedFunction - assign nullptr", "[vocabulary]") {
  int called = 0;
  FixedFunction<void()> fn([&called]() { ++called; });
  fn = nullptr;
  REQUIRE(!static_cast<bool>(fn));
}

// ============================================================================
// function_ref
// ============================================================================

TEST_CASE("function_ref - lambda", "[vocabulary]") {
  int value = 0;
  auto lambda = [&value]() { value = 42; };
  function_ref<void()> ref(lambda);
  ref();
  REQUIRE(value == 42);
}

TEST_CASE("function_ref - function pointer", "[vocabulary]") {
  auto fn = [](int x) -> int { return x * 2; };
  function_ref<int(int)> ref(fn);
  REQUIRE(ref(5) == 10);
}

// ============================================================================
// ScopeGuard
// ============================================================================

TEST_CASE("ScopeGuard - executes on scope exit", "[vocabulary]") {
  int value = 0;
  {
    ScopeGuard guard([&value]() { value = 1; });
  }
  REQUIRE(value == 1);
}

TEST_CASE("ScopeGuard - release prevents execution", "[vocabulary]") {
  int value = 0;
  {
    ScopeGuard guard([&value]() { value = 1; });
    guard.release();
  }
  REQUIRE(value == 0);
}

TEST_CASE("ScopeGuard - move", "[vocabulary]") {
  int value = 0;
  {
    ScopeGuard guard1([&value]() { value = 1; });
    ScopeGuard guard2 = static_cast<ScopeGuard&&>(guard1);
    // guard1 should be released, guard2 should fire
  }
  REQUIRE(value == 1);
}

// ============================================================================
// kCacheLine
// ============================================================================

TEST_CASE("kCacheLine constant", "[vocabulary]") {
  REQUIRE(kCacheLine == 64);
}
