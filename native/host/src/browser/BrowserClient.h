#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cef_client.h"
#include "cef_display_handler.h"
#include "cef_life_span_handler.h"
#include "cef_load_handler.h"
#include "cef_render_handler.h"
#include "cef_request_handler.h"
#include "wrapper/cef_message_router.h"

#include "frame/FramePublisher.h"
#include "uxwebview_ipc.h"

namespace uxwv
{
// Routes events to HostApp, which turns them into IPC messages.
using BrowserEventCallback =
	std::function<void(uint32_t instanceId, uint32_t eventType, const std::string& json)>;

class BridgeQueryHandler;
class ExecResultHandler;

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
		BrowserEventCallback onEvent, std::vector<std::string> allowedOrigins = {});

	~BrowserClient() override;

	// CefClient
	CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
	CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
	CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
	CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
		CefRefPtr<CefFrame> frame, CefProcessId sourceProcess,
		CefRefPtr<CefProcessMessage> message) override;

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
	bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
		const CefString& message, const CefString& source, int line) override;

	// CefRequestHandler
	bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		CefRefPtr<CefRequest> request, bool userGesture, bool isRedirect) override;
	void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
		TerminationStatus status, int errorCode, const CefString& errorString) override;

	/** Size is in device pixels; CEF lays out at Size/DeviceScale device-independent pixels. */
	void SetViewSize(int width, int height, float deviceScale);
	/**
	 * Page zoom that this browser must keep. Windowed browsers lay out at the
	 * monitor DPI, so an overlay browser zooms the page to reach the scale the
	 * widget asked for, which keeps its CSS pixels the same size as a texture view.
	 */
	void SetPageZoom(float zoomFactor);
	float GetPageZoom() const { return PageZoom; }
	int GetWidth() const { return Width; }
	int GetHeight() const { return Height; }
	float GetDeviceScale() const { return DeviceScale; }
	uint32_t GetInstanceId() const { return InstanceId; }

	/** Runs a script and reports {success, result} through UXWV_IPC_EXEC_RESULT. */
	void ExecuteScript(uint32_t requestId, const std::string& script);
	/** Delivers a game event to the page as a 'uxwv' CustomEvent. */
	void EmitEvent(const std::string& name, const std::string& payloadJson);
	/** Answers a pending page query; false when the query is already gone. */
	bool ResolveBridge(int64_t queryId, bool success, const std::string& result);
	/** True when the page may use the bridge (local schemes or allowlisted). */
	bool IsOriginAllowed(const std::string& url) const;

private:
	void Emit(uint32_t eventType, const std::string& json);
	void UpdateDipSize();
	void ApplyPageZoom();

	uint32_t InstanceId;
	FramePublisher* Publisher = nullptr;
	/** Set between OnAfterCreated and OnBeforeClose; used by script helpers. */
	CefRefPtr<CefBrowser> Browser;
	CreatedCallback OnCreated;
	ClosedCallback OnClosed;
	BrowserEventCallback OnEvent;
	int Width;
	int Height;
	float DeviceScale = 1.0f;
	/** Requested page zoom; applied once the browser exists and reasserted on load. */
	float PageZoom = 1.0f;
	/** Last zoom level handed to Chromium, so repeated loads stay quiet. */
	double AppliedZoomLevel = 0.0;
	// View size CEF reports through GetViewRect, in device-independent pixels.
	int DipWidth = 0;
	int DipHeight = 0;
	bool bLoggedPaintFallback = false;

	std::vector<std::string> AllowedOrigins;
	/** uxwvQuery: page -> game queries, origin checked and held open for a reply. */
	CefRefPtr<CefMessageRouterBrowserSide> BridgeRouter;
	/** uxwvExec: results of ExecuteScript, sent by the wrapper the host runs. */
	CefRefPtr<CefMessageRouterBrowserSide> ExecRouter;
	std::unique_ptr<BridgeQueryHandler> BridgeHandler;
	std::unique_ptr<ExecResultHandler> ExecHandler;

	IMPLEMENT_REFCOUNTING(BrowserClient);
};
} // namespace uxwv
