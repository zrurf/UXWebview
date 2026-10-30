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

/**
 * One web content instance. Owned by UUXWebviewSubsystem and must only be used
 * on the game thread. Every call returns a result instead of throwing.
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

	FUXWebviewResult LoadURL(const FString& Url);
	FUXWebviewResult LoadHTML(const FString& Html, const FString& BaseUrl);
	FUXWebviewResult Reload();
	FUXWebviewResult StopLoading();

	FUXWebviewResult SetViewport(const FIntPoint& Size, float DpiScale);
	FUXWebviewResult SetVisible(bool bVisible);
	FUXWebviewResult SetFrameRate(int32 FramesPerSecond);
	FUXWebviewResult SetZoom(float ZoomFactor);
	FUXWebviewResult SetUserAgent(const FString& UserAgent);

	FUXWebviewResult ExecuteScript(const FString& Script, FUXWebviewScriptCallback OnResult);
	FUXWebviewResult EmitToWeb(const FString& EventName, const FString& PayloadJson);

	/**
	 * Settles the page query delivered through OnScriptMessage. ResultJson is
	 * passed to the JS promise as-is, so it must be valid JSON text.
	 */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	bool RespondToScriptMessage(int32 RequestId, bool bSuccess, const FString& ResultJson);

	FUXWebviewResult SendInput(const FUXWebviewInputEvent& Event);
	FUXWebviewResult SetFocus(bool bFocused);
	FUXWebviewResult RequestFrame();
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
};
