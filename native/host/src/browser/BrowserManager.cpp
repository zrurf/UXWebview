#include "browser/BrowserManager.h"

#include <cmath>

#include "cef_frame.h"
#include "cef_parser.h"
#include "cef_task.h"
#include "cef_values.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

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

#if defined(_WIN32)
/**
 * The overlay rectangle arrives in the game window's client coordinates, but a
 * popup window is positioned on the desktop, so the client origin is added.
 */
CefRect OverlayScreenRect(CefWindowHandle parent, int x, int y, int width, int height)
{
	POINT origin{0, 0};
	if (parent && ::IsWindow(parent))
	{
		::ClientToScreen(parent, &origin);
	}
	return CefRect(origin.x + x, origin.y + y, width, height);
}

/**
 * Windowed browsers lay out at the monitor DPI, unlike windowless ones where
 * GetScreenInfo sets it. Zooming the page brings a windowed overlay back to the
 * scale the widget asked for, so its CSS pixels match a texture view of the same
 * size: CSS = physical / deviceScale, and the page gets the same DPI.
 */
float PageZoomFor(float deviceScale, HWND window)
{
	const float requested = deviceScale > 0.0f ? deviceScale : 1.0f;
	const float osScale = window && ::IsWindow(window)
		? static_cast<float>(::GetDpiForWindow(window)) / 96.0f
		: 1.0f;
	if (osScale <= 0.0f)
	{
		return 1.0f;
	}
	return requested / osScale;
}

/**
 * CEF always turns the browser window into a child of the handle it is given,
 * so the overlay uses a host-owned carrier popup instead of the game window:
 * the game's flip-model swap chain hides child HWNDs (airspace), a plain popup
 * above the game does not.
 */
HWND CreateOverlayWindow(HWND owner, const CefRect& bounds)
{
	static const ATOM windowClass = []()
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = ::DefWindowProcW;
		wc.hInstance = ::GetModuleHandleW(nullptr);
		wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
		wc.lpszClassName = L"UXWebviewOverlayWindow";
		return ::RegisterClassExW(&wc);
	}();
	if (windowClass == 0)
	{
		UXWV_LOG_ERROR("overlay window class registration failed (%lu)",
			::GetLastError());
		return nullptr;
	}

	HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"UXWebviewOverlayWindow",
		L"UXWebview overlay", WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		bounds.x, bounds.y, bounds.width, bounds.height, owner, nullptr,
		::GetModuleHandleW(nullptr), nullptr);
	if (!hwnd)
	{
		UXWV_LOG_ERROR("overlay window creation failed (%lu)", ::GetLastError());
	}
	return hwnd;
}

#endif
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
	const std::vector<std::string>& allowedOrigins, bool overlay, uint64_t parentHandle,
	int posX, int posY)
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
	if (overlay && parentHandle == 0)
	{
		UXWV_LOG_ERROR("instance %u: overlay needs a parent window handle", id);
		return false;
	}

	auto instance = std::make_unique<Instance>();
	instance->Id = id;
	instance->PendingUrl = url;
	instance->SharedMemoryName = sharedMemoryName;
	instance->bVisible = visible;
	instance->bOverlay = overlay;
	instance->ParentHandle = parentHandle;
	instance->PosX = posX;
	instance->PosY = posY;
	instance->Width = width;
	instance->Height = height;
	instance->DeviceScale = deviceScale > 0.0f ? deviceScale : 1.0f;

	if (overlay)
	{
		const CefWindowHandle owner =
			reinterpret_cast<CefWindowHandle>(static_cast<uintptr_t>(parentHandle));
		HWND carrier = CreateOverlayWindow(owner,
			OverlayScreenRect(owner, posX, posY, width, height));
		if (!carrier)
		{
			UXWV_LOG_ERROR("instance %u: overlay carrier window failed", id);
			return false;
		}
		instance->CarrierHandle = reinterpret_cast<uint64_t>(carrier);
	}

	// Overlay browsers composite natively, so they publish no frames at all.
	if (!overlay)
	{
		instance->Publisher = std::make_unique<FramePublisher>();
		if (!instance->Publisher->Start(sharedMemoryName, AdapterLuid, OwnerPid))
		{
			UXWV_LOG_ERROR("instance %u: frame publisher failed", id);
			return false;
		}
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
	CefBrowserSettings browserSettings;
	if (overlay)
	{
		// Chromium's child window fills the carrier popup, which is a member of
		// the game window's z-order but outside its flip-model swap chain.
		const CefWindowHandle carrier =
			reinterpret_cast<CefWindowHandle>(static_cast<uintptr_t>(raw->CarrierHandle));
		windowInfo.SetAsChild(carrier, CefRect(0, 0, width, height));
		// The window lays out at the monitor DPI, so the page is zoomed to the
		// widget's scale before it is created.
		ApplyOverlayPageZoom(*raw);
	}
	else
	{
		windowInfo.SetAsWindowless(nullptr);
		windowInfo.shared_texture_enabled = true;
		windowInfo.external_begin_frame_enabled = frameRate > 0;
		browserSettings.windowless_frame_rate = frameRate > 0 ? frameRate : 60;
	}

	UXWV_LOG_INFO("instance %u: creating %s browser %dx%d at scale %.3f fps=%d url='%s'",
		id, overlay ? "overlay" : "off-screen", width, height, deviceScale, frameRate, url.c_str());

	CefBrowserHost::CreateBrowser(
		windowInfo, raw->Client, CefString(url), browserSettings, nullptr, nullptr);
	return true;
}

bool BrowserManager::IsOverlay(uint32_t id) const
{
	auto it = Instances.find(id);
	return it != Instances.end() && it->second->bOverlay;
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
		HWND carrier = reinterpret_cast<HWND>(static_cast<uintptr_t>(instance->CarrierHandle));
		if (carrier && ::IsWindow(carrier))
		{
			::DestroyWindow(carrier);
		}
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
	if (!instance->Browser)
	{
		return;
	}

	if (instance->bOverlay)
	{
		// The native window is the view; moving it resizes Chromium's layout.
		MoveOverlayWindow(*instance, instance->PosX, instance->PosY, width, height);
		return;
	}

	// A new scale changes the DIP layout, which CEF only picks up when the
	// screen info is reported as changed.
	instance->Browser->GetHost()->NotifyScreenInfoChanged();
	instance->Browser->GetHost()->WasResized();
}

void BrowserManager::SetBounds(uint32_t id, int x, int y, int width, int height,
	float deviceScale)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || width <= 0 || height <= 0)
	{
		return;
	}

	Instance* instance = it->second.get();
	instance->PosX = x;
	instance->PosY = y;
	instance->DeviceScale = deviceScale > 0.0f ? deviceScale : 1.0f;
	instance->Client->SetViewSize(width, height, deviceScale);
	if (!instance->bOverlay || !instance->Browser)
	{
		return;
	}

	MoveOverlayWindow(*instance, x, y, width, height);
	// Resizing or moving across monitors can change the page scale Chromium would
	// otherwise keep from the monitor's DPI.
	ApplyOverlayPageZoom(*instance);
	UXWV_LOG_INFO("instance %u: overlay bounds %d,%d %dx%d at scale %.3f",
		id, x, y, width, height, deviceScale);
}

void BrowserManager::MoveOverlayWindow(Instance& instance, int x, int y, int width,
	int height)
{
	instance.PosX = x;
	instance.PosY = y;
	instance.Width = width;
	instance.Height = height;

	HWND carrier = reinterpret_cast<HWND>(static_cast<uintptr_t>(instance.CarrierHandle));
	if (!carrier || !::IsWindow(carrier))
	{
		return;
	}

	const CefRect bounds = OverlayScreenRect(
		reinterpret_cast<CefWindowHandle>(static_cast<uintptr_t>(instance.ParentHandle)),
		x, y, width, height);
	::SetWindowPos(carrier, nullptr, bounds.x, bounds.y, width, height,
		SWP_NOZORDER | SWP_NOACTIVATE);

	// Chromium does not follow the carrier on its own, so the browser window is
	// stretched to its client area; GetViewRect still defines the page layout.
	CefWindowHandle handle = instance.Browser ? instance.Browser->GetHost()->GetWindowHandle()
		: nullptr;
	if (handle)
	{
		::SetWindowPos(handle, nullptr, 0, 0, width, height,
			SWP_NOZORDER | SWP_NOACTIVATE);
		instance.Browser->GetHost()->NotifyMoveOrResizeStarted();
	}
}

void BrowserManager::ApplyOverlayPageZoom(Instance& instance)
{
	if (!instance.Client || !instance.bOverlay)
	{
		return;
	}

	HWND carrier = reinterpret_cast<HWND>(static_cast<uintptr_t>(instance.CarrierHandle));
	instance.Client->SetPageZoom(PageZoomFor(instance.DeviceScale, carrier));
}

void BrowserManager::SetVisible(uint32_t id, bool visible)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Browser)
	{
		return;
	}

	Instance* instance = it->second.get();
	instance->bVisible = visible;

	if (instance->bOverlay)
	{
		HWND carrier = reinterpret_cast<HWND>(static_cast<uintptr_t>(instance->CarrierHandle));
		if (carrier && ::IsWindow(carrier))
		{
			// SW_SHOWNA shows the popup above the game without stealing focus;
			// a click on the page activates it when the user wants to type.
			::ShowWindow(carrier, visible ? SW_SHOWNA : SW_HIDE);
			UXWV_LOG_INFO("instance %u: overlay window %s", id, visible ? "shown" : "hidden");
		}
		return;
	}

	instance->Browser->GetHost()->WasHidden(!visible);
}

void BrowserManager::SetFrameRate(uint32_t id, int frameRate)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Browser || frameRate <= 0)
	{
		return;
	}
	if (it->second->bOverlay)
	{
		// Windowed browsers are paced by the compositor, not by external begin frames.
		return;
	}
	it->second->Browser->GetHost()->SetWindowlessFrameRate(frameRate);
}

void BrowserManager::Focus(uint32_t id, bool focused)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Browser)
	{
		return;
	}

	CefRefPtr<CefBrowserHost> host = it->second->Browser->GetHost();
	if (it->second->bOverlay)
	{
		if (focused)
		{
			CefWindowHandle handle = host->GetWindowHandle();
			if (handle)
			{
				::SetFocus(handle);
			}
		}
		else
		{
			host->SetFocus(false);
		}
		return;
	}
	host->SetFocus(focused);
}

void BrowserManager::BeginFrame(uint32_t id)
{
	auto it = Instances.find(id);
	if (it == Instances.end() || !it->second->Browser || it->second->bOverlay)
	{
		return;
	}
	it->second->Browser->GetHost()->SendExternalBeginFrame();
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
	// The plugin sends view CSS pixels, while Chromium's mouse API takes view DIPs
	// and lays the page out at DIP * page zoom. An overlay page is zoomed to reach
	// the widget scale, so CSS is converted back to DIP here; a texture page
	// zooming at 1 leaves the coordinates untouched.
	const float pageZoom = it->second->Client->GetPageZoom();
	mouse.x = static_cast<int>(std::lround(event.X * pageZoom));
	mouse.y = static_cast<int>(std::lround(event.Y * pageZoom));
	mouse.modifiers = ToCefModifiers(event.Modifiers)
		// Held buttons must ride along on move events, otherwise Chromium
		// treats a drag as hover and text selection never starts.
		| static_cast<int>(it->second->PressedButtons);

	switch (event.Type)
	{
	case UXWV_INPUT_MOUSE_MOVE:
		host->SendMouseMoveEvent(mouse, false);
		break;
	case UXWV_INPUT_MOUSE_DOWN:
	case UXWV_INPUT_MOUSE_UP:
	{
		const uint32_t ButtonFlag = event.Button == 1 ? EVENTFLAG_RIGHT_MOUSE_BUTTON
			: event.Button == 2 ? EVENTFLAG_MIDDLE_MOUSE_BUTTON
			: EVENTFLAG_LEFT_MOUSE_BUTTON;
		// Setting the matching button flag lets the page see the right button.
		mouse.modifiers |= static_cast<int>(ButtonFlag);
		host->SendMouseClickEvent(mouse, ToMouseButton(event.Button),
			event.Type == UXWV_INPUT_MOUSE_UP, 1);

		if (event.Type == UXWV_INPUT_MOUSE_DOWN)
		{
			it->second->PressedButtons |= ButtonFlag;
		}
		else
		{
			it->second->PressedButtons &= ~ButtonFlag;
		}
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
	case UXWV_INPUT_IME_COMPOSITION:
	{
		// CefString wraps UTF-8 and the IME API expects UTF-16 code units.
		const CefString Utf8Text(text);
		const std::wstring Wide = Utf8Text.ToWString();
		host->ImeSetComposition(CefString(Wide), {},
			CefRange(UINT32_MAX, UINT32_MAX), CefRange(0, static_cast<uint32_t>(Wide.size())));
		break;
	}
	case UXWV_INPUT_IME_COMMIT:
	{
		const CefString Utf8Text(text);
		host->ImeCommitText(CefString(Utf8Text.ToWString()), CefRange(UINT32_MAX, UINT32_MAX), 0);
		break;
	}
	case UXWV_INPUT_IME_CANCEL:
		host->ImeCancelComposition();
		break;
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

	if (instance->bOverlay)
	{
		// The carrier is created hidden; the plugin shows it once the widget
		// rectangle is known, which avoids a flash at the origin.
		MoveOverlayWindow(*instance, instance->PosX, instance->PosY, instance->Width,
			instance->Height);
		HWND carrier = reinterpret_cast<HWND>(static_cast<uintptr_t>(instance->CarrierHandle));
		if (carrier && ::IsWindow(carrier))
		{
			if (instance->bVisible)
			{
				::ShowWindow(carrier, SW_SHOWNA);
			}
		}
	}
	else if (!instance->bVisible)
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

	HWND carrier = reinterpret_cast<HWND>(static_cast<uintptr_t>(it->second->CarrierHandle));
	if (carrier && ::IsWindow(carrier))
	{
		// The browser window is gone; the carrier would otherwise stay behind as
		// an empty popup above the game.
		::DestroyWindow(carrier);
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
