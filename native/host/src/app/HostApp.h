#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cef_app.h"
#include "cef_client.h"

#include "app/HostOptions.h"
#include "browser/BrowserManager.h"
#include "uxwebview_ipc.h"

namespace uxwv
{
class IpcServer;

// Hosts any number of off-screen browsers. IPC messages arrive on the pipe
// thread and are forwarded to the CEF UI thread; before CEF is initialized
// they are buffered so a fast plugin cannot lose the first commands.
class HostApp : public CefApp,
                public CefBrowserProcessHandler
{
public:
	explicit HostApp(HostOptions options);
	~HostApp();

	bool StartInfrastructure();
	void StopInfrastructure();

	// CefApp
	CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
	void OnBeforeCommandLineProcessing(const CefString& processType,
		CefRefPtr<CefCommandLine> commandLine) override;

	// CefBrowserProcessHandler
	void OnContextInitialized() override;

	// Internal entry points used by UI-thread tasks.
	void ProcessMessage(const UxIpcMessage& message, const std::vector<uint8_t>& payload);
	void CheckIdle();

private:
	// Runs on the pipe thread; buffers until the UI thread is available.
	void OnIpcMessage(const UxIpcMessage& message, const std::vector<uint8_t>& payload);
	void FlushPendingMessages();

	void HandleCreate(const UxIpcMessage& message, const std::vector<uint8_t>& payload);
	void HandleInput(const UxIpcMessage& message, const std::vector<uint8_t>& payload);

	void SendMessage(uint32_t type, uint32_t instance, uint32_t requestId,
		uint32_t a, uint32_t b, uint32_t flags, const std::string& payload);
	void SendEvent(uint32_t instance, uint32_t type, const std::string& json);

	void ScheduleIdleCheck();
	void RequestQuit();
	bool IsOwnerAlive();

	HostOptions Options;
	BrowserManager Manager;
	std::unique_ptr<IpcServer> Ipc;

	std::mutex PendingMutex;
	std::vector<std::pair<UxIpcMessage, std::vector<uint8_t>>> PendingMessages;
	bool bContextReady = false;
	bool bQuitting = false;
	double EmptySinceSeconds = 0.0;
	double LastIdleCheckSeconds = 0.0;

	IMPLEMENT_REFCOUNTING(HostApp);
};
} // namespace uxwv
