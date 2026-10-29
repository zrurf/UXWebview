#pragma once

#include <cstdint>
#include <string>

#include "util/Log.h"

namespace uxwv
{
struct HostOptions
{
	std::string IpcName;
	std::string SharedMemoryName;
	std::string Url{"about:blank"};
	std::string CacheDir;
	std::string LogPath;
	int Width = 1280;
	int Height = 720;
	int FrameRate = 60; /* 0 disables external begin frame */
	// Adapter LUID of the game process; 0 keeps the default adapter.
	uint64_t AdapterLuid = 0;
	// Game process id, used to duplicate shared texture handles.
	uint32_t OwnerPid = 0;
	bool bNoSandbox = false;
	LogLevel Level = LogLevel::Info;
};
} // namespace uxwv
