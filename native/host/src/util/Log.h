#pragma once

#include <string>

namespace uxwv
{
enum class LogLevel : int
{
	Error = 0,
	Warning = 1,
	Info = 2,
	Debug = 3
};

// Thread-safe, timestamped, English-only logging. Writes to stderr and to
// an optional log file. Never logs per-frame data.
void LogInit(const std::string& logPath, LogLevel level);
void LogShutdown();

void LogMessage(LogLevel level, const char* format, ...);

#define UXWV_LOG_ERROR(...) ::uxwv::LogMessage(::uxwv::LogLevel::Error, __VA_ARGS__)
#define UXWV_LOG_WARN(...) ::uxwv::LogMessage(::uxwv::LogLevel::Warning, __VA_ARGS__)
#define UXWV_LOG_INFO(...) ::uxwv::LogMessage(::uxwv::LogLevel::Info, __VA_ARGS__)
#define UXWV_LOG_DEBUG(...) ::uxwv::LogMessage(::uxwv::LogLevel::Debug, __VA_ARGS__)
} // namespace uxwv
