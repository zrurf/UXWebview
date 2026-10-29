#include "browser/BrowserClient.h"

#include "cef_browser.h"

#include "util/Log.h"

namespace uxwv
{
void BrowserClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
	rect = CefRect(0, 0, Width, Height);
}

void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
	UXWV_LOG_INFO("browser created, view %dx%d", Width, Height);
	if (OnCreated)
	{
		OnCreated(browser);
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
		UXWV_LOG_WARN("received CPU paint (%dx%d); shared textures are unavailable", width, height);
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

void BrowserClient::SetViewSize(int width, int height)
{
	if (width == Width && height == Height)
	{
		return;
	}

	Width = width;
	Height = height;
	UXWV_LOG_INFO("view resized to %dx%d", Width, Height);
}
} // namespace uxwv
