#pragma once

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
	bool bNoSandbox = false;
	LogLevel Level = LogLevel::Info;
};
} // namespace uxwv
