#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cef_client.h"
#include "cef_display_handler.h"
#include "cef_life_span_handler.h"
#include "cef_load_handler.h"
#include "cef_render_handler.h"
#include "cef_request_handler.h"

#include "frame/FramePublisher.h"
#include "uxwebview_ipc.h"

namespace uxwv
{
// Routes events to HostApp, which turns them into IPC messages.
using BrowserEventCallback =
	std::function<void(uint32_t instanceId, uint32_t eventType, const std::string& json)>;

// One browser instance: paints into its own FramePublisher and reports page
// and crash events through the callback. All callbacks run on the CEF UI thread.
class BrowserClient : public CefClient,
                      public CefRenderHandler,
                      public CefLifeSpanHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler,
                      public CefRequestHandler
{
public:
	using CreatedCallback = std::function<void(uint32_t instanceId, CefRefPtr<CefBrowser> browser)>;
	using ClosedCallback = std::function<void(uint32_t instanceId)>;

	BrowserClient(uint32_t instanceId, FramePublisher* publisher, int width, int height,
		float deviceScale, CreatedCallback onCreated, ClosedCallback onClosed,
		BrowserEventCallback onEvent)
		: InstanceId(instanceId)
		, Publisher(publisher)
		, Width(width)
		, Height(height)
		, DeviceScale(deviceScale > 0.0f ? deviceScale : 1.0f)
		, OnCreated(std::move(onCreated))
		, OnClosed(std::move(onClosed))
		, OnEvent(std::move(onEvent))
	{
		UpdateDipSize();
	}

	// CefClient
	CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
	CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
	CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
	CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

	// CefRenderHandler
	void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
	bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override;
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
	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

	// CefLoadHandler
	void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		TransitionType transitionType) override;
	void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		int httpStatusCode) override;
	void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) override;

	// CefDisplayHandler
	void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
	void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		const CefString& url) override;

	// CefRequestHandler
	void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
		TerminationStatus status, int errorCode, const CefString& errorString) override;

	/** Size is in device pixels; CEF lays out at Size/DeviceScale device-independent pixels. */
	void SetViewSize(int width, int height, float deviceScale);
	int GetWidth() const { return Width; }
	int GetHeight() const { return Height; }
	float GetDeviceScale() const { return DeviceScale; }
	uint32_t GetInstanceId() const { return InstanceId; }

private:
	void Emit(uint32_t eventType, const std::string& json);
	void UpdateDipSize();

	uint32_t InstanceId;
	FramePublisher* Publisher = nullptr;
	CreatedCallback OnCreated;
	ClosedCallback OnClosed;
	BrowserEventCallback OnEvent;
	int Width;
	int Height;
	float DeviceScale = 1.0f;
	// View size CEF reports through GetViewRect, in device-independent pixels.
	int DipWidth = 0;
	int DipHeight = 0;
	bool bLoggedPaintFallback = false;

	IMPLEMENT_REFCOUNTING(BrowserClient);
};
} // namespace uxwv
