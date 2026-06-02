// XTE-DCDU: logging helpers
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>
#include <string>

#include "XPLMUtilities.h"

namespace xtedcdu {

enum class LogLevel { Quiet = 0, Normal = 1, Verbose = 2 };

inline LogLevel& current_log_level() {
    static LogLevel lvl = LogLevel::Normal;
    return lvl;
}

inline void log_raw(const char* msg) {
    XPLMDebugString(msg);
}

inline void logf(LogLevel min_level, const char* fmt, ...) {
    if (static_cast<int>(current_log_level()) < static_cast<int>(min_level)) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[1100];
    std::snprintf(line, sizeof(line), "XTE-DCDU: %s\n", buf);
    XPLMDebugString(line);
}

#define XTED_LOG(...)   ::xtedcdu::logf(::xtedcdu::LogLevel::Normal,  __VA_ARGS__)
#define XTED_VLOG(...)  ::xtedcdu::logf(::xtedcdu::LogLevel::Verbose, __VA_ARGS__)
#define XTED_ERR(...)   ::xtedcdu::logf(::xtedcdu::LogLevel::Quiet,   __VA_ARGS__)

} // namespace xtedcdu
