#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

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
		const std::string& url, int frameRate, bool visible);
	void Destroy(uint32_t id);
	void ShutdownAll();

	bool Has(uint32_t id) const { return Instances.find(id) != Instances.end(); }
	size_t Count() const { return Instances.size(); }

	void Navigate(uint32_t id, const std::string& url);
	void LoadHtml(uint32_t id, const std::string& html, const std::string& base);
	void Reload(uint32_t id);
	void StopLoading(uint32_t id);

	void Resize(uint32_t id, int width, int height);
	void SetVisible(uint32_t id, bool visible);
	void SetFrameRate(uint32_t id, int frameRate);
	void Focus(uint32_t id, bool focused);
	void BeginFrame(uint32_t id);

	void SendInput(uint32_t id, const UxInputEvent& event, const std::string& text);
	void ExecuteScript(uint32_t id, uint32_t requestId, const std::string& script);

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
	};

	void OnBrowserCreated(uint32_t id, CefRefPtr<CefBrowser> browser);
	void OnBrowserClosed(uint32_t id);
	void Emit(uint32_t instanceId, uint32_t type, const std::string& json) const;

	uint64_t AdapterLuid = 0;
	uint32_t OwnerPid = 0;
	EventSender OnEvent;
	std::map<uint32_t, std::unique_ptr<Instance>> Instances;
};
} // namespace uxwv
