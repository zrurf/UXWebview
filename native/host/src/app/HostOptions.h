#pragma once

#include <cstdint>
#include <string>

#include "util/Log.h"

namespace uxwv
{
struct HostOptions
{
	std::string IpcName;
	std::string CacheDir;
	std::string LogPath;
	std::string Locale;
	// Adapter LUID of the game process; 0 keeps the default adapter.
	uint64_t AdapterLuid = 0;
	// Game process id, used to duplicate shared texture handles and to detect
	// that the owner went away.
	uint32_t OwnerPid = 0;
	int DevToolsPort = 0; /* 0 disables remote debugging */
	int IdleSeconds = 30; /* exit with no instances after this many seconds */
	bool bNoSandbox = false;
	LogLevel Level = LogLevel::Info;
};
} // namespace uxwv
