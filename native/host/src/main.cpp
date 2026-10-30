#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include "cef_app.h"

#include "app/HostApp.h"
#include "app/HostOptions.h"
#include "util/Log.h"

namespace
{
cef_log_severity_t MapLogSeverity(uxwv::LogLevel level)
{
	switch (level)
	{
	case uxwv::LogLevel::Error:
		return LOGSEVERITY_ERROR;
	case uxwv::LogLevel::Warning:
		return LOGSEVERITY_WARNING;
	case uxwv::LogLevel::Info:
		return LOGSEVERITY_INFO;
	case uxwv::LogLevel::Debug:
		return LOGSEVERITY_DEBUG;
	}
	return LOGSEVERITY_INFO;
}

bool StartsWith(const std::string& value, const char* prefix)
{
	return value.rfind(prefix, 0) == 0;
}

/** Subprocesses may start without --log; keep their diagnostics discoverable. */
std::string TempLogPath()
{
#if defined(_WIN32)
	char buffer[MAX_PATH] = {};
	if (GetTempPathA(MAX_PATH, buffer) == 0)
	{
		return std::string("UXWebviewHost");
	}
	return std::string(buffer) + "UXWebviewHost";
#else
	return std::string("UXWebviewHost");
#endif
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
		else if (arg == "--locale")
		{
			options.Locale = nextValue("--locale");
		}
		else if (arg == "--luid")
		{
			const std::string value = nextValue("--luid");
			options.AdapterLuid = strtoull(value.c_str(), nullptr, 0);
		}
		else if (arg == "--owner-pid")
		{
			options.OwnerPid = static_cast<uint32_t>(strtoul(nextValue("--owner-pid").c_str(), nullptr, 10));
		}
		else if (arg == "--devtools-port")
		{
			options.DevToolsPort = atoi(nextValue("--devtools-port").c_str());
		}
		else if (arg == "--idle-seconds")
		{
			options.IdleSeconds = atoi(nextValue("--idle-seconds").c_str());
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

	if (options.IpcName.empty())
	{
		UXWV_LOG_ERROR("--ipc is required");
		return 1;
	}
	if (options.OwnerPid == 0)
	{
		UXWV_LOG_WARN("no --owner-pid, the host cannot detect that the game exited");
	}
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
	// Overlay browsers live inside the game window, so the host must use the
	// same per-monitor DPI model as Unreal or the web UI gets bitmap scaled.
	::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

	// CEF subprocesses (renderer, gpu, utility) must exit through this path
	// before any other work happens. They inherit the parent command line, so
	// the log switches are honored here as well; each process writes its own
	// file because several processes cannot append to one file safely.
	std::string subprocessType;
	for (int i = 1; i < argc; ++i)
	{
		if (StartsWith(argv[i], "--type="))
		{
			subprocessType = std::string(argv[i] + 7);
		}
	}
	if (!subprocessType.empty())
	{
		// Subprocess diagnostics always run at debug level: they are low volume
		// and only written when a --log path or the temp fallback is available.
		uxwv::LogInit(TempLogPath() + "." + subprocessType + ".log", uxwv::LogLevel::Debug);
		UXWV_LOG_INFO("CEF subprocess '%s' starting", subprocessType.c_str());
	}

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
	if (!options.LogPath.empty())
	{
		CefString(&settings.log_file) = options.LogPath + ".cef.log";
	}
	if (!options.Locale.empty())
	{
		CefString(&settings.locale) = options.Locale;
	}
	CefString(&settings.root_cache_path) = options.CacheDir;
	CefString(&settings.cache_path) = options.CacheDir;

	UXWV_LOG_INFO("CefInitialize starting");
	if (!CefInitialize(mainArgs, settings, app, nullptr))
	{
		UXWV_LOG_ERROR("CefInitialize failed");
		uxwv::LogShutdown();
		return 1;
	}
	UXWV_LOG_INFO("CefInitialize done");

	UXWV_LOG_INFO("message loop starting");
	CefRunMessageLoop();

	app->StopInfrastructure();
	CefShutdown();
	UXWV_LOG_INFO("UXWebviewHost stopped");
	uxwv::LogShutdown();
	return 0;
}
