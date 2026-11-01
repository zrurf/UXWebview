#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cef_browser.h"

#include "browser/BrowserClient.h"
#include "frame/FramePublisher.h"
#include "uxwebview_ipc.h"

namespace uxwv
{
// Owns one browser + frame publisher per webview instance. Every method must
// be called on the CEF UI thread; IPC handlers post tasks before calling in.
class BrowserManager
{
public:
	using EventSender = std::function<void(uint32_t instanceId, uint32_t type, const std::string& json)>;

	BrowserManager(uint64_t adapterLuid, uint32_t ownerPid, EventSender onEvent);
	~BrowserManager();

	bool Create(uint32_t id, const std::string& sharedMemoryName, int width, int height,
		const std::string& url, int frameRate, bool visible, float deviceScale,
		const std::vector<std::string>& allowedOrigins, bool overlay, uint64_t parentHandle,
		int posX, int posY);
	void Destroy(uint32_t id);
	void ShutdownAll();

	bool Has(uint32_t id) const { return Instances.find(id) != Instances.end(); }
	bool IsOverlay(uint32_t id) const;
	size_t Count() const { return Instances.size(); }

	void Navigate(uint32_t id, const std::string& url);
	void LoadHtml(uint32_t id, const std::string& html, const std::string& base);
	void Reload(uint32_t id);
	void StopLoading(uint32_t id);

	void Resize(uint32_t id, int width, int height, float deviceScale);
	/**
	 * Overlay only: moves the native popup window to the given rectangle,
	 * expressed in the game window's client coordinates.
	 */
	void SetBounds(uint32_t id, int x, int y, int width, int height, float deviceScale);
	void SetVisible(uint32_t id, bool visible);
	void SetFrameRate(uint32_t id, int frameRate);
	void Focus(uint32_t id, bool focused);
	void BeginFrame(uint32_t id);

	void SendInput(uint32_t id, const UxInputEvent& event, const std::string& text);
	void ExecuteScript(uint32_t id, uint32_t requestId, const std::string& script);
	void EmitToWeb(uint32_t id, const std::string& event, const std::string& payloadJson);
	void ResolveBridge(uint32_t id, int64_t queryId, bool success, const std::string& result);

private:
	struct Instance
	{
		uint32_t Id = 0;
		CefRefPtr<BrowserClient> Client;
		CefRefPtr<CefBrowser> Browser;
		std::unique_ptr<FramePublisher> Publisher;
		std::string PendingUrl;
		std::string SharedMemoryName;
		bool bVisible = true;
		bool bClosing = false;
		/** Overlay instances own a native top-level popup instead of a frame pool. */
		bool bOverlay = false;
		/** Game window the overlay follows; also the origin for PosX/PosY. */
		uint64_t ParentHandle = 0;
		/** Host-owned carrier window that Chromium's child window lives in. */
		uint64_t CarrierHandle = 0;
		/** Overlay position in the game window's client coordinates. */
		int PosX = 0;
		int PosY = 0;
		/** Overlay size in physical pixels. */
		int Width = 0;
		int Height = 0;
		/** CSS-to-pixel scale the widget asked for; drives the overlay page zoom. */
		float DeviceScale = 1.0f;
		/** Mouse buttons currently held, as CEF EVENTFLAG_* bits (drag selection). */
		uint32_t PressedButtons = 0;
	};

	void OnBrowserCreated(uint32_t id, CefRefPtr<CefBrowser> browser);
	void OnBrowserClosed(uint32_t id);
	void MoveOverlayWindow(Instance& instance, int x, int y, int width, int height);
	/** Keeps the overlay page zoom in step with the widget's device scale. */
	void ApplyOverlayPageZoom(Instance& instance);
	void Emit(uint32_t instanceId, uint32_t type, const std::string& json) const;

	uint64_t AdapterLuid = 0;
	uint32_t OwnerPid = 0;
	EventSender OnEvent;
	std::map<uint32_t, std::unique_ptr<Instance>> Instances;
};
} // namespace uxwv
