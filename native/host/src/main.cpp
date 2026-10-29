#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include "cef_app.h"
#include "cef_browser_process_handler.h"

#include "app/HostApp.h"
#include "app/HostOptions.h"
#include "util/Log.h"

namespace
{
bool StartsWith(const std::string& value, const char* prefix)
{
	return value.rfind(prefix, 0) == 0;
}

int ParseArgs(int argc, char** argv, uxwv::HostOptions& options)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		auto nextValue = [&](const char* name) -> std::string
		{
			if (i + 1 >= argc)
			{
				UXWV_LOG_ERROR("missing value for %s", name);
				return std::string();
			}
			return argv[++i];
		};

		if (arg == "--ipc")
		{
			options.IpcName = nextValue("--ipc");
		}
		else if (arg == "--shmem")
		{
			options.SharedMemoryName = nextValue("--shmem");
		}
		else if (arg == "--url")
		{
			options.Url = nextValue("--url");
		}
		else if (arg == "--width")
		{
			options.Width = atoi(nextValue("--width").c_str());
		}
		else if (arg == "--height")
		{
			options.Height = atoi(nextValue("--height").c_str());
		}
		else if (arg == "--fps")
		{
			options.FrameRate = atoi(nextValue("--fps").c_str());
		}
		else if (arg == "--cache")
		{
			options.CacheDir = nextValue("--cache");
		}
		else if (arg == "--log")
		{
			options.LogPath = nextValue("--log");
		}
		else if (arg == "--log-level")
		{
			options.Level = static_cast<uxwv::LogLevel>(atoi(nextValue("--log-level").c_str()));
		}
		else if (arg == "--no-sandbox")
		{
			options.bNoSandbox = true;
		}
		else if (StartsWith(arg, "--type="))
		{
			// CEF subprocess arguments are handled before regular parsing.
		}
		else
		{
			UXWV_LOG_WARN("unknown argument '%s'", arg.c_str());
		}
	}

	if (options.IpcName.empty() || options.SharedMemoryName.empty())
	{
		UXWV_LOG_ERROR("both --ipc and --shmem are required");
		return 1;
	}
	if (options.Width <= 0 || options.Height <= 0)
	{
		UXWV_LOG_ERROR("invalid view size %dx%d", options.Width, options.Height);
		return 1;
	}
	return 0;
}

cef_log_severity_t MapLogSeverity(uxwv::LogLevel level)
{
	switch (level)
	{
	case uxwv::LogLevel::Debug: return LOGSEVERITY_INFO;
	case uxwv::LogLevel::Info: return LOGSEVERITY_WARNING;
	default: return LOGSEVERITY_ERROR;
	}
}
} // namespace

int main(int argc, char** argv)
{
	// CEF subprocesses (renderer, gpu, utility) must exit through this path
	// before any other work happens.
#if defined(_WIN32)
	CefMainArgs mainArgs(GetModuleHandle(nullptr));
#else
	CefMainArgs mainArgs(argc, argv);
#endif
	CefRefPtr<uxwv::HostApp> probe = new uxwv::HostApp(uxwv::HostOptions{});
	int processCode = CefExecuteProcess(mainArgs, probe, nullptr);
	if (processCode >= 0)
	{
		return processCode;
	}

	uxwv::HostOptions options;
	int parseResult = ParseArgs(argc, argv, options);
	if (parseResult != 0)
	{
		return parseResult;
	}

	uxwv::LogInit(options.LogPath, options.Level);
	UXWV_LOG_INFO("UXWebviewHost starting");

	CefRefPtr<uxwv::HostApp> app = new uxwv::HostApp(options);
	if (!app->StartInfrastructure())
	{
		UXWV_LOG_ERROR("failed to start host infrastructure");
		uxwv::LogShutdown();
		return 1;
	}

	CefSettings settings;
	settings.windowless_rendering_enabled = true;
	settings.no_sandbox = options.bNoSandbox;
	settings.multi_threaded_message_loop = false;
	settings.log_severity = MapLogSeverity(options.Level);
	settings.external_message_pump = false;
	CefString(&settings.root_cache_path) = options.CacheDir;
	CefString(&settings.cache_path) = options.CacheDir;

	if (!CefInitialize(mainArgs, settings, app, nullptr))
	{
		UXWV_LOG_ERROR("CefInitialize failed");
		uxwv::LogShutdown();
		return 1;
	}

	CefRunMessageLoop();

	app->StopInfrastructure();
	CefShutdown();
	UXWV_LOG_INFO("UXWebviewHost stopped");
	uxwv::LogShutdown();
	return 0;
}
