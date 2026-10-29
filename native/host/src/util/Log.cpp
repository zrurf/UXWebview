#include "util/Log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace uxwv
{
namespace
{
std::mutex gLogMutex;
FILE* gLogFile = nullptr;
LogLevel gLogLevel = LogLevel::Info;

const char* LevelName(LogLevel level)
{
	switch (level)
	{
	case LogLevel::Error: return "ERROR";
	case LogLevel::Warning: return "WARN";
	case LogLevel::Info: return "INFO";
	case LogLevel::Debug: return "DEBUG";
	default: return "INFO";
	}
}
} // namespace

void LogInit(const std::string& logPath, LogLevel level)
{
	std::lock_guard<std::mutex> Lock(gLogMutex);
	gLogLevel = level;
	if (!logPath.empty())
	{
		gLogFile = fopen(logPath.c_str(), "a");
		if (!gLogFile)
		{
			fprintf(stderr, "[UXWebviewHost] cannot open log file '%s'\n", logPath.c_str());
		}
	}
}

void LogShutdown()
{
	std::lock_guard<std::mutex> Lock(gLogMutex);
	if (gLogFile)
	{
		fclose(gLogFile);
		gLogFile = nullptr;
	}
}

void LogMessage(LogLevel level, const char* format, ...)
{
	if (static_cast<int>(level) > static_cast<int>(gLogLevel))
	{
		return;
	}

	char buffer[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	auto now = std::chrono::system_clock::now();
	std::time_t stamp = std::chrono::system_clock::to_time_t(now);
	std::tm local = *std::localtime(&stamp);
	int millis = static_cast<int>(
		std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);

	char line[1200];
	snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n",
		local.tm_hour, local.tm_min, local.tm_sec, millis, LevelName(level), buffer);

	std::lock_guard<std::mutex> Lock(gLogMutex);
	fputs(line, stderr);
	if (gLogFile)
	{
		fputs(line, gLogFile);
		fflush(gLogFile);
	}
}
} // namespace uxwv
