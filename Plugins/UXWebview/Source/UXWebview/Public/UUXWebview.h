#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "Provider/IUXWebviewProvider.h"
#include "UXWebviewTypes.h"

#include "UUXWebview.generated.h"

class UTexture2D;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FUXWebviewReadyDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUXWebviewUrlDelegate, const FString&, Url);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUXWebviewCrashDelegate, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FUXWebviewScriptMessageDelegate,
	const FString&, Name, const FString&, PayloadJson, int32, RequestId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FUXWebviewScriptResultDelegate,
	int32, RequestId, bool, bSuccess, const FString&, ResultJson);

/**
 * One web content instance. Owned by UUXWebviewSubsystem and must only be used
 * on the game thread. Every call returns a result instead of throwing.
 *
 * Blueprint usage: create with UUXWebviewSubsystem::CreateWebview, bind the
 * delegates below, then drive the view with the BlueprintCallable functions.
 * Long-running script replies arrive on OnScriptResult.
 */
UCLASS(BlueprintType)
class UXWEBVIEW_API UUXWebview : public UObject, public IUXWebviewViewClient
{
	GENERATED_BODY()

public:
	/** Creates a provider and starts initialization. Called by the subsystem. */
	FUXWebviewResult Initialize(const FUXWebviewCreateParams& Params);
	void Destroy();

	UFUNCTION(BlueprintPure, Category = "UXWebview")
	EUXWebviewState GetState() const { return State; }

	UFUNCTION(BlueprintPure, Category = "UXWebview")
	FString GetProviderName() const { return ProviderName; }

	UFUNCTION(BlueprintPure, Category = "UXWebview")
	UTexture2D* GetTexture() const;

	/** Physical size of the last received frame; zero before the first frame. */
	UFUNCTION(BlueprintPure, Category = "UXWebview")
	FIntPoint GetFrameSize() const { return FIntPoint(FrameWidth, FrameHeight); }

	/** Number of frames received since the view became ready. */
	UFUNCTION(BlueprintPure, Category = "UXWebview")
	int32 GetFrameCount() const { return static_cast<int32>(FrameCount); }

	/** Fixed provider capabilities, so gameplay code can branch on degradation. */
	UFUNCTION(BlueprintPure, Category = "UXWebview")
	FUXWebviewCapabilities GetCapabilities() const { return Capabilities; }

	/** Navigates to an absolute URL; requires a ready view. */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult LoadURL(const FString& Url);

	/** Loads HTML text. BaseUrl resolves relative links and the bridge origin. */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult LoadHTML(const FString& Html, const FString& BaseUrl);

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult Reload();

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult StopLoading();

	/** Resizes the page in physical pixels; a zero DPI scale keeps the current one. */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult SetViewport(const FIntPoint& Size, float DpiScale);

	/** Overlay only: absolute widget rect in parent client pixels, updated per tick. */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult SetOverlayBounds(const FIntPoint& Position, const FIntPoint& Size,
		float DpiScale);

	/** True when this view composites through a native child window. */
	UFUNCTION(BlueprintPure, Category = "UXWebview")
	bool IsOverlayMode() const;

	/**
	 * Overlay only: moves the page between composition hosting (transparent,
	 * page cursors, no typed input) and a regular controller (opaque, native
	 * keyboard and IME). The provider recreates its controller and reloads the
	 * current URL, so page state and scroll position are lost.
	 */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult SetCompositionOverlay(bool bComposition);

	/** True while the page renders through a composition controller. */
	UFUNCTION(BlueprintPure, Category = "UXWebview")
	bool IsCompositionOverlay() const;

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult SetVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult SetFrameRate(int32 FramesPerSecond);

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult SetZoom(float ZoomFactor);

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult SetUserAgent(const FString& UserAgent);

	FUXWebviewResult ExecuteScript(const FString& Script, FUXWebviewScriptCallback OnResult);

	/**
	 * Blueprint-friendly script call. Returns a request id for matching
	 * OnScriptResult, or 0 when the view cannot run scripts; the delegate still
	 * receives the failure with request id 0.
	 */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	int32 ExecuteScriptAsync(const FString& Script);

	/** Dispatches a uxwv CustomEvent to the page. */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult EmitToWeb(const FString& EventName, const FString& PayloadJson);

	/**
	 * Settles the page query delivered through OnScriptMessage. ResultJson is
	 * passed to the JS promise as-is, so it must be valid JSON text.
	 */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	bool RespondToScriptMessage(int32 RequestId, bool bSuccess, const FString& ResultJson);

	/** Routes one mouse, key or IME event; coordinates are view CSS pixels.
	 *  Overlay views forward as well: the native browser window owns real input,
	 *  so a synthetic event here never duplicates a physical one. Composition
	 *  overlay views accept mouse events only. */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult SendInput(const FUXWebviewInputEvent& Event);

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult SetFocus(bool bFocused);

	FUXWebviewResult RequestFrame();

	/** Opens provider devtools; fails with NotSupported when unavailable. */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	FUXWebviewResult OpenDevTools();

	UPROPERTY(BlueprintAssignable, Category = "UXWebview")
	FUXWebviewReadyDelegate OnReady;

	UPROPERTY(BlueprintAssignable, Category = "UXWebview")
	FUXWebviewUrlDelegate OnLoadCompleted;

	UPROPERTY(BlueprintAssignable, Category = "UXWebview")
	FUXWebviewCrashDelegate OnCrashed;

	/** A page called window.uxwv.call/emit; answer with RespondToScriptMessage. */
	UPROPERTY(BlueprintAssignable, Category = "UXWebview")
	FUXWebviewScriptMessageDelegate OnScriptMessage;

	/** Reply for a script started with ExecuteScriptAsync, matched by RequestId. */
	UPROPERTY(BlueprintAssignable, Category = "UXWebview")
	FUXWebviewScriptResultDelegate OnScriptResult;

	// IUXWebviewViewClient
	virtual void OnViewReady() override;
	virtual void OnViewLoadCompleted(const FString& Url) override;
	virtual void OnViewFrame(int32 Width, int32 Height) override;
	virtual void OnViewCrash(const FString& Reason) override;
	virtual void OnViewScriptMessage(const FString& Name, const FString& PayloadJson,
		int32 RequestId, FUXWebviewBridgeReply Reply) override;

protected:
	virtual void BeginDestroy() override;

private:
	/** Shared guard for calls that need a live, ready provider. */
	FUXWebviewResult GuardReady() const;
	FUXWebviewResult GuardAlive() const;
	void ShutdownProvider();

	UPROPERTY(Transient)
	EUXWebviewState State = EUXWebviewState::Created;

	FString ProviderName;
	FUXWebviewCapabilities Capabilities;

	// Not a UPROPERTY: providers are plain C++ objects and never serialized.
	TUniquePtr<IUXWebviewProvider> Provider;

	/** Keeps the provider frame texture alive between GC runs. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> CachedTexture;

	/** Page queries waiting for the game to call RespondToScriptMessage. */
	TMap<int32, FUXWebviewBridgeReply> PendingScriptReplies;

	/** Rejects every outstanding query, e.g. when the view goes away. */
	void FailPendingScriptReplies(const FString& Reason);

	int32 FrameWidth = 0;
	int32 FrameHeight = 0;
	uint64 FrameCount = 0;

	/** Request ids issued by ExecuteScriptAsync; zero is reserved for failures. */
	int32 NextScriptRequestId = 1;
};
