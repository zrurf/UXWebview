#include "app/HostApp.h"

#include <chrono>
#include <cstring>

#include "cef_frame.h"
#include "cef_parser.h"
#include "cef_task.h"
#include "cef_values.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

#include "browser/BridgeScript.h"
#include "ipc/IpcServer.h"
#include "util/Log.h"

namespace uxwv
{
namespace
{
double NowSeconds()
{
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

class IpcTask : public CefTask
{
public:
	IpcTask(CefRefPtr<HostApp> owner, const UxIpcMessage& message,
		const std::vector<uint8_t>& payload)
		: Owner(owner), Message(message), Payload(payload)
	{
	}

	void Execute() override
	{
		UXWV_LOG_DEBUG("IPC task executing type %u", Message.Type);
		if (Owner)
		{
			Owner->ProcessMessage(Message, Payload);
		}
	}

private:
	CefRefPtr<HostApp> Owner;
	UxIpcMessage Message;
	std::vector<uint8_t> Payload;

	IMPLEMENT_REFCOUNTING(IpcTask);
};

class QuitTask : public CefTask
{
public:
	void Execute() override { CefQuitMessageLoop(); }

	IMPLEMENT_REFCOUNTING(QuitTask);
};

class IdleTask : public CefTask
{
public:
	explicit IdleTask(CefRefPtr<HostApp> owner) : Owner(owner) {}

	void Execute() override
	{
		if (Owner)
		{
			Owner->CheckIdle();
		}
	}

private:
	CefRefPtr<HostApp> Owner;

	IMPLEMENT_REFCOUNTING(IdleTask);
};

CefRefPtr<CefDictionaryValue> ParsePayload(const std::vector<uint8_t>& payload)
{
	if (payload.empty())
	{
		return nullptr;
	}

	CefRefPtr<CefValue> root =
		CefParseJSON(payload.data(), payload.size(), JSON_PARSER_RFC);
	if (!root || root->GetType() != VTYPE_DICTIONARY)
	{
		return nullptr;
	}
	return root->GetDictionary();
}

std::string GetString(const CefRefPtr<CefDictionaryValue>& dict, const char* key)
{
	if (!dict || !dict->HasKey(key))
	{
		return std::string();
	}
	return dict->GetString(key).ToString();
}

int GetInt(const CefRefPtr<CefDictionaryValue>& dict, const char* key, int fallback)
{
	if (!dict || !dict->HasKey(key))
	{
		return fallback;
	}
	return dict->GetInt(key);
}

float GetFloat(const CefRefPtr<CefDictionaryValue>& dict, const char* key, float fallback)
{
	if (!dict || !dict->HasKey(key))
	{
		return fallback;
	}
	return static_cast<float>(dict->GetDouble(key));
}

bool GetBool(const CefRefPtr<CefDictionaryValue>& dict, const char* key, bool fallback)
{
	if (!dict || !dict->HasKey(key))
	{
		return fallback;
	}
	return dict->GetBool(key);
}

std::vector<std::string> GetStringList(const CefRefPtr<CefDictionaryValue>& dict, const char* key)
{
	std::vector<std::string> values;
	if (!dict || !dict->HasKey(key) || dict->GetType(key) != VTYPE_LIST)
	{
		return values;
	}

	CefRefPtr<CefListValue> list = dict->GetList(key);
	for (size_t index = 0; index < list->GetSize(); ++index)
	{
		if (list->GetType(index) == VTYPE_STRING)
		{
			values.push_back(list->GetString(index).ToString());
		}
	}
	return values;
}

bool ParseInput(const std::vector<uint8_t>& payload, UxInputEvent& event, std::string& text)
{
	if (payload.size() < sizeof(UxInputEvent))
	{
		return false;
	}

	std::memcpy(&event, payload.data(), sizeof(UxInputEvent));
	const size_t available = payload.size() - sizeof(UxInputEvent);
	if (event.TextBytes > available || event.TextBytes > UXWV_IPC_MAX_INPUT_TEXT)
	{
		return false;
	}
	if (event.TextBytes > 0)
	{
		text.assign(reinterpret_cast<const char*>(payload.data() + sizeof(UxInputEvent)),
			event.TextBytes);
	}
	return true;
}
} // namespace

HostApp::HostApp(HostOptions options)
	: Options(std::move(options))
	, Manager(Options.AdapterLuid, Options.OwnerPid,
		[this](uint32_t instanceId, uint32_t type, const std::string& json)
		{
			SendEvent(instanceId, type, json);
		})
{
}

HostApp::~HostApp()
{
	StopInfrastructure();
}

bool HostApp::StartInfrastructure()
{
	EmptySinceSeconds = NowSeconds();
	LastIdleCheckSeconds = EmptySinceSeconds;

	Ipc = std::make_unique<IpcServer>(
		Options.IpcName,
		[this](const UxIpcMessage& message, const std::vector<uint8_t>& payload)
		{
			OnIpcMessage(message, payload);
		},
		[this]()
		{
			UXWV_LOG_WARN("IPC client disconnected, shutting down");
			CefPostTask(TID_UI, new QuitTask());
		});

	if (!Ipc->Start())
	{
		return false;
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
	Manager.ShutdownAll();
}

void HostApp::OnBeforeCommandLineProcessing(const CefString& processType,
	CefRefPtr<CefCommandLine> commandLine)
{
	// Background throttling would starve pages while the game is busy.
	commandLine->AppendSwitch("disable-background-timer-throttling");
	commandLine->AppendSwitch("disable-renderer-backgrounding");
	commandLine->AppendSwitch("disable-backgrounding-occluded-windows");
	commandLine->AppendSwitch("enable-begin-frame-control");

	if (Options.DevToolsPort > 0)
	{
		commandLine->AppendSwitchWithValue("remote-debugging-port",
			std::to_string(Options.DevToolsPort));
	}
}

void HostApp::OnContextInitialized()
{
	UXWV_LOG_INFO("CEF context initialized");
	FlushPendingMessages();
	ScheduleIdleCheck();
}

void HostApp::EnsureRendererRouters()
{
	if (RendererBridgeRouter && RendererExecRouter)
	{
		return;
	}

	CefMessageRouterConfig bridgeConfig;
	bridgeConfig.js_query_function = BridgeQueryFunction();
	bridgeConfig.js_cancel_function = std::string(BridgeQueryFunction()) + "Cancel";
	RendererBridgeRouter = CefMessageRouterRendererSide::Create(bridgeConfig);

	CefMessageRouterConfig execConfig;
	execConfig.js_query_function = ExecQueryFunction();
	execConfig.js_cancel_function = std::string(ExecQueryFunction()) + "Cancel";
	RendererExecRouter = CefMessageRouterRendererSide::Create(execConfig);
}

void HostApp::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	CefRefPtr<CefV8Context> context)
{
	EnsureRendererRouters();
	RendererBridgeRouter->OnContextCreated(browser, frame, context);
	RendererExecRouter->OnContextCreated(browser, frame, context);

	// Only the main frame gets the friendly page API; iframe queries are also
	// origin checked against their own URL by the browser side.
	if (frame && frame->IsMain())
	{
		UXWV_LOG_INFO("renderer context created for '%s'",
			frame->GetURL().ToString().c_str());
		frame->ExecuteJavaScript(CefString(BridgeBootstrapScript()), frame->GetURL(), 0);
	}
}

void HostApp::OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	CefRefPtr<CefV8Context> context)
{
	if (!RendererBridgeRouter)
	{
		return;
	}
	RendererBridgeRouter->OnContextReleased(browser, frame, context);
	RendererExecRouter->OnContextReleased(browser, frame, context);
}

bool HostApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	CefProcessId sourceProcess, CefRefPtr<CefProcessMessage> message)
{
	if (!RendererBridgeRouter)
	{
		return false;
	}
	if (RendererBridgeRouter->OnProcessMessageReceived(browser, frame, sourceProcess, message))
	{
		return true;
	}
	return RendererExecRouter->OnProcessMessageReceived(browser, frame, sourceProcess, message);
}

void HostApp::OnIpcMessage(const UxIpcMessage& message, const std::vector<uint8_t>& payload)
{
	std::lock_guard<std::mutex> Lock(PendingMutex);
	UXWV_LOG_DEBUG("IPC dispatch type %u (context ready=%d)", message.Type,
		bContextReady ? 1 : 0);
	if (!bContextReady)
	{
		PendingMessages.emplace_back(message, payload);
		return;
	}

	CefPostTask(TID_UI, new IpcTask(this, message, payload));
}

void HostApp::FlushPendingMessages()
{
	std::vector<std::pair<UxIpcMessage, std::vector<uint8_t>>> pending;
	{
		std::lock_guard<std::mutex> Lock(PendingMutex);
		pending.swap(PendingMessages);
		bContextReady = true;
	}

	for (const auto& entry : pending)
	{
		ProcessMessage(entry.first, entry.second);
	}
}

void HostApp::ProcessMessage(const UxIpcMessage& message, const std::vector<uint8_t>& payload)
{
	switch (message.Type)
	{
	case UXWV_IPC_HELLO:
	{
		SendMessage(UXWV_IPC_HELLO_ACK, 0, message.RequestId,
			UXWV_IPC_VERSION, 0, UXWV_IPC_FLAG_RESPONSE, "{}");
		UXWV_LOG_INFO("client hello accepted");
		break;
	}
	case UXWV_IPC_CREATE:
	{
		HandleCreate(message, payload);
		break;
	}
	case UXWV_IPC_DESTROY:
	{
		UXWV_LOG_INFO("instance %u: destroy requested", message.Instance);
		Manager.Destroy(message.Instance);
		break;
	}
	case UXWV_IPC_NAVIGATE:
	{
		const CefRefPtr<CefDictionaryValue> dict = ParsePayload(payload);
		if (dict)
		{
			Manager.Navigate(message.Instance, GetString(dict, "url"));
		}
		break;
	}
	case UXWV_IPC_LOAD_HTML:
	{
		const CefRefPtr<CefDictionaryValue> dict = ParsePayload(payload);
		if (dict)
		{
			Manager.LoadHtml(message.Instance,
				GetString(dict, "html"), GetString(dict, "base"));
		}
		break;
	}
	case UXWV_IPC_RELOAD:
	{
		Manager.Reload(message.Instance);
		break;
	}
	case UXWV_IPC_STOP:
	{
		Manager.StopLoading(message.Instance);
		break;
	}
	case UXWV_IPC_RESIZE:
	{
		const CefRefPtr<CefDictionaryValue> dict = ParsePayload(payload);
		Manager.Resize(message.Instance, static_cast<int>(message.A), static_cast<int>(message.B),
			GetFloat(dict, "scale", 1.0f));
		break;
	}
	case UXWV_IPC_SET_VISIBLE:
	{
		Manager.SetVisible(message.Instance, message.A != 0);
		break;
	}
	case UXWV_IPC_SET_FRAME_RATE:
	{
		Manager.SetFrameRate(message.Instance, static_cast<int>(message.A));
		break;
	}
	case UXWV_IPC_FOCUS:
	{
		Manager.Focus(message.Instance, message.A != 0);
		break;
	}
	case UXWV_IPC_BEGIN_FRAME:
	{
		Manager.BeginFrame(message.Instance);
		break;
	}
	case UXWV_IPC_INPUT:
	{
		HandleInput(message, payload);
		break;
	}
	case UXWV_IPC_EXEC:
	{
		const CefRefPtr<CefDictionaryValue> dict = ParsePayload(payload);
		if (dict)
		{
			UXWV_LOG_INFO("instance %u: exec request %u", message.Instance, message.RequestId);
			Manager.ExecuteScript(message.Instance, message.RequestId,
				GetString(dict, "script"));
		}
		else
		{
			UXWV_LOG_WARN("instance %u: exec request %u without a payload",
				message.Instance, message.RequestId);
		}
		break;
	}
	case UXWV_IPC_EMIT:
	{
		const CefRefPtr<CefDictionaryValue> dict = ParsePayload(payload);
		if (dict)
		{
			Manager.EmitToWeb(message.Instance, GetString(dict, "event"),
				GetString(dict, "payload"));
		}
		break;
	}
	case UXWV_IPC_BRIDGE_RESULT:
	{
		const CefRefPtr<CefDictionaryValue> dict = ParsePayload(payload);
		if (dict)
		{
			Manager.ResolveBridge(message.Instance, GetInt(dict, "request", 0),
				GetBool(dict, "success", false), GetString(dict, "result"));
		}
		break;
	}
	case UXWV_IPC_REQUEST_DEVTOOLS:
	{
		const std::string url = Options.DevToolsPort > 0
			? "http://127.0.0.1:" + std::to_string(Options.DevToolsPort)
			: std::string();
		CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
		dict->SetString("url", url);
		CefRefPtr<CefValue> root = CefValue::Create();
		root->SetDictionary(dict);
		SendEvent(message.Instance, UXWV_IPC_EVENT_DEVTOOLS,
			CefWriteJSON(root, JSON_WRITER_DEFAULT).ToString());
		break;
	}
	case UXWV_IPC_PING:
	{
		SendMessage(UXWV_IPC_PONG, 0, message.RequestId, 0, 0,
			UXWV_IPC_FLAG_RESPONSE, "{}");
		break;
	}
	case UXWV_IPC_SHUTDOWN:
	{
		SendMessage(UXWV_IPC_SHUTDOWN_ACK, 0, message.RequestId, 0, 0,
			UXWV_IPC_FLAG_RESPONSE, "{}");
		RequestQuit();
		break;
	}
	default:
	{
		UXWV_LOG_WARN("unhandled IPC message type %u", message.Type);
		break;
	}
	}
}

void HostApp::HandleCreate(const UxIpcMessage& message, const std::vector<uint8_t>& payload)
{
	const CefRefPtr<CefDictionaryValue> dict = ParsePayload(payload);
	if (!dict)
	{
		UXWV_LOG_ERROR("instance %u: create without a JSON payload", message.Instance);
		SendMessage(UXWV_IPC_CREATE_ACK, message.Instance, message.RequestId, 0, 0,
			UXWV_IPC_FLAG_RESPONSE, "{\"ok\":false}");
		return;
	}

	const std::string sharedMemory = GetString(dict, "shmem");
	const int width = GetInt(dict, "width", 1280);
	const int height = GetInt(dict, "height", 720);
	const int frameRate = GetInt(dict, "fps", 60);
	const bool visible = GetBool(dict, "visible", true);
	const float deviceScale = GetFloat(dict, "scale", 1.0f);
	const std::string url = GetString(dict, "url");
	const std::vector<std::string> allowedOrigins = GetStringList(dict, "origins");

	const bool created = Manager.Create(message.Instance, sharedMemory, width, height,
		url.empty() ? "about:blank" : url, frameRate, visible, deviceScale, allowedOrigins);

	CefRefPtr<CefDictionaryValue> reply = CefDictionaryValue::Create();
	reply->SetBool("ok", created);
	CefRefPtr<CefValue> root = CefValue::Create();
	root->SetDictionary(reply);
	SendMessage(UXWV_IPC_CREATE_ACK, message.Instance, message.RequestId,
		static_cast<uint32_t>(width), static_cast<uint32_t>(height),
		UXWV_IPC_FLAG_RESPONSE, CefWriteJSON(root, JSON_WRITER_DEFAULT).ToString());

	if (!created)
	{
		UXWV_LOG_ERROR("instance %u: create failed", message.Instance);
	}
}

void HostApp::HandleInput(const UxIpcMessage& message, const std::vector<uint8_t>& payload)
{
	UxInputEvent event{};
	std::string text;
	if (!ParseInput(payload, event, text))
	{
		UXWV_LOG_WARN("instance %u: malformed input payload", message.Instance);
		return;
	}
	Manager.SendInput(message.Instance, event, text);
}

void HostApp::SendMessage(uint32_t type, uint32_t instance, uint32_t requestId,
	uint32_t a, uint32_t b, uint32_t flags, const std::string& payload)
{
	if (!Ipc)
	{
		return;
	}

	UxIpcMessage message{};
	message.Size = static_cast<uint32_t>(payload.size());
	message.Type = type;
	message.ProtocolVersion = UXWV_IPC_VERSION;
	message.Instance = instance;
	message.RequestId = requestId;
	message.A = a;
	message.B = b;
	message.Flags = flags;

	std::vector<uint8_t> bytes(payload.begin(), payload.end());
	Ipc->Send(message, bytes);
}

void HostApp::SendEvent(uint32_t instance, uint32_t type, const std::string& json)
{
	SendMessage(type, instance, 0, 0, 0, 0, json);
}

void HostApp::ScheduleIdleCheck()
{
	CefPostDelayedTask(TID_UI, new IdleTask(this), 5000);
}

void HostApp::CheckIdle()
{
	if (bQuitting)
	{
		return;
	}

	const double now = NowSeconds();

	// The game process is the owner; when it disappears the IPC pipe usually
	// disconnects too, but a suspended game may keep the pipe open.
	if (Options.OwnerPid != 0 && !IsOwnerAlive())
	{
		UXWV_LOG_WARN("owner process %u is gone, shutting down", Options.OwnerPid);
		RequestQuit();
		return;
	}

	if (Manager.Count() == 0)
	{
		if (EmptySinceSeconds == 0.0)
		{
			EmptySinceSeconds = now;
		}
		else if (Options.IdleSeconds > 0 && now - EmptySinceSeconds >= Options.IdleSeconds)
		{
			UXWV_LOG_INFO("no webviews for %d seconds, shutting down", Options.IdleSeconds);
			RequestQuit();
			return;
		}
	}
	else
	{
		EmptySinceSeconds = 0.0;
	}

	LastIdleCheckSeconds = now;
	ScheduleIdleCheck();
}

void HostApp::RequestQuit()
{
	if (bQuitting)
	{
		return;
	}
	bQuitting = true;
	UXWV_LOG_INFO("host shutting down");
	CefPostTask(TID_UI, new QuitTask());
}

bool HostApp::IsOwnerAlive()
{
#if defined(_WIN32)
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, Options.OwnerPid);
	if (!process)
	{
		return false;
	}
	const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
	CloseHandle(process);
	return alive;
#else
	return true;
#endif
}
} // namespace uxwv
