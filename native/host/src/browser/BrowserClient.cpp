#include "browser/BrowserClient.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "cef_browser.h"
#include "cef_frame.h"
#include "cef_parser.h"
#include "cef_values.h"

#include "browser/BridgeScript.h"
#include "util/Log.h"

namespace uxwv
{
namespace
{
/** Failure codes handed to the page's onFailure callback. */
const int kErrorBridgeDenied = -2;
const int kErrorBridgeBadRequest = -3;

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

std::string WriteJson(CefRefPtr<CefValue> value)
{
	if (!value)
	{
		return "null";
	}
	return CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}

/** scheme://host[:port] of a URL; local schemes keep only the scheme. */
std::string UrlOrigin(const std::string& url)
{
	if (url.empty())
	{
		return std::string();
	}

	const size_t schemeEnd = url.find("://");
	if (schemeEnd == std::string::npos)
	{
		const size_t colon = url.find(':');
		return colon == std::string::npos ? url : url.substr(0, colon + 1);
	}

	const size_t authorityStart = schemeEnd + 3;
	const size_t pathStart = url.find_first_of("/?#", authorityStart);
	return url.substr(0, pathStart == std::string::npos ? url.size() : pathStart);
}

std::string ToLower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return static_cast<char>(::tolower(c)); });
	return value;
}

/** Trims a trailing slash so 'http://host/' and 'http://host' compare equal. */
std::string NormalizeOrigin(std::string value)
{
	while (!value.empty() && (value.back() == '/' || value.back() == ' '))
	{
		value.pop_back();
	}
	return ToLower(value);
}

bool IsLocalOrigin(const std::string& url)
{
	const std::string origin = ToLower(UrlOrigin(url));
	return origin == "file:" || origin == "file://"
		|| origin == "data:" || origin == "about:" || origin == "blob:";
}
} // namespace

/**
 * Handles window.uxwvQuery. Queries are held open until the game replies, so
 * the JavaScript promise stays pending instead of resolving with a stub.
 */
class BridgeQueryHandler : public CefMessageRouterBrowserSide::Handler
{
public:
	using EventCallback = std::function<void(uint32_t type, const std::string& json)>;
	using OriginFilter = std::function<bool(const std::string& url)>;

	BridgeQueryHandler(EventCallback onEvent, OriginFilter isAllowed)
		: OnEvent(std::move(onEvent)), IsAllowed(std::move(isAllowed))
	{
	}

	bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		int64_t queryId, const CefString& request, bool /*persistent*/,
		CefRefPtr<Callback> callback) override
	{
		const std::string url = frame ? frame->GetURL().ToString() : std::string();
		if (!IsAllowed || !IsAllowed(url))
		{
			UXWV_LOG_WARN("instance bridge denied for origin '%s'",
				UrlOrigin(url).c_str());
			callback->Failure(kErrorBridgeDenied, "BridgeDenied: origin not allowed");
			return true;
		}

		CefRefPtr<CefValue> root = CefParseJSON(request.ToString(), JSON_PARSER_RFC);
		if (!root || root->GetType() != VTYPE_DICTIONARY)
		{
			callback->Failure(kErrorBridgeBadRequest, "BridgeBadRequest: malformed message");
			return true;
		}

		CefRefPtr<CefDictionaryValue> dict = root->GetDictionary();
		const std::string name = dict->GetString("name").ToString();
		if (name.empty())
		{
			callback->Failure(kErrorBridgeBadRequest, "BridgeBadRequest: missing name");
			return true;
		}

		UXWV_LOG_INFO("bridge query %lld '%s' from '%s'", static_cast<long long>(queryId),
			name.c_str(), UrlOrigin(url).c_str());

		CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
		payload->SetInt("request", static_cast<int>(queryId));
		payload->SetString("name", name);
		payload->SetString("payload",
			dict->HasKey("payload") ? WriteJson(dict->GetValue("payload")) : "null");
		CefRefPtr<CefValue> message = CefValue::Create();
		message->SetDictionary(payload);

		Pending[queryId] = callback;
		if (OnEvent)
		{
			OnEvent(UXWV_IPC_EVENT_BRIDGE, CefWriteJSON(message, JSON_WRITER_DEFAULT).ToString());
		}
		return true;
	}

	void OnQueryCanceled(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		int64_t queryId) override
	{
		Pending.erase(queryId);
	}

	bool Resolve(int64_t queryId, bool success, const std::string& result)
	{
		auto it = Pending.find(queryId);
		if (it == Pending.end())
		{
			return false;
		}

		CefRefPtr<Callback> callback = it->second;
		Pending.erase(it);
		if (success)
		{
			callback->Success(result);
		}
		else
		{
			callback->Failure(-1, result);
		}
		return true;
	}

private:
	EventCallback OnEvent;
	OriginFilter IsAllowed;
	std::map<int64_t, CefRefPtr<Callback>> Pending;
};

/** Handles window.uxwvExec: the wrapper reports the result of ExecuteScript. */
class ExecResultHandler : public CefMessageRouterBrowserSide::Handler
{
public:
	using EventCallback = std::function<void(uint32_t type, const std::string& json)>;

	explicit ExecResultHandler(EventCallback onEvent) : OnEvent(std::move(onEvent))
	{
	}

	bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		int64_t /*queryId*/, const CefString& request, bool /*persistent*/,
		CefRefPtr<Callback> callback) override
	{
		CefRefPtr<CefValue> root = CefParseJSON(request.ToString(), JSON_PARSER_RFC);
		if (!root || root->GetType() != VTYPE_DICTIONARY)
		{
			callback->Failure(kErrorBridgeBadRequest, "BridgeBadRequest: malformed result");
			return true;
		}

		CefRefPtr<CefDictionaryValue> dict = root->GetDictionary();
		const int requestId = dict->HasKey("id") ? dict->GetInt("id") : 0;
		const bool bOk = dict->HasKey("ok") && dict->GetBool("ok");
		UXWV_LOG_INFO("exec result for request %d success=%d", requestId, bOk ? 1 : 0);

		CefRefPtr<CefDictionaryValue> result = CefDictionaryValue::Create();
		result->SetInt("request", requestId);
		result->SetBool("success", bOk);
		result->SetString("result",
			dict->HasKey("value") ? WriteJson(dict->GetValue("value")) : "null");
		CefRefPtr<CefValue> message = CefValue::Create();
		message->SetDictionary(result);

		if (OnEvent)
		{
			OnEvent(UXWV_IPC_EXEC_RESULT, CefWriteJSON(message, JSON_WRITER_DEFAULT).ToString());
		}
		callback->Success("{}");
		return true;
	}

private:
	EventCallback OnEvent;
};

BrowserClient::BrowserClient(uint32_t instanceId, FramePublisher* publisher, int width, int height,
	float deviceScale, CreatedCallback onCreated, ClosedCallback onClosed,
	BrowserEventCallback onEvent, std::vector<std::string> allowedOrigins)
	: InstanceId(instanceId)
	, Publisher(publisher)
	, Width(width)
	, Height(height)
	, DeviceScale(deviceScale > 0.0f ? deviceScale : 1.0f)
	, OnCreated(std::move(onCreated))
	, OnClosed(std::move(onClosed))
	, OnEvent(std::move(onEvent))
	, AllowedOrigins(std::move(allowedOrigins))
{
	UpdateDipSize();

	auto emit = [this](uint32_t type, const std::string& json)
	{
		Emit(type, json);
	};
	BridgeHandler = std::make_unique<BridgeQueryHandler>(emit,
		[this](const std::string& url) { return IsOriginAllowed(url); });
	ExecHandler = std::make_unique<ExecResultHandler>(emit);

	CefMessageRouterConfig bridgeConfig;
	bridgeConfig.js_query_function = BridgeQueryFunction();
	bridgeConfig.js_cancel_function = std::string(BridgeQueryFunction()) + "Cancel";
	BridgeRouter = CefMessageRouterBrowserSide::Create(bridgeConfig);
	BridgeRouter->AddHandler(BridgeHandler.get(), false);

	CefMessageRouterConfig execConfig;
	execConfig.js_query_function = ExecQueryFunction();
	execConfig.js_cancel_function = std::string(ExecQueryFunction()) + "Cancel";
	ExecRouter = CefMessageRouterBrowserSide::Create(execConfig);
	ExecRouter->AddHandler(ExecHandler.get(), false);
}

BrowserClient::~BrowserClient()
{
	if (BridgeRouter)
	{
		BridgeRouter->RemoveHandler(BridgeHandler.get());
	}
	if (ExecRouter)
	{
		ExecRouter->RemoveHandler(ExecHandler.get());
	}
}

bool BrowserClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
	CefRefPtr<CefFrame> frame, CefProcessId sourceProcess,
	CefRefPtr<CefProcessMessage> message)
{
	UXWV_LOG_DEBUG("instance %u: process message '%s'", InstanceId,
		message ? message->GetName().ToString().c_str() : "<null>");
	if (BridgeRouter && BridgeRouter->OnProcessMessageReceived(
			browser, frame, sourceProcess, message))
	{
		return true;
	}
	return ExecRouter && ExecRouter->OnProcessMessageReceived(
		browser, frame, sourceProcess, message);
}

bool BrowserClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	CefRefPtr<CefRequest> request, bool userGesture, bool isRedirect)
{
	// Pending queries belong to the old document; drop them on navigation.
	if (BridgeRouter)
	{
		BridgeRouter->OnBeforeBrowse(browser, frame);
	}
	return false;
}

void BrowserClient::ExecuteScript(uint32_t requestId, const std::string& script)
{
	if (!ExecRouter)
	{
		return;
	}

	// CEF 156 has no result visitor, so the script runs inside a wrapper that
	// reports its completion value (or rejection) through window.uxwvExec.
	CefRefPtr<CefValue> encoded = CefValue::Create();
	encoded->SetString(script);
	const std::string literal = CefWriteJSON(encoded, JSON_WRITER_DEFAULT).ToString();

	std::ostringstream wrapped;
	wrapped << "(function(){var id=" << requestId << ";"
		<< "function report(ok,v){var text;"
		<< "try{text=JSON.stringify({id:id,ok:ok,value:v===undefined?null:v});}"
		<< "catch(e){text=JSON.stringify({id:id,ok:false,value:String(e)});}"
		<< "try{window.uxwvExec({request:text,persistent:false,"
		<< "onSuccess:function(){},onFailure:function(){}});}catch(_){}}"
		<< "try{var r=eval(" << literal << ");"
		<< "if(r&&typeof r.then==='function'){"
		<< "r.then(function(v){report(true,v);},"
		<< "function(e){report(false,e&&e.message?e.message:String(e));});}"
		<< "else{report(true,r);}}"
		<< "catch(e){report(false,e&&e.message?e.message:String(e));}})();";

	CefRefPtr<CefBrowser> browser = Browser;
	if (!browser)
	{
		UXWV_LOG_WARN("instance %u: exec %u dropped, browser is gone", InstanceId, requestId);
		return;
	}
	CefRefPtr<CefFrame> frame = browser->GetMainFrame();
	if (frame)
	{
		UXWV_LOG_INFO("instance %u: exec %u (%zu bytes)", InstanceId, requestId,
			wrapped.str().size());
		frame->ExecuteJavaScript(CefString(wrapped.str()), frame->GetURL(), 0);
	}
}

void BrowserClient::EmitEvent(const std::string& name, const std::string& payloadJson)
{
	CefRefPtr<CefBrowser> browser = Browser;
	if (!browser)
	{
		return;
	}

	CefRefPtr<CefValue> payload;
	if (!payloadJson.empty())
	{
		payload = CefParseJSON(payloadJson, JSON_PARSER_RFC);
	}
	if (!payload)
	{
		payload = CefValue::Create();
		payload->SetString(payloadJson);
	}

	CefRefPtr<CefDictionaryValue> detail = CefDictionaryValue::Create();
	detail->SetString("name", name);
	detail->SetValue("payload", payload);
	CefRefPtr<CefValue> root = CefValue::Create();
	root->SetDictionary(detail);
	const std::string script = ";(function(){try{window.dispatchEvent(new CustomEvent('uxwv',"
		"{detail:" + CefWriteJSON(root, JSON_WRITER_DEFAULT).ToString() + "}));}catch(e){}})();";

	CefRefPtr<CefFrame> frame = browser->GetMainFrame();
	if (frame)
	{
		frame->ExecuteJavaScript(CefString(script), frame->GetURL(), 0);
	}
}

bool BrowserClient::ResolveBridge(int64_t queryId, bool success, const std::string& result)
{
	return BridgeHandler && BridgeHandler->Resolve(queryId, success, result);
}

bool BrowserClient::IsOriginAllowed(const std::string& url) const
{
	if (url.empty())
	{
		return false;
	}
	if (IsLocalOrigin(url))
	{
		return true;
	}

	const std::string origin = ToLower(UrlOrigin(url));
	const std::string lowerUrl = ToLower(url);
	for (const std::string& entry : AllowedOrigins)
	{
		const std::string allowed = NormalizeOrigin(entry);
		if (allowed.empty())
		{
			continue;
		}
		if (allowed == "*" || origin == allowed
			|| (lowerUrl.size() > allowed.size()
				&& lowerUrl.compare(0, allowed.size(), allowed) == 0))
		{
			return true;
		}
	}
	return false;
}

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
	Browser = browser;
	UXWV_LOG_INFO("instance %u: browser created (%dx%d)", InstanceId, Width, Height);
	if (OnCreated)
	{
		OnCreated(InstanceId, browser);
	}
}

void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
	UXWV_LOG_INFO("instance %u: browser closed", InstanceId);
	if (BridgeRouter)
	{
		BridgeRouter->OnBeforeClose(browser);
	}
	if (ExecRouter)
	{
		ExecRouter->OnBeforeClose(browser);
	}
	Browser = nullptr;

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

bool BrowserClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
	const CefString& message, const CefString& source, int line)
{
	// Warnings and errors always land in the host log; informational chatter
	// would be too noisy at the default level.
	if (level >= LOGSEVERITY_WARNING)
	{
		UXWV_LOG_INFO("instance %u console: %s (%s:%d)", InstanceId,
			message.ToString().c_str(), source.ToString().c_str(), line);
	}
	else
	{
		UXWV_LOG_DEBUG("instance %u console: %s (%s:%d)", InstanceId,
			message.ToString().c_str(), source.ToString().c_str(), line);
	}
	return false;
}

void BrowserClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
	TerminationStatus status, int errorCode, const CefString& errorString)
{
	if (BridgeRouter)
	{
		BridgeRouter->OnRenderProcessTerminated(browser);
	}
	if (ExecRouter)
	{
		ExecRouter->OnRenderProcessTerminated(browser);
	}

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
