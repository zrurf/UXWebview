#include "browser/BrowserClient.h"

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
	rect = CefRect(0, 0, Width, Height);
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

	Publisher->PublishFromHandle(info.shared_texture_handle, Width, Height);
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

void BrowserClient::SetViewSize(int width, int height)
{
	if (width == Width && height == Height)
	{
		return;
	}

	Width = width;
	Height = height;
	UXWV_LOG_INFO("instance %u: view resized to %dx%d", InstanceId, Width, Height);
}

void BrowserClient::Emit(uint32_t eventType, const std::string& json)
{
	if (OnEvent)
	{
		OnEvent(InstanceId, eventType, json);
	}
}
} // namespace uxwv
