#include "browser/BrowserClient.h"

#include <algorithm>
#include <cmath>

#include "cef_browser.h"
#include "cef_frame.h"
#include "cef_parser.h"
#include "cef_values.h"

#include "util/Log.h"

namespace uxwv
{
namespace
{
std::string MakeStringJson(const char* key, const std::string& value)
{
	CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
	dict->SetString(key, value);
	CefRefPtr<CefValue> root = CefValue::Create();
	root->SetDictionary(dict);
	return CefWriteJSON(root, JSON_WRITER_DEFAULT).ToString();
}

std::string MakeUrlCodeJson(const std::string& url, int code)
{
	CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
	dict->SetString("url", url);
	dict->SetInt("code", code);
	CefRefPtr<CefValue> root = CefValue::Create();
	root->SetDictionary(dict);
	return CefWriteJSON(root, JSON_WRITER_DEFAULT).ToString();
}
} // namespace

void BrowserClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
	rect = CefRect(0, 0, DipWidth, DipHeight);
}

bool BrowserClient::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info)
{
	if (DeviceScale <= 0.0f || std::abs(DeviceScale - 1.0f) < 0.001f)
	{
		return false;
	}

	// Keeping CSS pixels at DIPs makes page text the same size as native UI,
	// while the paint buffer stays at device resolution and therefore sharp.
	screen_info.device_scale_factor = DeviceScale;
	screen_info.depth = 32;
	screen_info.depth_per_component = 8;
	screen_info.is_monochrome = false;
	screen_info.rect = CefRect(0, 0, DipWidth, DipHeight);
	screen_info.available_rect = screen_info.rect;
	return true;
}

void BrowserClient::UpdateDipSize()
{
	const float scale = DeviceScale > 0.0f ? DeviceScale : 1.0f;
	DipWidth = std::max(1, static_cast<int>(std::lround(Width / scale)));
	DipHeight = std::max(1, static_cast<int>(std::lround(Height / scale)));
}

void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
	UXWV_LOG_INFO("instance %u: browser created (%dx%d)", InstanceId, Width, Height);
	if (OnCreated)
	{
		OnCreated(InstanceId, browser);
	}
}

void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
	UXWV_LOG_INFO("instance %u: browser closed", InstanceId);
	if (OnClosed)
	{
		OnClosed(InstanceId);
	}
}

void BrowserClient::OnPaint(CefRefPtr<CefBrowser> browser,
	PaintElementType type,
	const RectList& dirtyRects,
	const void* buffer,
	int width,
	int height)
{
	if (!bLoggedPaintFallback)
	{
		bLoggedPaintFallback = true;
		UXWV_LOG_WARN("instance %u: CPU paint (%dx%d), shared textures unavailable",
			InstanceId, width, height);
	}
}

void BrowserClient::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
	PaintElementType type,
	const RectList& dirtyRects,
	const CefAcceleratedPaintInfo& info)
{
	if (type != PET_VIEW || !Publisher)
	{
		return;
	}

	// The shared texture is at device resolution, which differs from the DIP
	// view rect whenever the display is scaled.
	const int paintWidth = info.extra.coded_size.width > 0
		? info.extra.coded_size.width : Width;
	const int paintHeight = info.extra.coded_size.height > 0
		? info.extra.coded_size.height : Height;
	Publisher->PublishFromHandle(info.shared_texture_handle, paintWidth, paintHeight);
}

void BrowserClient::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	TransitionType transitionType)
{
	if (!frame || !frame->IsMain())
	{
		return;
	}
	Emit(UXWV_IPC_EVENT_LOAD_START, MakeStringJson("url", frame->GetURL().ToString()));
}

void BrowserClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	int httpStatusCode)
{
	if (!frame || !frame->IsMain())
	{
		return;
	}
	Emit(UXWV_IPC_EVENT_LOAD_END, MakeUrlCodeJson(frame->GetURL().ToString(), httpStatusCode));
}

void BrowserClient::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl)
{
	if (!frame || !frame->IsMain() || errorCode == ERR_ABORTED)
	{
		return;
	}
	UXWV_LOG_WARN("instance %u: load failed (%d) for '%s'", InstanceId,
		static_cast<int>(errorCode), failedUrl.ToString().c_str());
}

void BrowserClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title)
{
	Emit(UXWV_IPC_EVENT_TITLE, MakeStringJson("title", title.ToString()));
}

void BrowserClient::OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	const CefString& url)
{
	if (!frame || !frame->IsMain())
	{
		return;
	}
	Emit(UXWV_IPC_EVENT_URL, MakeStringJson("url", url.ToString()));
}

void BrowserClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
	TerminationStatus status, int errorCode, const CefString& errorString)
{
	const std::string reason = std::string("render_process_terminated status=")
		+ std::to_string(static_cast<int>(status))
		+ " error=" + std::to_string(errorCode)
		+ " " + errorString.ToString();
	UXWV_LOG_ERROR("instance %u: %s", InstanceId, reason.c_str());
	Emit(UXWV_IPC_EVENT_CRASH, MakeStringJson("reason", reason));
}

void BrowserClient::SetViewSize(int width, int height, float deviceScale)
{
	const float scale = deviceScale > 0.0f ? deviceScale : 1.0f;
	if (width == Width && height == Height && std::abs(scale - DeviceScale) < 0.001f)
	{
		return;
	}

	Width = width;
	Height = height;
	DeviceScale = scale;
	UpdateDipSize();
	UXWV_LOG_INFO("instance %u: view %dx%d px = %dx%d dip at scale %.3f",
		InstanceId, Width, Height, DipWidth, DipHeight, DeviceScale);
}

void BrowserClient::Emit(uint32_t eventType, const std::string& json)
{
	if (OnEvent)
	{
		OnEvent(InstanceId, eventType, json);
	}
}
} // namespace uxwv
