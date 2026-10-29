#pragma once

#include "CoreMinimal.h"

#include "UXWebviewTypes.h"

class UTexture2D;

using FUXWebviewScriptCallback = TFunction<void(bool bSuccess, const FString& ResultJson)>;

/**
 * Receives provider callbacks. Every callback is delivered on the game thread.
 * The client outlives the provider it is registered with.
 */
class IUXWebviewViewClient
{
public:
	virtual ~IUXWebviewViewClient() = default;

	virtual void OnViewReady() = 0;
	virtual void OnViewLoadCompleted(const FString& Url) = 0;
	virtual void OnViewFrame(int32 Width, int32 Height) = 0;
	virtual void OnViewCrash(const FString& Reason) = 0;
};

/**
 * Platform specific web engine. All methods are called on the game thread;
 * providers are responsible for moving work to their own threads.
 * Calls made before Initialize finished return NotReady, calls after Shutdown
 * return InvalidState.
 */
class IUXWebviewProvider
{
public:
	virtual ~IUXWebviewProvider() = default;

	virtual FUXWebviewProviderInfo GetInfo() const = 0;
	virtual FUXWebviewCapabilities GetCapabilities() const = 0;

	virtual FUXWebviewResult Initialize(const FUXWebviewInitParams& InitParams, IUXWebviewViewClient* Client) = 0;
	virtual void Shutdown() = 0;

	virtual FUXWebviewResult LoadURL(const FString& Url) = 0;
	virtual FUXWebviewResult LoadHTML(const FString& Html, const FString& BaseUrl) = 0;
	virtual FUXWebviewResult Reload() = 0;
	virtual FUXWebviewResult StopLoading() = 0;

	virtual FUXWebviewResult SetViewport(const FIntPoint& Size, float DpiScale) = 0;
	virtual FUXWebviewResult SetVisible(bool bVisible) = 0;
	virtual FUXWebviewResult SetFrameRate(int32 FramesPerSecond) = 0;
	virtual FUXWebviewResult SetZoom(float ZoomFactor) = 0;
	virtual FUXWebviewResult SetUserAgent(const FString& UserAgent) = 0;

	virtual FUXWebviewResult ExecuteScript(const FString& Script, FUXWebviewScriptCallback OnResult) = 0;
	virtual FUXWebviewResult EmitToWeb(const FString& EventName, const FString& PayloadJson) = 0;

	virtual FUXWebviewResult SendInput(const FUXWebviewInputEvent& Event) = 0;
	virtual FUXWebviewResult SetFocus(bool bFocused) = 0;

	/** Drives external begin frame providers; a no-op elsewhere. */
	virtual FUXWebviewResult RequestFrame() = 0;

	/** Current frame texture, owned by the provider. Null when no frame yet. */
	virtual UTexture2D* GetTexture() const = 0;

	virtual FUXWebviewResult OpenDevTools() = 0;
};

/** Creates providers on demand and reports availability before instantiation. */
class IUXWebviewProviderFactory
{
public:
	virtual ~IUXWebviewProviderFactory() = default;

	virtual FUXWebviewProviderInfo GetInfo() const = 0;
	virtual TUniquePtr<IUXWebviewProvider> Create() = 0;
};
