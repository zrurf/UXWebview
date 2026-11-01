#pragma once

#include "CoreMinimal.h"

#include "Containers/Ticker.h"
#include "Provider/IUXWebviewProvider.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUXWebviewWebView2, Log, All);

#if PLATFORM_WINDOWS && defined(UXWEBVIEW_WITH_WEBVIEW2) && UXWEBVIEW_WITH_WEBVIEW2

THIRD_PARTY_INCLUDES_START
#include <objidl.h>
#include <WebView2.h>
#include <wrl.h>
THIRD_PARTY_INCLUDES_END

class UTexture2D;
struct FUXWebviewWebView2State;

/**
 * In-process WebView2 (Microsoft Edge) provider.
 *
 * Texture mode hosts the browser in an off-screen window that stays shown but
 * never activates (hiding it would drop wheel input), captures the page with
 * ICoreWebView2::CapturePreview and decodes the PNG into a UTexture2D.
 * That costs a CPU capture and decode per presented frame, so the provider
 * reports bCpuPixelBuffer and caps its frame rate instead of claiming shared
 * GPU textures. Overlay mode hosts the browser in a native child window of the
 * game window; Chromium then composites and handles input itself.
 *
 * Page <-> game messaging uses the same window.uxwv contract as the CEF
 * provider. See docs/architecture.md for the capability matrix and the input
 * and IME limitations of texture mode.
 */
class FUXWebviewWebView2Provider final : public IUXWebviewProvider
{
public:
	virtual ~FUXWebviewWebView2Provider() override;

	// IUXWebviewProvider
	virtual FUXWebviewProviderInfo GetInfo() const override;
	virtual FUXWebviewCapabilities GetCapabilities() const override;

	virtual FUXWebviewResult Initialize(const FUXWebviewInitParams& InitParams,
		IUXWebviewViewClient* InClient) override;
	virtual void Shutdown() override;

	virtual FUXWebviewResult LoadURL(const FString& Url) override;
	virtual FUXWebviewResult LoadHTML(const FString& Html, const FString& BaseUrl) override;
	virtual FUXWebviewResult Reload() override;
	virtual FUXWebviewResult StopLoading() override;

	virtual FUXWebviewResult SetViewport(const FIntPoint& Size, float InDpiScale) override;
	virtual FUXWebviewResult SetOverlayBounds(const FIntPoint& Position,
		const FIntPoint& Size, float InDpiScale) override;
	virtual bool IsOverlayMode() const override;
	virtual FUXWebviewResult SetCompositionOverlay(bool bComposition) override;
	virtual bool IsCompositionOverlay() const override;
	virtual FUXWebviewResult SetVisible(bool bInVisible) override;
	virtual FUXWebviewResult SetFrameRate(int32 FramesPerSecond) override;
	virtual FUXWebviewResult SetZoom(float ZoomFactor) override;
	virtual FUXWebviewResult SetUserAgent(const FString& UserAgent) override;

	virtual FUXWebviewResult ExecuteScript(const FString& Script,
		FUXWebviewScriptCallback OnResult) override;
	virtual FUXWebviewResult EmitToWeb(const FString& EventName,
		const FString& PayloadJson) override;

	virtual FUXWebviewResult SendInput(const FUXWebviewInputEvent& Event) override;
	virtual FUXWebviewResult SetFocus(bool bFocused) override;
	virtual FUXWebviewResult RequestFrame() override;

	virtual UTexture2D* GetTexture() const override;
	virtual FUXWebviewResult OpenDevTools() override;

private:
	FUXWebviewResult GuardInitialized() const;
	FUXWebviewResult GuardReady() const;

	bool Tick(float DeltaSeconds);
	static uint64 ResolveGameWindowHandle();

	void CreateHostWindow(bool bOverlay, uint64 ParentHandle);
	void DestroyHostWindow();
	/**
	 * Swaps the host window procedure when the composition controller uses it.
	 * Restoring the saved pair also lets a regular controller take input again.
	 */
	void ApplyCompositionHosting(bool bComposition);
	void ApplyGeometry();
	void ApplyVisibility();

	/**
	 * Composition overlay only: builds the D3D11 + DirectComposition device
	 * chain, binds the root visual to the composition controller and registers
	 * the page cursor handler. Returns false when the chain cannot be created.
	 */
	bool SetupCompositionVisual();
	void ReleaseCompositionVisual();

	/**
	 * Kicks off the asynchronous environment/controller creation chain.
	 * Returns immediately; the result reports whether the chain could be started.
	 */
	FUXWebviewResult StartEnvironment();
	/**
	 * Requests the controller that hosts the page on the existing environment
	 * and host window. Returns false before issuing any asynchronous request when
	 * the mode is impossible (no environment3, no host window), so callers can
	 * fall back instead of waiting for a controller that never arrives.
	 */
	bool CreateController(bool bComposition);
	void ConfigureWebView();
	/**
	 * Applies geometry, visibility and the initial navigation. Runs after the
	 * bridge bootstrap has been registered for future documents.
	 */
	void StartNavigation();
	void OnInitializationFailed(const FString& Reason);
	/** Tears the controller down and builds the other kind on the same host. */
	FUXWebviewResult BeginControllerSwitch(bool bComposition, const FString& PageStateJson);
	void OnPageStateCaptured(bool bSuccess, const FString& PageStateJson);
	void ApplyPageState(const FString& PageStateJson);

	void StartCapture();
	/** PNG -> BGRA -> UTexture2D; runs after CapturePreview completes. */
	void OnCaptureCompleted(HRESULT Error, TArray<uint8>&& PngData);
	void ApplyFrame(const TArray<uint8>& PngData);
	bool CreateFrameTexture(int32 Width, int32 Height);
	void ReleaseFrameTexture();
	void NotifyFrame(int32 Width, int32 Height, bool bSizeChanged);

	void OnWebMessage(const FString& Message, const FString& SourceUrl);
	void OnNavigationCompleted(bool bSuccess, int32 ErrorStatus, const FString& Url);
	void OnProcessFailed(int32 Kind);
	/** Applies the view's new-window policy; the popup itself never opens. */
	void OnNewWindowRequested(const FString& Url, const FString& FrameName);
	/** Rewrites the download path and reports its progress through the client. */
	void OnDownloadStarting(ICoreWebView2DownloadStartingEventArgs* Args);
	void OnScriptCompleted(uint32 RequestId, bool bSuccess, const FString& Result);
	void ReplyToPage(int32 PageRequestId, bool bSuccess, const FString& ResultJson);
	void CompleteBridgeRequest(int32 RequestId, bool bSuccess, const FString& ResultJson);
	void PollTimeouts();

	/** Shared ExecuteScript body used by the public API and IME helpers. */
	FUXWebviewResult ExecuteScriptInternal(const FString& Script,
		FUXWebviewScriptCallback OnResult);

	HWND EnsureInputWindow();
	/** Takes OS keyboard focus for one key delivery; null when focus did not move. */
	HWND AcquireInputFocus();
	void ReleaseInputFocus(HWND PreviousFocus);
	FUXWebviewResult SendWin32Input(const FUXWebviewInputEvent& Event);
	/**
	 * Composition overlay only. Mouse events become SendMouseInput calls; key
	 * and IME events report NotSupported because the SDK exposes no way to feed
	 * them to a composition controller.
	 */
	FUXWebviewResult SendCompositionInput(const FUXWebviewInputEvent& Event);
	void SendImeCommand(const FString& Command, const FString& Text);

	TSharedPtr<FUXWebviewWebView2State> State;
	FTSTicker::FDelegateHandle TickerHandle;
};

/** Registers/unregisters the "WebView2" provider in the global registry. */
void RegisterWebView2Provider();
void UnregisterWebView2Provider();

#endif // PLATFORM_WINDOWS && UXWEBVIEW_WITH_WEBVIEW2
