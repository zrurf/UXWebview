#include "app/HostApp.h"

#include <vector>

#include "cef_browser.h"
#include "cef_command_line.h"
#include "cef_task.h"

#include "browser/BrowserClient.h"
#include "ipc/IpcServer.h"
#include "util/Log.h"

namespace uxwv
{
namespace
{
class QuitTask : public CefTask
{
public:
	void Execute() override { CefQuitMessageLoop(); }

	IMPLEMENT_REFCOUNTING(QuitTask);
};

class BeginFrameTask : public CefTask
{
public:
	explicit BeginFrameTask(CefRefPtr<HostApp> owner) : Owner(owner) {}

	void Execute() override
	{
		if (Owner)
		{
			Owner->SendBeginFrame();
		}
	}

private:
	CefRefPtr<HostApp> Owner;

	IMPLEMENT_REFCOUNTING(BeginFrameTask);
};
} // namespace

HostApp::HostApp(HostOptions options) : Options(std::move(options))
{
}

HostApp::~HostApp()
{
	StopInfrastructure();
}

bool HostApp::StartInfrastructure()
{
	if (!Publisher.Start(Options.SharedMemoryName))
	{
		return false;
	}

	if (!Options.IpcName.empty())
	{
		Ipc = std::make_unique<IpcServer>(
			Options.IpcName,
			[this](const UxIpcMessage& message) { HandleIpcMessage(message); },
			[this]()
			{
				UXWV_LOG_WARN("IPC client disconnected");
				Shutdown();
			});

		if (!Ipc->Start())
		{
			return false;
		}
	}

	return true;
}

void HostApp::StopInfrastructure()
{
	if (Ipc)
	{
		Ipc->Stop();
		Ipc.reset();
	}
	Publisher.Stop();
}

void HostApp::OnBeforeCommandLineProcessing(const CefString& processType,
	CefRefPtr<CefCommandLine> commandLine)
{
	// Background throttling would starve the page when the game is busy.
	commandLine->AppendSwitch("disable-background-timer-throttling");
	commandLine->AppendSwitch("disable-renderer-backgrounding");
	commandLine->AppendSwitch("disable-backgrounding-occluded-windows");

	if (Options.FrameRate > 0)
	{
		commandLine->AppendSwitch("enable-begin-frame-control");
	}
}

void HostApp::OnContextInitialized()
{
	CreateBrowser();
}

void HostApp::CreateBrowser()
{
	Client = new BrowserClient(
		&Publisher, Options.Width, Options.Height,
		[this](CefRefPtr<CefBrowser> browser) { Browser = browser; });

	CefWindowInfo windowInfo;
	windowInfo.SetAsWindowless(nullptr);
	windowInfo.shared_texture_enabled = true;
	windowInfo.external_begin_frame_enabled = Options.FrameRate > 0;

	CefBrowserSettings browserSettings;
	browserSettings.windowless_frame_rate = Options.FrameRate > 0 ? Options.FrameRate : 60;

	UXWV_LOG_INFO("creating browser %dx%d url='%s' fps=%d",
		Options.Width, Options.Height, Options.Url.c_str(), Options.FrameRate);

	CefBrowserHost::CreateBrowser(
		windowInfo, Client, Options.Url, browserSettings, nullptr, nullptr);
}

void HostApp::SendBeginFrame()
{
	if (Browser && Browser->GetHost())
	{
		Browser->GetHost()->SendExternalBeginFrame();
	}
}

void HostApp::HandleIpcMessage(const UxIpcMessage& message)
{
	switch (message.Type)
	{
	case UXWV_IPC_HELLO:
	{
		UxIpcMessage reply{};
		reply.Type = UXWV_IPC_HELLO_ACK;
		reply.ProtocolVersion = UXWV_IPC_VERSION;
		reply.Value = UXWV_IPC_VERSION;
		reply.RequestId = message.RequestId;
		if (Ipc)
		{
			Ipc->Send(reply);
		}
		UXWV_LOG_INFO("IPC hello received");
		break;
	}
	case UXWV_IPC_CREATE:
	case UXWV_IPC_RESIZE:
		if (Client)
		{
			Client->SetViewSize(static_cast<int>(message.A), static_cast<int>(message.B));
		}
		if (Browser && Browser->GetHost())
		{
			Browser->GetHost()->WasResized();
		}
		break;
	case UXWV_IPC_BEGIN_FRAME:
		// Called from the IPC thread; forward to the UI thread.
		CefPostTask(TID_UI, new BeginFrameTask(this));
		break;
	case UXWV_IPC_SHUTDOWN:
		Shutdown();
		break;
	default:
		UXWV_LOG_WARN("unhandled IPC message type %u", message.Type);
		break;
	}
}

void HostApp::Shutdown()
{
	CefPostTask(TID_UI, new QuitTask());
}
} // namespace uxwv
