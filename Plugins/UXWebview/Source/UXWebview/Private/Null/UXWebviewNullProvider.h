#pragma once

#include "CoreMinimal.h"

#include "Containers/Ticker.h"
#include "Provider/IUXWebviewProvider.h"

class UTexture2D;

/**
 * Build-free provider used by tests and as a fallback when no browser engine is
 * available. It cycles a set of pre-generated textures so the whole UE-side
 * lifecycle (create, frame, destroy) can be exercised without CEF.
 */
class FUXWebviewNullProvider final : public IUXWebviewProvider
{
public:
	virtual FUXWebviewProviderInfo GetInfo() const override;
	virtual FUXWebviewCapabilities GetCapabilities() const override;

	virtual FUXWebviewResult Initialize(const FUXWebviewInitParams& InitParams, IUXWebviewViewClient* InClient) override;
	virtual void Shutdown() override;

	virtual FUXWebviewResult LoadURL(const FString& Url) override;
	virtual FUXWebviewResult LoadHTML(const FString& Html, const FString& BaseUrl) override;
	virtual FUXWebviewResult Reload() override;
	virtual FUXWebviewResult StopLoading() override;

	virtual FUXWebviewResult SetViewport(const FIntPoint& Size, float InDpiScale) override;
	virtual FUXWebviewResult SetVisible(bool bInVisible) override;
	virtual FUXWebviewResult SetFrameRate(int32 FramesPerSecond) override;
	virtual FUXWebviewResult SetZoom(float ZoomFactor) override;
	virtual FUXWebviewResult SetUserAgent(const FString& UserAgent) override;

	virtual FUXWebviewResult ExecuteScript(const FString& Script, FUXWebviewScriptCallback OnResult) override;
	virtual FUXWebviewResult EmitToWeb(const FString& EventName, const FString& PayloadJson) override;

	virtual FUXWebviewResult SendInput(const FUXWebviewInputEvent& Event) override;
	virtual FUXWebviewResult SetFocus(bool bFocused) override;

	virtual FUXWebviewResult RequestFrame() override;
	virtual UTexture2D* GetTexture() const override;
	virtual FUXWebviewResult OpenDevTools() override;

private:
	FUXWebviewResult GuardReady() const;
	bool Tick(float DeltaSeconds);
	bool CreateFrames();
	void ReleaseFrames();

	IUXWebviewViewClient* Client = nullptr;
	TArray<UTexture2D*> Frames;
	int32 CurrentIndex = 0;
	FIntPoint ViewSize = FIntPoint(256, 256);
	float DpiScale = 1.0f;
	int32 FrameRate = 30;
	bool bVisible = true;
	bool bInitialized = false;
	double Accumulator = 0.0;
	FString CurrentUrl;
	FTSTicker::FDelegateHandle TickerHandle;
};

/** Registers/unregisters the "Null" provider in the global registry. */
void RegisterNullProvider();
void UnregisterNullProvider();
