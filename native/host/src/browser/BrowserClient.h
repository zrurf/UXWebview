#pragma once

#include <functional>

#include "cef_client.h"
#include "cef_life_span_handler.h"
#include "cef_render_handler.h"

#include "frame/FramePublisher.h"

namespace uxwv
{
// Receives OSR callbacks from CEF and forwards accelerated frames.
class BrowserClient : public CefClient,
                      public CefRenderHandler,
                      public CefLifeSpanHandler
{
public:
	using CreatedCallback = std::function<void(CefRefPtr<CefBrowser>)>;

	BrowserClient(FramePublisher* publisher, int width, int height, CreatedCallback onCreated)
		: Publisher(publisher), Width(width), Height(height), OnCreated(std::move(onCreated))
	{
	}

	// CefClient
	CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

	// CefRenderHandler
	void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
	void OnPaint(CefRefPtr<CefBrowser> browser,
		PaintElementType type,
		const RectList& dirtyRects,
		const void* buffer,
		int width,
		int height) override;
	void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
		PaintElementType type,
		const RectList& dirtyRects,
		const CefAcceleratedPaintInfo& info) override;

	// CefLifeSpanHandler
	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;

	void SetViewSize(int width, int height);

private:
	FramePublisher* Publisher = nullptr;
	CreatedCallback OnCreated;
	int Width;
	int Height;
	bool bLoggedPaintFallback = false;

	IMPLEMENT_REFCOUNTING(BrowserClient);
};
} // namespace uxwv
