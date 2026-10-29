#pragma once

#include <memory>

#include "cef_app.h"
#include "cef_client.h"

#include "app/HostOptions.h"
#include "frame/FramePublisher.h"
#include "uxwebview_ipc.h"

namespace uxwv
{
class BrowserClient;
class IpcServer;

// Spike implementation: one browser, one client, one frame publisher.
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

	void SendBeginFrame();

private:
	void CreateBrowser();
	void HandleIpcMessage(const UxIpcMessage& message);
	void Shutdown();

	HostOptions Options;
	FramePublisher Publisher;
	std::unique_ptr<IpcServer> Ipc;
	CefRefPtr<BrowserClient> Client;
	CefRefPtr<CefBrowser> Browser;

	IMPLEMENT_REFCOUNTING(HostApp);
};
} // namespace uxwv
