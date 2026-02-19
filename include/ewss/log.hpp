/**
 * MIT License
 *
 * Copyright (c) 2026 liudegui
 *
 * Logging utilities for EWSS (loghelper-compatible interface).
 * Provides EWSS_LOG_INFO, EWSS_LOG_WARN, EWSS_LOG_ERROR macros.
 */

#ifndef EWSS_LOG_HPP_
#define EWSS_LOG_HPP_

#include <iostream>
#include <string>

namespace ewss {

// Simple logging implementation (will integrate loghelper later)
class Logger {
 public:
  enum class Level { kInfo, kWarn, kError, kDebug };

  static void log(Level level, const std::string& msg) {
    const char* prefix[] = {"[INFO]", "[WARN]", "[ERROR]", "[DEBUG]"};
    std::cerr << prefix[static_cast<int>(level)] << " " << msg << std::endl;
  }
};

#define EWSS_LOG_INFO(msg) ::ewss::Logger::log(::ewss::Logger::Level::kInfo, msg)
#define EWSS_LOG_WARN(msg) ::ewss::Logger::log(::ewss::Logger::Level::kWarn, msg)
#define EWSS_LOG_ERROR(msg) ::ewss::Logger::log(::ewss::Logger::Level::kError, msg)
#define EWSS_LOG_DEBUG(msg) ::ewss::Logger::log(::ewss::Logger::Level::kDebug, msg)

}  // namespace ewss

#endif  // EWSS_LOG_HPP_
