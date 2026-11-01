#include "UXWebviewWebView2Provider.h"

#if PLATFORM_WINDOWS && defined(UXWEBVIEW_WITH_WEBVIEW2) && UXWEBVIEW_WITH_WEBVIEW2

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/FileManager.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Provider/UXWebviewProviderRegistry.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UXWebviewSettings.h"
#include "Widgets/SWindow.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <objidl.h>
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

using Microsoft::WRL::ComPtr;

namespace
{
/** WebView2 has no maximum, but capture + decode + upload cost grows linearly. */
constexpr int32 kMaxViewSize = 4096;
/** CapturePreview is a CPU readback, so the provider does not claim 60 fps. */
constexpr int32 kMaxFrameRate = 30;
constexpr double kBridgeTimeoutSeconds = 30.0;
constexpr double kScriptTimeoutSeconds = 30.0;
/** A capture that never reported back must not stall the ticker forever. */
constexpr double kCaptureStallSeconds = 2.0;
/** CapturePreview reports this until the page has produced its first frame. */
constexpr HRESULT kCaptureNotReady = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);

WCHAR* NativeWindowClass = const_cast<WCHAR*>(L"STATIC");

FString TakeComString(LPWSTR Value)
{
	if (!Value)
	{
		return FString();
	}
	FString Result(Value);
	CoTaskMemFree(Value);
	return Result;
}

/** Quotes and escapes a string so it can be embedded in a script literal. */
FString SerializeJsonString(const FString& Value)
{
	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	Writer->WriteValue(Value);
	Writer->Close();
	return Out;
}

/** Bridge messages carry the payload as embedded JSON text; see docs/script-bridge.md. */
FString BuildWebMessage(const FString& Kind, const FString& Name, const FString& PayloadJson,
	int32 Id)
{
	const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("uxwv"), 1.0);
	Object->SetStringField(TEXT("kind"), Kind);
	if (Id > 0)
	{
		Object->SetNumberField(TEXT("id"), static_cast<double>(Id));
	}
	Object->SetStringField(TEXT("name"), Name);
	Object->SetStringField(TEXT("payloadJson"), PayloadJson);

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Out;
}

FString UrlOrigin(const FString& Url)
{
	if (Url.IsEmpty())
	{
		return FString();
	}

	const int32 SchemeEnd = Url.Find(TEXT("://"));
	if (SchemeEnd == INDEX_NONE)
	{
		const int32 Colon = Url.Find(TEXT(":"));
		return Colon == INDEX_NONE ? Url : Url.Left(Colon + 1);
	}

	const int32 AuthorityStart = SchemeEnd + 3;
	int32 PathStart = INDEX_NONE;
	for (int32 Index = AuthorityStart; Index < Url.Len(); ++Index)
	{
		if (Url[Index] == TEXT('/') || Url[Index] == TEXT('?') || Url[Index] == TEXT('#'))
		{
			PathStart = Index;
			break;
		}
	}
	return Url.Left(PathStart == INDEX_NONE ? Url.Len() : PathStart);
}

FString NormalizeOrigin(FString Value)
{
	Value.TrimStartAndEndInline();
	while (Value.Len() > 0 && Value[Value.Len() - 1] == TEXT('/'))
	{
		Value.LeftChopInline(1, EAllowShrinking::No);
	}
	return Value.ToLower();
}

bool IsLocalOrigin(const FString& Url)
{
	const FString Origin = UrlOrigin(Url).ToLower();
	return Origin == TEXT("file:") || Origin == TEXT("file://")
		|| Origin == TEXT("data:") || Origin == TEXT("about:") || Origin == TEXT("blob:");
}

/** Mirrors the CEF host policy: local schemes pass, remote origins need an entry. */
bool IsOriginAllowed(const FString& Url)
{
	if (Url.IsEmpty())
	{
		return false;
	}
	if (IsLocalOrigin(Url))
	{
		return true;
	}

	const FString Origin = UrlOrigin(Url).ToLower();
	const FString LowerUrl = Url.ToLower();
	for (const FString& Entry : UUXWebviewSettings::Get().BridgeAllowedOrigins)
	{
		const FString Allowed = NormalizeOrigin(Entry);
		if (Allowed.IsEmpty())
		{
			continue;
		}
		if (Allowed == TEXT("*") || Origin == Allowed
			|| (LowerUrl.Len() > Allowed.Len() && LowerUrl.StartsWith(Allowed, ESearchCase::CaseSensitive)))
		{
			return true;
		}
	}
	return false;
}

/** Adds a <base> element so relative URLs resolve like the CEF provider's BaseUrl. */
FString InjectBaseUrl(const FString& Html, const FString& BaseUrl)
{
	if (BaseUrl.IsEmpty())
	{
		return Html;
	}

	FString SafeBase = BaseUrl;
	SafeBase.ReplaceInline(TEXT("\""), TEXT("&quot;"));
	const FString Tag = FString::Printf(TEXT("<base href=\"%s\">"), *SafeBase);

	const int32 HeadIndex = Html.Find(TEXT("<head"), ESearchCase::IgnoreCase);
	if (HeadIndex != INDEX_NONE)
	{
		const int32 HeadEnd = Html.Find(TEXT(">"), ESearchCase::IgnoreCase, ESearchDir::FromStart,
			HeadIndex);
		if (HeadEnd != INDEX_NONE)
		{
			return Html.Left(HeadEnd + 1) + Tag + Html.Mid(HeadEnd + 1);
		}
	}
	return Tag + Html;
}

bool IsExtendedVirtualKey(int32 Vk)
{
	switch (Vk)
	{
	case VK_LEFT:
	case VK_UP:
	case VK_RIGHT:
	case VK_DOWN:
	case VK_HOME:
	case VK_END:
	case VK_PRIOR:
	case VK_NEXT:
	case VK_INSERT:
	case VK_DELETE:
	case VK_LWIN:
	case VK_RWIN:
	case VK_APPS:
	case VK_DIVIDE:
	case VK_NUMLOCK:
		return true;
	default:
		return false;
	}
}

LPARAM MakeKeyLParam(int32 Vk, bool bKeyUp)
{
	const UINT ScanCode = MapVirtualKeyW(static_cast<UINT>(Vk), MAPVK_VK_TO_VSC);
	LPARAM LParam = 1 | (static_cast<LPARAM>(ScanCode & 0xFF) << 16);
	if (IsExtendedVirtualKey(Vk))
	{
		LParam |= (1 << 24);
	}
	if (bKeyUp)
	{
		LParam |= (1 << 30) | (1 << 31);
	}
	return LParam;
}

WPARAM MouseModifiers(const FUXWebviewInputEvent& Event)
{
	WPARAM Modifiers = 0;
	if (Event.bShift)
	{
		Modifiers |= MK_SHIFT;
	}
	if (Event.bControl)
	{
		Modifiers |= MK_CONTROL;
	}
	return Modifiers;
}

FString ResolveUserDataFolder()
{
	// A stable folder keeps cookies, cache and local storage across runs; the
	// settings advertise offline cache support on that basis.
	const FString Folder = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("UXWebview/WebView2"));
	IFileManager::Get().MakeDirectory(*Folder, true);
	return Folder;
}

/**
 * All WebView2 state lives here so asynchronous COM handlers can outlive the
 * provider without touching freed memory. bShutdown is checked before every
 * callback that reaches the game.
 */
struct FUXWebviewBridgeRequest
{
	int32 PageRequestId = 0;
	double Deadline = 0.0;
	bool bResolved = false;
};

} // namespace

struct FUXWebviewWebView2State
{
	ComPtr<ICoreWebView2Environment> Environment;
	ComPtr<ICoreWebView2Controller> Controller;
	ComPtr<ICoreWebView2> WebView;

	EventRegistrationToken NavigationCompletedToken{};
	EventRegistrationToken ProcessFailedToken{};
	EventRegistrationToken WebMessageToken{};

	FUXWebviewWebView2Provider* Owner = nullptr;
	IUXWebviewViewClient* Client = nullptr;

	HWND HostWindow = nullptr;
	HWND InputWindow = nullptr;

	bool bShutdown = false;
	bool bInitialized = false;
	bool bReady = false;
	bool bOverlayMode = false;
	bool bVisible = true;
	bool bComInitialized = false;
	bool bCaptureInFlight = false;
	bool bCaptureFailedLogged = false;
	bool bInputWindowMissingLogged = false;
	bool bFirstFrame = true;

	FIntPoint ViewSize = FIntPoint(1280, 720);
	FIntPoint OverlayPosition = FIntPoint::ZeroValue;
	FIntPoint OverlaySize = FIntPoint::ZeroValue;
	float DpiScale = 1.0f;
	float OverlayDpiScale = 1.0f;
	int32 FrameRate = kMaxFrameRate;
	double LastCaptureSeconds = 0.0;
	FString CurrentUrl;
	FString UserAgent;

	UTexture2D* Texture = nullptr;
	int32 TextureWidth = 0;
	int32 TextureHeight = 0;
	uint64 FrameCount = 0;
	int32 MouseButtons = 0;

	TMap<uint32, TPair<FUXWebviewScriptCallback, double>> PendingScripts;
	uint32 NextScriptRequestId = 1;

	TMap<int32, TSharedPtr<FUXWebviewBridgeRequest>> PendingBridge;
	int32 NextBridgeRequestId = 1;
};

namespace
{
/**
 * Page-side bridge. It is installed in every document before page scripts run,
 * so window.uxwv exists even for pages that load synchronously.
 */
const TCHAR* kBootstrapScript = TEXT(R"UXWV(
(function () {
  if (window.uxwv) { return; }
  var pending = {};
  var nextId = 1;
  var timeoutMs = 30000;
  function parsePayload(text) {
    if (text === undefined || text === null || text === '') { return null; }
    try { return JSON.parse(text); } catch (e) { return text; }
  }
  function post(kind, name, data) {
    var id = nextId++;
    var payloadJson;
    try { payloadJson = JSON.stringify(data === undefined ? null : data); }
    catch (e) { payloadJson = JSON.stringify(String(data)); }
    return new Promise(function (resolve, reject) {
      var timer = setTimeout(function () {
        delete pending[id];
        reject(new Error('timeout'));
      }, timeoutMs);
      pending[id] = function (ok, payloadText) {
        clearTimeout(timer);
        if (ok) { resolve(parsePayload(payloadText)); }
        else { reject(new Error(payloadText || 'bridge failure')); }
      };
      try {
        window.chrome.webview.postMessage(JSON.stringify({
          uxwv: 1, kind: kind, id: id, name: String(name), payloadJson: payloadJson
        }));
      } catch (e) {
        delete pending[id];
        clearTimeout(timer);
        reject(new Error('the view is not available'));
      }
    });
  }
  window.uxwv = {
    call: function (name, data) { return post('call', name, data); },
    emit: function (name, data) { return post('emit', name, data); }
  };
  window.__uxwvReply = function (id, ok, payloadText) {
    var entry = pending[id];
    if (!entry) { return; }
    delete pending[id];
    entry(!!ok, payloadText === undefined ? null : String(payloadText));
  };
  window.chrome.webview.addEventListener('message', function (event) {
    var message;
    try { message = JSON.parse(event.data); } catch (e) { return; }
    if (!message || message.uxwv !== 1 || message.kind !== 'event') { return; }
    window.dispatchEvent(new CustomEvent('uxwv', {
      detail: { name: String(message.name), payload: parsePayload(message.payloadJson) }
    }));
  });
  // Texture mode has no native IME, so composition is rendered by editing the
  // focused element directly. Overlay mode never reaches this helper.
  function clearComposition(el) {
    var comp = el.__uxwvComp;
    el.__uxwvComp = null;
    if (!comp) { return null; }
    if (comp.input) {
      var value = el.value;
      el.value = value.slice(0, comp.start) + value.slice(comp.start + comp.text.length);
      el.selectionStart = el.selectionEnd = comp.start;
      return comp.start;
    }
    if (comp.node && comp.node.parentNode) {
      var range = document.createRange();
      range.setStartBefore(comp.node);
      range.collapse(true);
      var selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      comp.node.parentNode.removeChild(comp.node);
    }
    return null;
  }
  window.__uxwvIme = function (cmd, text) {
    var el = document.activeElement;
    if (!el) { return; }
    if (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA') {
      var start = clearComposition(el);
      if (start === null || start === undefined) {
        start = typeof el.selectionStart === 'number' ? el.selectionStart : el.value.length;
      }
      if (cmd === 'cancel') { return; }
      el.value = el.value.slice(0, start) + text + el.value.slice(start);
      el.selectionStart = el.selectionEnd = start + text.length;
      if (cmd === 'composition') {
        el.__uxwvComp = { input: true, start: start, text: text };
      }
      return;
    }
    if (!el.isContentEditable) { return; }
    var comp = el.__uxwvComp;
    if (cmd === 'cancel') { clearComposition(el); return; }
    if (comp && comp.node && comp.node.parentNode) {
      comp.node.data = text;
      var range = document.createRange();
      range.setStartAfter(comp.node);
      range.collapse(true);
      var selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      if (cmd === 'commit') { el.__uxwvComp = null; }
      return;
    }
    var selection = window.getSelection();
    if (!selection || selection.rangeCount === 0) { return; }
    var live = selection.getRangeAt(0);
    live.deleteContents();
    var node = document.createTextNode(text);
    live.insertNode(node);
    var caret = document.createRange();
    caret.setStartAfter(node);
    caret.collapse(true);
    selection.removeAllRanges();
    selection.addRange(caret);
    if (cmd === 'composition') { el.__uxwvComp = { node: node }; }
  };
})();
)UXWV");

FUXWebviewProviderInfo MakeProviderInfo()
{
	FUXWebviewProviderInfo Info;
	Info.Name = TEXT("WebView2");
	Info.DisplayName = TEXT("WebView2 (Microsoft Edge)");
	Info.Platform = TEXT("Win64");

	LPWSTR Version = nullptr;
	const HRESULT Result = GetAvailableCoreWebView2BrowserVersionString(nullptr, &Version);
	if (SUCCEEDED(Result) && Version)
	{
		Info.Version = TakeComString(Version);
		Info.bAvailable = true;
	}
	else
	{
		Info.UnavailableReason = TEXT("the Microsoft Edge WebView2 Runtime is not installed");
	}
	return Info;
}

class FUXWebviewWebView2Factory final : public IUXWebviewProviderFactory
{
public:
	virtual FUXWebviewProviderInfo GetInfo() const override
	{
		return MakeProviderInfo();
	}

	virtual TUniquePtr<IUXWebviewProvider> Create() override
	{
		return MakeUnique<FUXWebviewWebView2Provider>();
	}
};

} // namespace

FUXWebviewWebView2Provider::~FUXWebviewWebView2Provider()
{
	Shutdown();
}

FUXWebviewProviderInfo FUXWebviewWebView2Provider::GetInfo() const
{
	return MakeProviderInfo();
}

FUXWebviewCapabilities FUXWebviewWebView2Provider::GetCapabilities() const
{
	FUXWebviewCapabilities Capabilities;
	Capabilities.bCpuPixelBuffer = true;
	Capabilities.bOverlayMode = true;
	Capabilities.bOfflineCache = true;
	Capabilities.bDevTools = UUXWebviewSettings::Get().bEnableDevTools;
	Capabilities.MaxFrameRate = kMaxFrameRate;
	Capabilities.MaxViewportSize = FIntPoint(kMaxViewSize, kMaxViewSize);
	return Capabilities;
}

FUXWebviewResult FUXWebviewWebView2Provider::Initialize(
	const FUXWebviewInitParams& InitParams, IUXWebviewViewClient* InClient)
{
	if (State.IsValid())
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidState,
			TEXT("webview2 provider is already initialized"));
	}
	if (!InClient)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("webview2 provider needs a view client"));
	}

	State = MakeShared<FUXWebviewWebView2State>();
	State->Owner = this;
	State->Client = InClient;
	State->bInitialized = true;
	State->DpiScale = InitParams.DpiScale > 0.0f
		? FMath::Clamp(InitParams.DpiScale, 0.25f, 4.0f) : 1.0f;
	State->ViewSize = FIntPoint(
		FMath::Clamp(InitParams.ViewportSize.X, 64, kMaxViewSize),
		FMath::Clamp(InitParams.ViewportSize.Y, 64, kMaxViewSize));
	State->FrameRate = FMath::Clamp(InitParams.TargetFrameRate, 0, kMaxFrameRate);
	State->bVisible = true;
	State->CurrentUrl = InitParams.InitialUrl.IsEmpty()
		? FString(TEXT("about:blank")) : InitParams.InitialUrl;
	State->UserAgent = InitParams.UserAgent;

	// Overlay needs a real parent window; without one the texture pipeline is a
	// better answer than silently dropping the view.
	uint64 ParentHandle = 0;
	if (InitParams.bOverlay)
	{
		ParentHandle = ResolveGameWindowHandle();
		if (ParentHandle == 0)
		{
			UE_LOG(LogUXWebviewWebView2, Warning,
				TEXT("overlay was requested but no game window is available; using texture mode"));
		}
	}
	State->bOverlayMode = ParentHandle != 0;

	CreateHostWindow(State->bOverlayMode, ParentHandle);
	if (!State->HostWindow)
	{
		State.Reset();
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			TEXT("webview2 provider could not create its host window"));
	}

	// WebView2 requires an STA thread with a message pump. UE pumps messages on
	// the game thread, but another plugin may have initialized it as MTA.
	const HRESULT ComResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (ComResult == RPC_E_CHANGED_MODE)
	{
		DestroyHostWindow();
		State.Reset();
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
			TEXT("the game thread already uses a multithreaded COM apartment; "
				"WebView2 needs it to be single-threaded"));
	}
	State->bComInitialized = SUCCEEDED(ComResult);

	const FUXWebviewResult StartResult = StartEnvironment();
	if (!StartResult.bSuccess)
	{
		Shutdown();
		return StartResult;
	}

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FUXWebviewWebView2Provider::Tick));

	UE_LOG(LogUXWebviewWebView2, Log,
		TEXT("webview2 view created: %s %dx%d scale=%.2f fps=%d url='%s'"),
		State->bOverlayMode ? TEXT("overlay") : TEXT("texture"),
		State->ViewSize.X, State->ViewSize.Y, State->DpiScale, State->FrameRate,
		*State->CurrentUrl);
	return FUXWebviewResult::Ok();
}

uint64 FUXWebviewWebView2Provider::ResolveGameWindowHandle()
{
	if (!GEngine || !GEngine->GameViewport)
	{
		return 0;
	}

	const TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow();
	if (!Window.IsValid())
	{
		return 0;
	}

	const TSharedPtr<FGenericWindow> Native = Window->GetNativeWindow();
	if (!Native.IsValid())
	{
		return 0;
	}
	return static_cast<uint64>(reinterpret_cast<uintptr_t>(Native->GetOSWindowHandle()));
}

void FUXWebviewWebView2Provider::CreateHostWindow(bool bOverlay, uint64 ParentHandle)
{
	// The texture path keeps the window off-screen; only CapturePreview reads it.
	// The overlay path is a child of the game window and is shown on first bounds.
	const DWORD Style = bOverlay
		? (WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS)
		: (WS_POPUP);
	const DWORD ExStyle = bOverlay ? 0 : (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
	HWND Parent = bOverlay ? reinterpret_cast<HWND>(ParentHandle) : nullptr;
	const int32 X = bOverlay ? 0 : -32000;
	const int32 Y = bOverlay ? 0 : -32000;

	State->HostWindow = CreateWindowExW(ExStyle, NativeWindowClass, L"UXWebviewWebView2Host",
		Style, X, Y, State->ViewSize.X, State->ViewSize.Y, Parent, nullptr,
		GetModuleHandleW(nullptr), nullptr);
	if (!State->HostWindow)
	{
		UE_LOG(LogUXWebviewWebView2, Error,
			TEXT("CreateWindowExW failed with error %u"), ::GetLastError());
		return;
	}

	ShowWindow(State->HostWindow, bOverlay ? SW_HIDE : SW_SHOWNOACTIVATE);
}

void FUXWebviewWebView2Provider::DestroyHostWindow()
{
	if (State.IsValid() && State->HostWindow && IsWindow(State->HostWindow))
	{
		DestroyWindow(State->HostWindow);
	}
	if (State.IsValid())
	{
		State->HostWindow = nullptr;
		State->InputWindow = nullptr;
	}
}

void FUXWebviewWebView2Provider::ApplyGeometry()
{
	if (!State.IsValid() || !State->Controller)
	{
		return;
	}

	RECT Bounds = {};
	if (State->bOverlayMode)
	{
		const FIntPoint Size = State->OverlaySize != FIntPoint::ZeroValue
			? State->OverlaySize : State->ViewSize;
		Bounds.right = Size.X;
		Bounds.bottom = Size.Y;
		if (State->HostWindow && State->OverlaySize != FIntPoint::ZeroValue)
		{
			SetWindowPos(State->HostWindow, HWND_TOP, State->OverlayPosition.X,
				State->OverlayPosition.Y, Size.X, Size.Y,
				SWP_NOACTIVATE | SWP_SHOWWINDOW);
		}
	}
	else
	{
		if (State->HostWindow)
		{
			MoveWindow(State->HostWindow, -32000, -32000, State->ViewSize.X,
				State->ViewSize.Y, 0);
		}
		Bounds.right = State->ViewSize.X;
		Bounds.bottom = State->ViewSize.Y;
	}

	State->Controller->put_Bounds(Bounds);

	// RasterizationScale ties CSS pixels to DIPs; without it Chromium would use
	// the monitor scale and clicks would drift on scaled displays.
	ComPtr<ICoreWebView2Controller3> Controller3;
	if (SUCCEEDED(State->Controller.As(&Controller3)) && Controller3)
	{
		const float Scale = State->bOverlayMode ? State->OverlayDpiScale : State->DpiScale;
		Controller3->put_ShouldDetectMonitorScaleChanges(0);
		Controller3->put_RasterizationScale(static_cast<double>(Scale));
	}
}

void FUXWebviewWebView2Provider::ApplyVisibility()
{
	if (!State.IsValid() || !State->Controller)
	{
		return;
	}
	State->Controller->put_IsVisible(State->bVisible ? 1 : 0);

	if (State->HostWindow)
	{
		// The texture host sits off-screen, but Chromium drops input sent to a
		// hidden window, so it stays shown while the view is visible.
		const bool bShow = State->bVisible
			&& (!State->bOverlayMode || State->OverlaySize != FIntPoint::ZeroValue);
		ShowWindow(State->HostWindow, bShow ? SW_SHOWNOACTIVATE : SW_HIDE);
	}
}

FUXWebviewResult FUXWebviewWebView2Provider::StartEnvironment()
{
	const FString UserDataFolder = ResolveUserDataFolder();
	const TSharedPtr<FUXWebviewWebView2State> Captured = State;

	const HRESULT Hr = CreateCoreWebView2EnvironmentWithOptions(nullptr,
		UserDataFolder.IsEmpty() ? nullptr : *UserDataFolder, nullptr,
		Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[Captured](HRESULT Error, ICoreWebView2Environment* Environment) -> HRESULT
			{
				if (Captured->bShutdown || !Captured->Owner)
				{
					return S_OK;
				}
				if (FAILED(Error) || !Environment)
				{
					Captured->Owner->OnInitializationFailed(FString::Printf(
						TEXT("WebView2 environment creation failed (0x%08X)"),
						static_cast<uint32>(Error)));
					return S_OK;
				}

				Captured->Environment = Environment;
				const HRESULT ControllerResult = Environment->CreateCoreWebView2Controller(
					Captured->HostWindow,
					Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
						[Captured](HRESULT ControllerError,
							ICoreWebView2Controller* Controller) -> HRESULT
						{
							if (Captured->bShutdown || !Captured->Owner)
							{
								return S_OK;
							}
							if (FAILED(ControllerError) || !Controller)
							{
								Captured->Owner->OnInitializationFailed(FString::Printf(
									TEXT("WebView2 controller creation failed (0x%08X)"),
									static_cast<uint32>(ControllerError)));
								return S_OK;
							}

							Captured->Controller = Controller;
							ComPtr<ICoreWebView2> WebView;
							if (FAILED(Controller->get_CoreWebView2(&WebView)) || !WebView)
							{
								Captured->Owner->OnInitializationFailed(
									TEXT("WebView2 did not expose its core view"));
								return S_OK;
							}
							Captured->WebView = WebView;
							Captured->Owner->ConfigureWebView();
							return S_OK;
						}).Get());

				if (FAILED(ControllerResult) && Captured->Owner)
				{
					Captured->Owner->OnInitializationFailed(FString::Printf(
						TEXT("WebView2 controller request failed (0x%08X)"),
						static_cast<uint32>(ControllerResult)));
				}
				return S_OK;
			}).Get());

	if (FAILED(Hr))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			FString::Printf(TEXT("WebView2 environment request failed (0x%08X)"),
				static_cast<uint32>(Hr)));
	}
	return FUXWebviewResult::Ok();
}

void FUXWebviewWebView2Provider::ConfigureWebView()
{
	if (!State.IsValid() || !State->WebView || !State->Controller)
	{
		return;
	}

	ComPtr<ICoreWebView2Settings> Settings;
	if (SUCCEEDED(State->WebView->get_Settings(&Settings)) && Settings)
	{
		const bool bDevTools = UUXWebviewSettings::Get().bEnableDevTools;
		Settings->put_IsScriptEnabled(1);
		Settings->put_AreDefaultContextMenusEnabled(1);
		Settings->put_IsStatusBarEnabled(0);
		Settings->put_AreDevToolsEnabled(bDevTools ? 1 : 0);
		Settings->put_IsBuiltInErrorPageEnabled(0);

		if (!State->UserAgent.IsEmpty())
		{
			ComPtr<ICoreWebView2Settings2> Settings2;
			if (SUCCEEDED(Settings.As(&Settings2)) && Settings2)
			{
				Settings2->put_UserAgent(*State->UserAgent);
			}
		}
	}

	const TSharedPtr<FUXWebviewWebView2State> Captured = State;

	State->WebView->add_NavigationCompleted(
		Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
			[Captured](ICoreWebView2* Sender,
				ICoreWebView2NavigationCompletedEventArgs* Args) -> HRESULT
			{
				if (Captured->bShutdown || !Captured->Owner)
				{
					return S_OK;
				}
				BOOL bSucceeded = 0;
				COREWEBVIEW2_WEB_ERROR_STATUS Status =
					COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
				if (Args)
				{
					Args->get_IsSuccess(&bSucceeded);
					Args->get_WebErrorStatus(&Status);
				}
				LPWSTR Uri = nullptr;
				if (Sender)
				{
					Sender->get_Source(&Uri);
				}
				Captured->Owner->OnNavigationCompleted(bSucceeded != 0,
					static_cast<int32>(Status), TakeComString(Uri));
				return S_OK;
			}).Get(),
		&State->NavigationCompletedToken);

	State->WebView->add_ProcessFailed(
		Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
			[Captured](ICoreWebView2* /*Sender*/,
				ICoreWebView2ProcessFailedEventArgs* Args) -> HRESULT
			{
				if (Captured->bShutdown || !Captured->Owner)
				{
					return S_OK;
				}
				COREWEBVIEW2_PROCESS_FAILED_KIND Kind =
					COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
				if (Args)
				{
					Args->get_ProcessFailedKind(&Kind);
				}
				Captured->Owner->OnProcessFailed(static_cast<int32>(Kind));
				return S_OK;
			}).Get(),
		&State->ProcessFailedToken);

	State->WebView->add_WebMessageReceived(
		Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
			[Captured](ICoreWebView2* /*Sender*/,
				ICoreWebView2WebMessageReceivedEventArgs* Args) -> HRESULT
			{
				if (Captured->bShutdown || !Captured->Owner || !Args)
				{
					return S_OK;
				}
				LPWSTR Message = nullptr;
				LPWSTR Source = nullptr;
				Args->TryGetWebMessageAsString(&Message);
				Args->get_Source(&Source);
				Captured->Owner->OnWebMessage(TakeComString(Message),
					TakeComString(Source));
				return S_OK;
			}).Get(),
		&State->WebMessageToken);

	const TSharedPtr<FUXWebviewWebView2State> BootstrapState = State;
	// AddScriptToExecuteOnDocumentCreated completes asynchronously. Navigation
	// waits for it so the first document always has window.uxwv installed.
	State->WebView->AddScriptToExecuteOnDocumentCreated(kBootstrapScript,
		Microsoft::WRL::Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
			[BootstrapState](HRESULT Error, LPCWSTR /*ScriptId*/) -> HRESULT
			{
				if (FAILED(Error))
				{
					UE_LOG(LogUXWebviewWebView2, Warning,
						TEXT("the bridge bootstrap script was rejected (0x%08X)"),
						static_cast<uint32>(Error));
				}
				if (!BootstrapState->bShutdown && BootstrapState->Owner)
				{
					BootstrapState->Owner->StartNavigation();
				}
				return S_OK;
			}).Get());
}

void FUXWebviewWebView2Provider::StartNavigation()
{
	if (!State.IsValid() || State->bShutdown || !State->WebView)
	{
		return;
	}

	ApplyGeometry();
	ApplyVisibility();

	if (!State->UserAgent.IsEmpty())
	{
		SetUserAgent(State->UserAgent);
	}

	State->WebView->Navigate(*State->CurrentUrl);
	State->bReady = true;

	UE_LOG(LogUXWebviewWebView2, Log,
		TEXT("webview2 %s ready, navigating to '%s'"),
		State->bOverlayMode ? TEXT("overlay") : TEXT("texture"), *State->CurrentUrl);

	if (State->Client)
	{
		State->Client->OnViewReady();
	}
}

void FUXWebviewWebView2Provider::OnInitializationFailed(const FString& Reason)
{
	UE_LOG(LogUXWebviewWebView2, Error, TEXT("%s"), *Reason);
	if (State.IsValid())
	{
		State->bReady = false;
		if (State->Client)
		{
			State->Client->OnViewCrash(Reason);
		}
	}
}

void FUXWebviewWebView2Provider::Shutdown()
{
	if (!State.IsValid())
	{
		return;
	}

	State->bShutdown = true;
	State->Client = nullptr;
	State->Owner = nullptr;

	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (State->WebView)
	{
		State->WebView->remove_NavigationCompleted(State->NavigationCompletedToken);
		State->WebView->remove_ProcessFailed(State->ProcessFailedToken);
		State->WebView->remove_WebMessageReceived(State->WebMessageToken);
	}
	if (State->Controller)
	{
		State->Controller->Close();
	}

	State->WebView.Reset();
	State->Controller.Reset();
	State->Environment.Reset();

	ReleaseFrameTexture();
	DestroyHostWindow();

	State->PendingScripts.Reset();
	State->PendingBridge.Reset();

	if (State->bComInitialized)
	{
		::CoUninitialize();
		State->bComInitialized = false;
	}

	UE_LOG(LogUXWebviewWebView2, Log, TEXT("webview2 provider shut down"));
	State.Reset();
}

FUXWebviewResult FUXWebviewWebView2Provider::GuardInitialized() const
{
	if (!State.IsValid() || State->bShutdown)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidState,
			TEXT("webview2 provider is not running"));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::GuardReady() const
{
	const FUXWebviewResult Initialized = GuardInitialized();
	if (!Initialized.bSuccess)
	{
		return Initialized;
	}
	if (!State->bReady || !State->WebView)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady,
			TEXT("webview2 view is not ready yet"));
	}
	return FUXWebviewResult::Ok();
}

bool FUXWebviewWebView2Provider::IsOverlayMode() const
{
	return State.IsValid() && State->bOverlayMode;
}

FUXWebviewResult FUXWebviewWebView2Provider::LoadURL(const FString& Url)
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	if (Url.IsEmpty())
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("url must not be empty"));
	}

	State->CurrentUrl = Url;
	const HRESULT Hr = State->WebView->Navigate(*Url);
	if (FAILED(Hr))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			FString::Printf(TEXT("Navigate failed (0x%08X)"), static_cast<uint32>(Hr)));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::LoadHTML(const FString& Html, const FString& BaseUrl)
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	State->CurrentUrl = BaseUrl;
	const FString Document = InjectBaseUrl(Html, BaseUrl);
	const HRESULT Hr = State->WebView->NavigateToString(*Document);
	if (FAILED(Hr))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			FString::Printf(TEXT("NavigateToString failed (0x%08X)"),
				static_cast<uint32>(Hr)));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::Reload()
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	State->WebView->Reload();
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::StopLoading()
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	State->WebView->Stop();
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::SetViewport(const FIntPoint& Size, float InDpiScale)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	State->DpiScale = InDpiScale > 0.0f ? FMath::Clamp(InDpiScale, 0.25f, 4.0f) : 1.0f;
	if (State->bOverlayMode)
	{
		// Overlay geometry is owned by SetOverlayBounds, not by the widget size.
		return FUXWebviewResult::Ok();
	}

	State->ViewSize = FIntPoint(
		FMath::Clamp(Size.X, 64, kMaxViewSize),
		FMath::Clamp(Size.Y, 64, kMaxViewSize));
	ApplyGeometry();
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::SetOverlayBounds(const FIntPoint& Position,
	const FIntPoint& Size, float InDpiScale)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	if (!State->bOverlayMode)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
			TEXT("the webview2 view is not running in overlay mode"));
	}

	State->OverlayPosition = Position;
	State->OverlaySize = FIntPoint(
		FMath::Clamp(Size.X, 64, kMaxViewSize),
		FMath::Clamp(Size.Y, 64, kMaxViewSize));
	State->OverlayDpiScale = InDpiScale > 0.0f
		? FMath::Clamp(InDpiScale, 0.25f, 4.0f) : 1.0f;

	ApplyGeometry();
	ApplyVisibility();
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::SetVisible(bool bInVisible)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	State->bVisible = bInVisible;
	ApplyVisibility();
	if (bInVisible)
	{
		// Capture the new frame immediately instead of waiting for the next slot.
		State->LastCaptureSeconds = 0.0;
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::SetFrameRate(int32 FramesPerSecond)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	State->FrameRate = FMath::Clamp(FramesPerSecond, 0, kMaxFrameRate);
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::SetZoom(float ZoomFactor)
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	const double Clamped = FMath::Clamp(static_cast<double>(ZoomFactor), 0.25, 5.0);
	State->Controller->put_ZoomFactor(Clamped);
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::SetUserAgent(const FString& UserAgent)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	State->UserAgent = UserAgent;
	if (!State->WebView)
	{
		// Applied by ConfigureWebView once the controller exists.
		return FUXWebviewResult::Ok();
	}

	ComPtr<ICoreWebView2Settings> Settings;
	if (FAILED(State->WebView->get_Settings(&Settings)) || !Settings)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
			TEXT("WebView2 settings are not available"));
	}

	ComPtr<ICoreWebView2Settings2> Settings2;
	if (FAILED(Settings.As(&Settings2)) || !Settings2)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
			TEXT("WebView2 does not support a custom user agent"));
	}
	Settings2->put_UserAgent(*UserAgent);
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::ExecuteScript(const FString& Script,
	FUXWebviewScriptCallback OnResult)
{
	return ExecuteScriptInternal(Script, MoveTemp(OnResult));
}

FUXWebviewResult FUXWebviewWebView2Provider::ExecuteScriptInternal(const FString& Script,
	FUXWebviewScriptCallback OnResult)
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	const uint32 RequestId = State->NextScriptRequestId++;
	if (OnResult)
	{
		State->PendingScripts.Add(RequestId, TPair<FUXWebviewScriptCallback, double>(
			MoveTemp(OnResult), FPlatformTime::Seconds() + kScriptTimeoutSeconds));
	}

	// ICoreWebView2::ExecuteScript does not await promises, so it would report
	// an empty object for window.uxwv.call(...). The script runs inside a
	// wrapper that settles the value and posts it back as an exec message, which
	// gives game code the same awaited result as the CEF backend.
	const FString Wrapped = FString::Printf(TEXT(
		"(function(){var id=%u;"
		"function report(ok,v){var valueJson;"
		"try{valueJson=JSON.stringify(v===undefined?null:v);}"
		"catch(e){ok=false;valueJson=JSON.stringify(String(e));}"
		"try{window.chrome.webview.postMessage(JSON.stringify("
		"{uxwv:1,kind:'exec',id:id,ok:ok,valueJson:valueJson}));}catch(_){}}"
		"try{var r=eval(%s);"
		"if(r&&typeof r.then==='function'){"
		"r.then(function(v){report(true,v);},"
		"function(e){report(false,e&&e.message?e.message:String(e));});}"
		"else{report(true,r);}}"
		"catch(e){report(false,e&&e.message?e.message:String(e));}})();"),
		RequestId, *SerializeJsonString(Script));

	const HRESULT Hr = State->WebView->ExecuteScript(*Wrapped, nullptr);
	if (FAILED(Hr))
	{
		State->PendingScripts.Remove(RequestId);
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			FString::Printf(TEXT("ExecuteScript failed (0x%08X)"),
				static_cast<uint32>(Hr)));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::EmitToWeb(const FString& EventName,
	const FString& PayloadJson)
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	if (EventName.IsEmpty())
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("event name must not be empty"));
	}

	const FString Message = BuildWebMessage(TEXT("event"), EventName, PayloadJson, 0);
	const HRESULT Hr = State->WebView->PostWebMessageAsString(*Message);
	if (FAILED(Hr))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			FString::Printf(TEXT("PostWebMessageAsString failed (0x%08X)"),
				static_cast<uint32>(Hr)));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::SendInput(const FUXWebviewInputEvent& Event)
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	if (State->bOverlayMode)
	{
		// The native window receives real input; forwarding would double it.
		return FUXWebviewResult::Ok();
	}
	return SendWin32Input(Event);
}

HWND FUXWebviewWebView2Provider::EnsureInputWindow()
{
	if (State->InputWindow && IsWindow(State->InputWindow))
	{
		return State->InputWindow;
	}
	State->InputWindow = nullptr;

	if (!State->HostWindow)
	{
		return nullptr;
	}

	struct FFindContext
	{
		HWND Found = nullptr;
	};

	// Chromium nests two Chrome_WidgetWin windows under the host. The outer one
	// (Chrome_WidgetWin_0) is a wrapper that never converts window messages into
	// page input; the inner one accepts mouse, wheel and keyboard messages. Pick
	// a Chrome_WidgetWin whose parent is also a Chrome_WidgetWin and keep the
	// first non-nested match only as a fallback.
	FFindContext Context;
	EnumChildWindows(State->HostWindow,
		[](HWND Window, LPARAM Param) -> BOOL
		{
			FFindContext* Find = reinterpret_cast<FFindContext*>(Param);
			WCHAR ClassName[256] = {};
			GetClassNameW(Window, ClassName, 255);
			if (wcsstr(ClassName, L"Chrome_WidgetWin") == nullptr)
			{
				return 1;
			}

			WCHAR ParentClass[256] = {};
			GetClassNameW(GetParent(Window), ParentClass, 255);
			if (wcsstr(ParentClass, L"Chrome_WidgetWin") != nullptr)
			{
				Find->Found = Window;
				return 0;
			}
			if (!Find->Found)
			{
				Find->Found = Window;
			}
			return 1;
		}, reinterpret_cast<LPARAM>(&Context));
	State->InputWindow = Context.Found;

	if (!State->InputWindow && !State->bInputWindowMissingLogged)
	{
		State->bInputWindowMissingLogged = true;
		UE_LOG(LogUXWebviewWebView2, Warning,
			TEXT("webview2 input window not found; mouse, keyboard and IME events cannot reach the page"));
	}
	return State->InputWindow;
}

/**
 * Chromium converts WM_KEYDOWN into page events only while its browser window
 * owns the OS keyboard focus. The provider takes focus for the duration of one
 * delivery and restores it afterwards, so Slate keeps receiving key messages
 * for the widget. Returns the window that had focus, or null if nothing changed.
 */
HWND FUXWebviewWebView2Provider::AcquireInputFocus()
{
	HWND Target = EnsureInputWindow();
	HWND Previous = GetFocus();
	if (!Target || Previous == Target)
	{
		return nullptr;
	}

	// Both windows live on the game thread, so this is a plain SetFocus without
	// AttachThreadInput.
	State->Controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
	::SetFocus(Target);
	return Previous;
}

void FUXWebviewWebView2Provider::ReleaseInputFocus(HWND PreviousFocus)
{
	if (PreviousFocus && IsWindow(PreviousFocus))
	{
		::SetFocus(PreviousFocus);
	}
}

FUXWebviewResult FUXWebviewWebView2Provider::SendWin32Input(const FUXWebviewInputEvent& Event)
{
	HWND Target = EnsureInputWindow();
	if (!Target)
	{
		// The browser child window appears asynchronously with the controller.
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady,
			TEXT("webview2 input window is not available yet"));
	}

	const bool bNeedsFocus = Event.Type == EUXWebviewInputType::KeyDown
		|| Event.Type == EUXWebviewInputType::KeyUp
		|| Event.Type == EUXWebviewInputType::Char;
	HWND PreviousFocus = bNeedsFocus ? AcquireInputFocus() : nullptr;

	const int32 X = FMath::RoundToInt(Event.Position.X * State->DpiScale);
	const int32 Y = FMath::RoundToInt(Event.Position.Y * State->DpiScale);
	const WPARAM Modifiers = MouseModifiers(Event);

	// The browser windows belong to the game thread, so a synchronous delivery
	// reaches Chromium before SendInput returns and cannot be coalesced away.
	switch (Event.Type)
	{
	case EUXWebviewInputType::MouseMove:
		SendMessageW(Target, WM_MOUSEMOVE,
			Modifiers | static_cast<WPARAM>(State->MouseButtons),
			MAKELPARAM(X, Y));
		break;

	case EUXWebviewInputType::MouseDown:
	case EUXWebviewInputType::MouseUp:
	{
		UINT Message = WM_LBUTTONDOWN;
		WPARAM ButtonFlag = MK_LBUTTON;
		if (Event.Button == 1)
		{
			Message = Event.Type == EUXWebviewInputType::MouseDown
				? WM_RBUTTONDOWN : WM_RBUTTONUP;
			ButtonFlag = MK_RBUTTON;
		}
		else if (Event.Button == 2)
		{
			Message = Event.Type == EUXWebviewInputType::MouseDown
				? WM_MBUTTONDOWN : WM_MBUTTONUP;
			ButtonFlag = MK_MBUTTON;
		}
		else
		{
			Message = Event.Type == EUXWebviewInputType::MouseDown
				? WM_LBUTTONDOWN : WM_LBUTTONUP;
		}

		if (Event.Type == EUXWebviewInputType::MouseDown)
		{
			State->MouseButtons |= ButtonFlag;
		}
		else
		{
			State->MouseButtons &= ~ButtonFlag;
		}
		SendMessageW(Target, Message,
			Modifiers | static_cast<WPARAM>(State->MouseButtons), MAKELPARAM(X, Y));
		break;
	}

	case EUXWebviewInputType::MouseWheel:
	{
		const int32 Delta = FMath::RoundToInt(Event.WheelDelta * WHEEL_DELTA);
		if (Delta == 0)
		{
			break;
		}
		POINT Point = { X, Y };
		ClientToScreen(Target, &Point);
		SendMessageW(Target, WM_MOUSEWHEEL,
			MAKEWPARAM(0, static_cast<short>(Delta)),
			MAKELPARAM(Point.x, Point.y));
		break;
	}

	case EUXWebviewInputType::KeyDown:
		if (Event.KeyCode > 0)
		{
			SendMessageW(Target, WM_KEYDOWN, static_cast<WPARAM>(Event.KeyCode),
				MakeKeyLParam(Event.KeyCode, false));
		}
		break;

	case EUXWebviewInputType::KeyUp:
		if (Event.KeyCode > 0)
		{
			SendMessageW(Target, WM_KEYUP, static_cast<WPARAM>(Event.KeyCode),
				MakeKeyLParam(Event.KeyCode, true));
		}
		break;

	case EUXWebviewInputType::Char:
		for (const TCHAR Character : Event.Text)
		{
			SendMessageW(Target, WM_CHAR, static_cast<WPARAM>(Character), 1);
		}
		break;

	case EUXWebviewInputType::ImeComposition:
		SendImeCommand(TEXT("composition"), Event.Text);
		break;

	case EUXWebviewInputType::ImeCommit:
		SendImeCommand(TEXT("commit"), Event.Text);
		break;

	case EUXWebviewInputType::ImeCancel:
		SendImeCommand(TEXT("cancel"), FString());
		break;
	}

	ReleaseInputFocus(PreviousFocus);
	return FUXWebviewResult::Ok();
}

void FUXWebviewWebView2Provider::SendImeCommand(const FString& Command, const FString& Text)
{
	if (!State.IsValid() || !State->WebView || State->bShutdown)
	{
		return;
	}
	const FString Script = FString::Printf(
		TEXT("window.__uxwvIme && window.__uxwvIme('%s', %s);"),
		*Command, *SerializeJsonString(Text));
	State->WebView->ExecuteScript(*Script, nullptr);
}

FUXWebviewResult FUXWebviewWebView2Provider::SetFocus(bool bFocused)
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	if (bFocused)
	{
		State->Controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
	}
	else
	{
		State->Controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT);
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewWebView2Provider::RequestFrame()
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	if (State->bOverlayMode)
	{
		return FUXWebviewResult::Ok();
	}

	// Explicit requests ignore the rate limit but still avoid overlapping work.
	if (!State->bCaptureInFlight)
	{
		StartCapture();
	}
	return FUXWebviewResult::Ok();
}

UTexture2D* FUXWebviewWebView2Provider::GetTexture() const
{
	return State.IsValid() ? State->Texture : nullptr;
}

FUXWebviewResult FUXWebviewWebView2Provider::OpenDevTools()
{
	const FUXWebviewResult Guard = GuardReady();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	if (!UUXWebviewSettings::Get().bEnableDevTools)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
			TEXT("devtools are disabled in the UXWebview settings"));
	}

	State->WebView->OpenDevToolsWindow();
	return FUXWebviewResult::Ok();
}

bool FUXWebviewWebView2Provider::Tick(float /*DeltaSeconds*/)
{
	if (!State.IsValid() || State->bShutdown)
	{
		return false;
	}

	PollTimeouts();

	const double Now = FPlatformTime::Seconds();
	if (State->bCaptureInFlight)
	{
		if (Now - State->LastCaptureSeconds > kCaptureStallSeconds)
		{
			// A dropped handler must not freeze the view permanently.
			State->bCaptureInFlight = false;
		}
		return true;
	}

	if (State->bOverlayMode || !State->bReady || !State->bVisible || State->FrameRate <= 0)
	{
		return true;
	}

	const double Interval = 1.0 / static_cast<double>(State->FrameRate);
	if (Now - State->LastCaptureSeconds >= Interval)
	{
		StartCapture();
	}
	return true;
}

void FUXWebviewWebView2Provider::StartCapture()
{
	if (!State.IsValid() || !State->WebView || State->bShutdown || State->bCaptureInFlight)
	{
		return;
	}

	ComPtr<IStream> Stream;
	if (FAILED(CreateStreamOnHGlobal(nullptr, 1, &Stream)) || !Stream)
	{
		UE_LOG(LogUXWebviewWebView2, Warning,
			TEXT("could not allocate the WebView2 capture stream"));
		return;
	}

	State->bCaptureInFlight = true;
	State->LastCaptureSeconds = FPlatformTime::Seconds();

	const TSharedPtr<FUXWebviewWebView2State> Captured = State;
	const HRESULT Hr = State->WebView->CapturePreview(
		COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, Stream.Get(),
		Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>(
			[Captured, Stream](HRESULT Error) -> HRESULT
			{
				if (Captured->bShutdown || !Captured->Owner)
				{
					return S_OK;
				}

				TArray<uint8> Png;
				if (SUCCEEDED(Error))
				{
					STATSTG Stat = {};
					if (SUCCEEDED(Stream->Stat(&Stat, STATFLAG_NONAME))
						&& Stat.cbSize.QuadPart > 0
						&& Stat.cbSize.QuadPart < 256ll * 1024 * 1024)
					{
						Png.SetNumUninitialized(
							static_cast<int32>(Stat.cbSize.QuadPart));
						LARGE_INTEGER Start = {};
						Stream->Seek(Start, STREAM_SEEK_SET, nullptr);
						ULONG Read = 0;
						Stream->Read(Png.GetData(),
							static_cast<ULONG>(Png.Num()), &Read);
						Png.SetNum(static_cast<int32>(Read));
					}
				}

				Captured->Owner->OnCaptureCompleted(Error, MoveTemp(Png));
				return S_OK;
			}).Get());

	if (FAILED(Hr))
	{
		State->bCaptureInFlight = false;
		// The first capture can land before the page has painted; retry quietly.
		if (Hr == kCaptureNotReady)
		{
			return;
		}
		if (!State->bCaptureFailedLogged)
		{
			State->bCaptureFailedLogged = true;
			UE_LOG(LogUXWebviewWebView2, Warning,
				TEXT("CapturePreview request failed (0x%08X)"),
				static_cast<uint32>(Hr));
		}
	}
}

void FUXWebviewWebView2Provider::OnCaptureCompleted(HRESULT Error, TArray<uint8>&& PngData)
{
	if (!State.IsValid() || State->bShutdown)
	{
		return;
	}
	State->bCaptureInFlight = false;

	if (FAILED(Error) || PngData.Num() == 0)
	{
		if (Error == kCaptureNotReady)
		{
			return;
		}
		if (!State->bCaptureFailedLogged)
		{
			State->bCaptureFailedLogged = true;
			UE_LOG(LogUXWebviewWebView2, Warning,
				TEXT("CapturePreview failed (0x%08X, %d bytes)"),
				static_cast<uint32>(Error), PngData.Num());
		}
		return;
	}
	State->bCaptureFailedLogged = false;

	ApplyFrame(PngData);
}

void FUXWebviewWebView2Provider::ApplyFrame(const TArray<uint8>& PngData)
{
	IImageWrapperModule& Module =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	FImage Image;
	if (!Module.DecompressImage(PngData.GetData(), PngData.Num(), Image)
		|| Image.SizeX <= 0 || Image.SizeY <= 0 || Image.NumSlices != 1)
	{
		if (!State->bCaptureFailedLogged)
		{
			State->bCaptureFailedLogged = true;
			UE_LOG(LogUXWebviewWebView2, Warning, TEXT("could not decode a captured frame"));
		}
		return;
	}

	if (Image.Format != ERawImageFormat::BGRA8 || Image.GammaSpace != EGammaSpace::sRGB)
	{
		Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	}

	const int32 Width = Image.SizeX;
	const int32 Height = Image.SizeY;
	const int64 Bytes = static_cast<int64>(Width) * Height * 4;
	if (Image.RawData.Num() < Bytes)
	{
		return;
	}

	const bool bSizeChanged = Width != State->TextureWidth
		|| Height != State->TextureHeight || !State->Texture;
	if (!bSizeChanged)
	{
		// Texture is reused; only the pixels change.
	}
	else if (!CreateFrameTexture(Width, Height))
	{
		return;
	}

	uint8* Pixels = new uint8[static_cast<SIZE_T>(Bytes)];
	FMemory::Memcpy(Pixels, Image.RawData.GetData(), static_cast<SIZE_T>(Bytes));

	FUpdateTextureRegion2D* Region =
		new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);
	State->Texture->UpdateTextureRegions(0, 1, Region, Width * 4, 4, Pixels,
		[](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
		{
			delete[] SrcData;
			delete Regions;
		});

	++State->FrameCount;
	NotifyFrame(Width, Height, bSizeChanged);
}

bool FUXWebviewWebView2Provider::CreateFrameTexture(int32 Width, int32 Height)
{
	ReleaseFrameTexture();

	State->Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (!State->Texture)
	{
		UE_LOG(LogUXWebviewWebView2, Error,
			TEXT("CreateTransient failed for %dx%d"), Width, Height);
		return false;
	}

	State->Texture->AddToRoot();
	State->Texture->SRGB = true;
	State->Texture->Filter = TF_Bilinear;
	State->Texture->AddressX = TA_Clamp;
	State->Texture->AddressY = TA_Clamp;
	State->Texture->UpdateResource();

	State->TextureWidth = Width;
	State->TextureHeight = Height;
	return true;
}

void FUXWebviewWebView2Provider::ReleaseFrameTexture()
{
	if (!State.IsValid() || !State->Texture)
	{
		return;
	}
	State->Texture->RemoveFromRoot();
	State->Texture->MarkAsGarbage();
	State->Texture = nullptr;
	State->TextureWidth = 0;
	State->TextureHeight = 0;
}

void FUXWebviewWebView2Provider::NotifyFrame(int32 Width, int32 Height, bool bSizeChanged)
{
	if (State->bFirstFrame)
	{
		State->bFirstFrame = false;
		UE_LOG(LogUXWebviewWebView2, Log, TEXT("webview2 first frame %dx%d"),
			Width, Height);
	}

	if (State->Client && (bSizeChanged || State->FrameCount == 1))
	{
		State->Client->OnViewFrame(Width, Height);
	}
}

void FUXWebviewWebView2Provider::OnWebMessage(const FString& Message, const FString& SourceUrl)
{
	if (!State.IsValid() || State->bShutdown)
	{
		return;
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		UE_LOG(LogUXWebviewWebView2, Warning,
			TEXT("ignored a malformed bridge message from '%s'"), *SourceUrl);
		return;
	}

	const int32 Version = Object->HasField(TEXT("uxwv"))
		? Object->GetIntegerField(TEXT("uxwv")) : 0;
	if (Version != 1)
	{
		return;
	}

	const FString Kind = Object->HasField(TEXT("kind"))
		? Object->GetStringField(TEXT("kind")) : FString();

	// Exec results come from the wrapper this provider injected, so they do not
	// pass the bridge origin policy; the page already ran the script.
	if (Kind == TEXT("exec"))
	{
		const int32 ScriptRequestId = Object->HasField(TEXT("id"))
			? Object->GetIntegerField(TEXT("id")) : 0;
		const bool bSuccess = Object->HasField(TEXT("ok"))
			&& Object->GetBoolField(TEXT("ok"));
		const FString ValueJson = Object->HasField(TEXT("valueJson"))
			? Object->GetStringField(TEXT("valueJson")) : FString(TEXT("null"));
		if (ScriptRequestId > 0)
		{
			OnScriptCompleted(static_cast<uint32>(ScriptRequestId), bSuccess, ValueJson);
		}
		return;
	}

	const int32 PageRequestId = Object->HasField(TEXT("id"))
		? Object->GetIntegerField(TEXT("id")) : 0;
	const FString Name = Object->HasField(TEXT("name"))
		? Object->GetStringField(TEXT("name")) : FString();
	const FString PayloadJson = Object->HasField(TEXT("payloadJson"))
		? Object->GetStringField(TEXT("payloadJson")) : FString(TEXT("null"));

	if (Name.IsEmpty() || PageRequestId <= 0)
	{
		ReplyToPage(PageRequestId, false, TEXT("BridgeBadRequest: malformed message"));
		return;
	}

	if (!IsOriginAllowed(SourceUrl))
	{
		UE_LOG(LogUXWebviewWebView2, Warning,
			TEXT("bridge denied for origin '%s'"), *UrlOrigin(SourceUrl));
		ReplyToPage(PageRequestId, false, TEXT("BridgeDenied: origin not allowed"));
		return;
	}

	if (!State->Client)
	{
		ReplyToPage(PageRequestId, false, TEXT("the view is not available"));
		return;
	}

	const int32 RequestId = State->NextBridgeRequestId++;
	TSharedPtr<FUXWebviewBridgeRequest> Request = MakeShared<FUXWebviewBridgeRequest>();
	Request->PageRequestId = PageRequestId;
	Request->Deadline = FPlatformTime::Seconds() + kBridgeTimeoutSeconds;
	State->PendingBridge.Add(RequestId, Request);

	State->Client->OnViewScriptMessage(Name, PayloadJson, RequestId,
		[CapturedState = State, RequestId](bool bSuccess, const FString& ResultJson)
		{
			if (CapturedState->bShutdown || !CapturedState->Owner)
			{
				return;
			}
			CapturedState->Owner->CompleteBridgeRequest(RequestId, bSuccess, ResultJson);
		});
}

void FUXWebviewWebView2Provider::CompleteBridgeRequest(int32 RequestId, bool bSuccess,
	const FString& ResultJson)
{
	if (!State.IsValid() || State->bShutdown)
	{
		return;
	}

	const TSharedPtr<FUXWebviewBridgeRequest>* Found = State->PendingBridge.Find(RequestId);
	if (!Found || !Found->IsValid())
	{
		return;
	}

	TSharedPtr<FUXWebviewBridgeRequest> Request = *Found;
	State->PendingBridge.Remove(RequestId);
	if (Request->bResolved)
	{
		return;
	}
	Request->bResolved = true;
	ReplyToPage(Request->PageRequestId, bSuccess, ResultJson);
}

void FUXWebviewWebView2Provider::ReplyToPage(int32 PageRequestId, bool bSuccess,
	const FString& ResultJson)
{
	if (!State.IsValid() || State->bShutdown || !State->WebView || PageRequestId <= 0)
	{
		return;
	}

	const FString Script = FString::Printf(
		TEXT("window.__uxwvReply && window.__uxwvReply(%d, %s, %s);"),
		PageRequestId, bSuccess ? TEXT("true") : TEXT("false"),
		*SerializeJsonString(ResultJson));
	State->WebView->ExecuteScript(*Script, nullptr);
}

void FUXWebviewWebView2Provider::OnScriptCompleted(uint32 RequestId, bool bSuccess,
	const FString& Result)
{
	if (!State.IsValid() || State->bShutdown)
	{
		return;
	}

	TPair<FUXWebviewScriptCallback, double>* Found = State->PendingScripts.Find(RequestId);
	if (!Found)
	{
		return;
	}

	FUXWebviewScriptCallback Callback = MoveTemp(Found->Key);
	State->PendingScripts.Remove(RequestId);
	if (Callback)
	{
		Callback(bSuccess, Result);
	}
}

void FUXWebviewWebView2Provider::PollTimeouts()
{
	if (!State.IsValid() || State->bShutdown)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();

	TArray<uint32> ExpiredScripts;
	for (const TPair<uint32, TPair<FUXWebviewScriptCallback, double>>& Pair :
		State->PendingScripts)
	{
		if (Now >= Pair.Value.Value)
		{
			ExpiredScripts.Add(Pair.Key);
		}
	}
	for (const uint32 RequestId : ExpiredScripts)
	{
		TPair<FUXWebviewScriptCallback, double>* Found =
			State->PendingScripts.Find(RequestId);
		if (!Found)
		{
			continue;
		}
		FUXWebviewScriptCallback Callback = MoveTemp(Found->Key);
		State->PendingScripts.Remove(RequestId);
		if (Callback)
		{
			Callback(false, TEXT("timeout"));
		}
	}

	TArray<int32> ExpiredBridge;
	for (const TPair<int32, TSharedPtr<FUXWebviewBridgeRequest>>& Pair :
		State->PendingBridge)
	{
		if (Pair.Value.IsValid() && !Pair.Value->bResolved
			&& Now >= Pair.Value->Deadline)
		{
			ExpiredBridge.Add(Pair.Key);
		}
	}
	for (const int32 RequestId : ExpiredBridge)
	{
		const TSharedPtr<FUXWebviewBridgeRequest>* Found =
			State->PendingBridge.Find(RequestId);
		if (!Found || !Found->IsValid())
		{
			continue;
		}
		const TSharedPtr<FUXWebviewBridgeRequest> Request = *Found;
		State->PendingBridge.Remove(RequestId);
		if (!Request->bResolved)
		{
			Request->bResolved = true;
			ReplyToPage(Request->PageRequestId, false, TEXT("timeout"));
		}
	}
}

void FUXWebviewWebView2Provider::OnNavigationCompleted(bool bSuccess, int32 ErrorStatus,
	const FString& Url)
{
	if (!State.IsValid() || State->bShutdown)
	{
		return;
	}

	if (!Url.IsEmpty())
	{
		State->CurrentUrl = Url;
	}
	if (!bSuccess)
	{
		UE_LOG(LogUXWebviewWebView2, Warning,
			TEXT("navigation finished with error status %d"), ErrorStatus);
	}

	State->bReady = true;
	if (State->Client)
	{
		State->Client->OnViewLoadCompleted(State->CurrentUrl);
	}
}

void FUXWebviewWebView2Provider::OnProcessFailed(int32 Kind)
{
	if (!State.IsValid() || State->bShutdown)
	{
		return;
	}

	const bool bRenderProcessFailed =
		Kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED
		|| Kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE;

	if (bRenderProcessFailed && UUXWebviewSettings::Get().bAutoRecoverOnCrash)
	{
		UE_LOG(LogUXWebviewWebView2, Warning,
			TEXT("the WebView2 render process failed (kind %d); reloading"), Kind);
		if (State->WebView)
		{
			State->WebView->Reload();
		}
		State->bReady = true;
		if (State->Client)
		{
			State->Client->OnViewReady();
		}
		return;
	}

	State->bReady = false;
	UE_LOG(LogUXWebviewWebView2, Warning,
		TEXT("the WebView2 process failed (kind %d)"), Kind);
	if (State->Client)
	{
		State->Client->OnViewCrash(FString::Printf(
			TEXT("the WebView2 process failed (kind %d)"), Kind));
	}
}

void RegisterWebView2Provider()
{
	FUXWebviewProviderRegistry::Get().RegisterFactory(
		MakeShared<FUXWebviewWebView2Factory>());
	UE_LOG(LogUXWebviewWebView2, Log, TEXT("webview2 provider registered"));
}

void UnregisterWebView2Provider()
{
	FUXWebviewProviderRegistry::Get().UnregisterFactory(TEXT("WebView2"));
}

#endif // PLATFORM_WINDOWS && UXWEBVIEW_WITH_WEBVIEW2
