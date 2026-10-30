#include "browser/BrowserManager.h"

#include "cef_frame.h"
#include "cef_parser.h"
#include "cef_values.h"

#include "util/Log.h"

namespace uxwv
{
namespace
{
int ToCefModifiers(uint32_t modifiers)
{
	int result = 0;
	if (modifiers & UXWV_MOD_SHIFT)
	{
		result |= EVENTFLAG_SHIFT_DOWN;
	}
	if (modifiers & UXWV_MOD_CTRL)
	{
		result |= EVENTFLAG_CONTROL_DOWN;
	}
	if (modifiers & UXWV_MOD_ALT)
	{
		result |= EVENTFLAG_ALT_DOWN;
	}
	return result;
}

CefBrowserHost::MouseButtonType ToMouseButton(uint32_t button)
{
	switch (button)
	{
	case 1: return MBT_RIGHT;
	case 2: return MBT_MIDDLE;
	default: return MBT_LEFT;
	}
}

std::string EscapeHtmlAttribute(const std::string& value)
{
	std::string escaped;
	escaped.reserve(value.size());
	for (const char c : value)
	{
		if (c == '"' || c == '<' || c == '>')
		{
			continue;
		}
		escaped.push_back(c);
	}
	return escaped;
}

// data: URLs have no base, so inject a <base> element when a base is given.
std::string InjectBaseUrl(const std::string& html, const std::string& base)
{
	if (base.empty())
	{
		return html;
	}

	const std::string tag = "<base href=\"" + EscapeHtmlAttribute(base) + "\">";
	const size_t head = html.find("<head>");
	if (head != std::string::npos)
	{
		return html.substr(0, head + 6) + tag + html.substr(head + 6);
	}
	const size_t headOpen = html.find("<head");
	if (headOpen != std::string::npos)
	{
		const size_t end = html.find('>', headOpen);
		if (end != std::string::npos)
		{
			return html.substr(0, end + 1) + tag + html.substr(end + 1);
		}
	}
	return tag + html;
}
} // namespace

BrowserManager::BrowserManager(uint64_t adapterLuid, uint32_t ownerPid, EventSender onEvent)
	: AdapterLuid(adapterLuid), OwnerPid(ownerPid), OnEvent(std::move(onEvent))
{
}

BrowserManager::~BrowserManager()
{
	ShutdownAll();
}

bool BrowserManager::Create(uint32_t id, const std::string& sharedMemoryName, int width, int height,
	const std::string& url, int frameRate, bool visible, float deviceScale,
	const std::vector<std::string>& allowedOrigins)
{
	if (width <= 0 || height <= 0)
	{
		UXWV_LOG_ERROR("instance %u: invalid size %dx%d", id, width, height);
		return false;
	}
	if (Instances.find(id) != Instances.end())
	{
		UXWV_LOG_WARN("instance %u already exists", id);
		return false;
	}

	auto instance = std::make_unique<Instance>();
	instance->Id = id;
	instance->Publisher = std::make_unique<FramePublisher>();
	instance->PendingUrl = url;
	instance->SharedMemoryName = sharedMemoryName;
	instance->bVisible = visible;

	if (!instance->Publisher->Start(sharedMemoryName, AdapterLuid, OwnerPid))
	{
		UXWV_LOG_ERROR("instance %u: frame publisher failed", id);
		return false;
	}

	instance->Client = new BrowserClient(
		id, instance->Publisher.get(), width, height, deviceScale,
		[this](uint32_t createdId, CefRefPtr<CefBrowser> browser)
		{
			OnBrowserCreated(createdId, browser);
		},
		[this](uint32_t closedId) { OnBrowserClosed(closedId); },
		[this](uint32_t eventInstance, uint32_t type, const std::string& json)
		{
			Emit(eventInstance, type, json);
		}, allowedOrigins);

	Instance* raw = instance.get();
	Instances[id] = std::move(instance);

	CefWindowInfo windowInfo;
	windowInfo.SetAsWindowless(nullptr);
	windowInfo.shared_texture_enabled = true;
	windowInfo.external_begin_frame_enabled = frameRate > 0;

	CefBrowserSettings browserSettings;
	browserSettings.windowless_frame_rate = frameRate > 0 ? frameRate : 60;

	UXWV_LOG_INFO("instance %u: creating browser %dx%d at scale %.3f fps=%d url='%s'",
		id, width, height, deviceScale, frameRate, url.c_str());

	CefBrowserHost::CreateBrowser(
		windowInfo, raw->Client, CefString(url), browserSettings, nullptr, nullptr);
	return true;
}

void BrowserManager::Destroy(uint32_t id)
{
	auto it = Instances.find(id);
	if (it == Instances.end())
	{
		return;
	}

	Instance* instance = it->second.get();
	instance->bClosing = true;
	if (instance->Browser)
	{
		UXWV_LOG_INFO("instance %u: closing browser", id);
		instance->Browser->GetHost()->CloseBrowser(true);
	}
	else
	{
		Instances.erase(it);
	}
}

void BrowserManager::ShutdownAll()
{
	for (auto& pair : Instances)
	{
		pair.second->bClosing = true;
		if (pair.second->Browser)
		{
			pair.second->Browser->GetHost()->CloseBrowser(true);
		}
	}
	// CefShutdown disposes any browser that did not close in time; the
	// instances and their frame publishers stay alive until then so no
	// callback observes a dangling publisher.
}

void BrowserManager::Navigate(uint32_t id, const std::string& url)
{
	auto it = Instances.find(id);
	if (it == Instances.end())
	{
		return;
	}

	Instance* instance = it->second.get();
	instance->PendingUrl = url;
	if (!instance->Browser)
	{
		return;
	}
	instance->Browser->GetMainFrame()->LoadURL(CefString(url));
}

void BrowserManager::LoadHtml(uint32_t id, const std::string& html, const std::string& base)
{
	const std::string document = InjectBaseUrl(html, base);
	const std::string encoded =
		CefBase64Encode(document.data(), document.size()).ToString();
	Navigate(id, "data:text/html;charset=utf-8;base64," + encoded);
}

void BrowserManager::Reload(uint32_t id)
{
	auto it = Instances.find(id);
	if (it != Instances.end() && it->second->Browser)
	{
		it->second->Browser->Reload();
	}
}

void BrowserManager::StopLoading(uint32_t id)
{
	auto it = Instances.find(id);
	if (it != Instances.end() && it->second->Browser)
	{
		it->second->Browser->StopLoad();
	}
}

void BrowserManager::Resize(uint32_t id, int width, int height, float deviceScale)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || width <= 0 || height <= 0)
	{
		return;
	}

	Instance* instance = it->second.get();
	instance->Client->SetViewSize(width, height, deviceScale);
	if (instance->Browser)
	{
		// A new scale changes the DIP layout, which CEF only picks up when the
		// screen info is reported as changed.
		instance->Browser->GetHost()->NotifyScreenInfoChanged();
		instance->Browser->GetHost()->WasResized();
	}
}

void BrowserManager::SetVisible(uint32_t id, bool visible)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Browser)
	{
		return;
	}

	CefRefPtr<CefBrowserHost> host = it->second->Browser->GetHost();
	host->WasHidden(!visible);
}

void BrowserManager::SetFrameRate(uint32_t id, int frameRate)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Browser || frameRate <= 0)
	{
		return;
	}
	it->second->Browser->GetHost()->SetWindowlessFrameRate(frameRate);
}

void BrowserManager::Focus(uint32_t id, bool focused)
{
	auto it = Instances.find(id);
	if (it != Instances.end() && it->second->Browser)
	{
		it->second->Browser->GetHost()->SetFocus(focused);
	}
}

void BrowserManager::BeginFrame(uint32_t id)
{
	auto it = Instances.find(id);
	if (it != Instances.end() && it->second->Browser)
	{
		it->second->Browser->GetHost()->SendExternalBeginFrame();
	}
}

void BrowserManager::SendInput(uint32_t id, const UxInputEvent& event, const std::string& text)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Browser)
	{
		return;
	}

	CefRefPtr<CefBrowserHost> host = it->second->Browser->GetHost();
	CefMouseEvent mouse;
	mouse.x = static_cast<int>(event.X);
	mouse.y = static_cast<int>(event.Y);
	mouse.modifiers = ToCefModifiers(event.Modifiers);

	switch (event.Type)
	{
	case UXWV_INPUT_MOUSE_MOVE:
		host->SendMouseMoveEvent(mouse, false);
		break;
	case UXWV_INPUT_MOUSE_DOWN:
	case UXWV_INPUT_MOUSE_UP:
	{
		// Setting the matching button flag lets the page see the right button.
		mouse.modifiers |= event.Button == 1 ? EVENTFLAG_RIGHT_MOUSE_BUTTON
			: event.Button == 2 ? EVENTFLAG_MIDDLE_MOUSE_BUTTON
			: EVENTFLAG_LEFT_MOUSE_BUTTON;
		host->SendMouseClickEvent(mouse, ToMouseButton(event.Button),
			event.Type == UXWV_INPUT_MOUSE_UP, 1);
		break;
	}
	case UXWV_INPUT_MOUSE_WHEEL:
		host->SendMouseWheelEvent(mouse, 0, static_cast<int>(event.WheelDelta * 120.0f));
		break;
	case UXWV_INPUT_KEY_DOWN:
	case UXWV_INPUT_KEY_UP:
	{
		CefKeyEvent keyEvent{};
		keyEvent.type = event.Type == UXWV_INPUT_KEY_DOWN ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
		keyEvent.windows_key_code = static_cast<int>(event.KeyCode);
		keyEvent.native_key_code = static_cast<int>(event.KeyCode);
		keyEvent.modifiers = ToCefModifiers(event.Modifiers);
		host->SendKeyEvent(keyEvent);
		break;
	}
	case UXWV_INPUT_CHAR:
	{
		CefKeyEvent keyEvent{};
		keyEvent.type = KEYEVENT_CHAR;
		keyEvent.modifiers = ToCefModifiers(event.Modifiers);
		const CefString utf16(text);
		const std::wstring wide = utf16.ToWString();
		keyEvent.character = wide.empty() ? 0 : wide[0];
		keyEvent.unmodified_character = keyEvent.character;
		keyEvent.windows_key_code = static_cast<int>(event.KeyCode);
		host->SendKeyEvent(keyEvent);
		break;
	}
	default:
		break;
	}
}

void BrowserManager::ExecuteScript(uint32_t id, uint32_t requestId, const std::string& script)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Browser)
	{
		return;
	}

	// The result comes back through UXWV_IPC_EXEC_RESULT once the wrapper
	// finishes, so a hanging script cannot block the browser process.
	it->second->Client->ExecuteScript(requestId, script);
}

void BrowserManager::EmitToWeb(uint32_t id, const std::string& event, const std::string& payloadJson)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Client)
	{
		return;
	}
	it->second->Client->EmitEvent(event, payloadJson);
}

void BrowserManager::ResolveBridge(uint32_t id, int64_t queryId, bool success,
	const std::string& result)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Client)
	{
		return;
	}
	if (!it->second->Client->ResolveBridge(queryId, success, result))
	{
		UXWV_LOG_WARN("instance %u: bridge query %lld is no longer pending", id,
			static_cast<long long>(queryId));
	}
}

void BrowserManager::OnBrowserCreated(uint32_t id, CefRefPtr<CefBrowser> browser)
{
	auto it = Instances.find(id);
	if (it == Instances.end())
	{
		browser->GetHost()->CloseBrowser(true);
		return;
	}

	Instance* instance = it->second.get();
	instance->Browser = browser;
	Emit(id, UXWV_IPC_EVENT_READY, "{}");

	if (!instance->bVisible)
	{
		browser->GetHost()->WasHidden(true);
	}

	// Navigation requested before the browser existed wins over the start URL.
	if (!instance->PendingUrl.empty())
	{
		browser->GetMainFrame()->LoadURL(CefString(instance->PendingUrl));
	}
}

void BrowserManager::OnBrowserClosed(uint32_t id)
{
	auto it = Instances.find(id);
	if (it == Instances.end())
	{
		return;
	}
	Instances.erase(it);
}

void BrowserManager::Emit(uint32_t instanceId, uint32_t type, const std::string& json) const
{
	if (OnEvent)
	{
		OnEvent(instanceId, type, json);
	}
}
} // namespace uxwv
