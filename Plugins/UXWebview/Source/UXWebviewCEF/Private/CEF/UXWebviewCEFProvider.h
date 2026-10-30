#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS

#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "Frame/UXWebviewFrameGovernor.h"
#include "Frame/UXWebviewFrameSlotRing.h"
#include "Provider/IUXWebviewProvider.h"
#include "RHI.h"
#include "uxwebview_frame.h"

#include "CEF/UXWebviewHostClient.h"

class UTexture2D;

/** One page query waiting for a game reply; shared so a late reply is ignored. */
struct FUXWebviewBridgeRequest
{
	double Deadline = 0.0;
	bool bResolved = false;
};

/** Render thread outcome of one shared texture copy, drained on the game thread. */
struct FUXWebviewCefCopyResult
{
	int32 SlotIndex = INDEX_NONE;
	bool bSuccess = false;
};

/** One published host frame; copied by value into a render command. */
struct FUXWebviewCefFrameSample
{
	uint32 Seq = 0;
	uint32 Generation = 0;
	uint32 BufferIndex = 0;
	int32 SlotIndex = INDEX_NONE;
	int32 Width = 0;
	int32 Height = 0;
	uint64 Handles[UXWV_FRAME_POOL_SIZE] = { 0, 0, 0 };
	FRHITexture* DestTexture = nullptr;
};

/**
 * State shared with render commands. Commands capture the shared pointer, so
 * they stay valid even if the provider is already gone.
 */
struct FUXWebviewCefRenderState
{
	TArray<FTextureRHIRef> CachedSources;
	uint32 CachedGeneration = 0;
	bool bLoggedOpenFailure = false;
	TQueue<FUXWebviewCefCopyResult, EQueueMode::Spsc> Completed;
	TQueue<uint32, EQueueMode::Spsc> CopyMicros;
};

/**
 * CEF provider backed by UXWebviewHost.exe. Frames arrive through shared
 * memory; input, navigation and scripts travel over the IPC pipe.
 */
class FUXWebviewCEFProvider final : public IUXWebviewProvider, public IUXWebviewHostInstanceClient
{
public:
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

	// IUXWebviewHostInstanceClient
	virtual void OnHostReady() override;
	virtual void OnHostLoadStart() override;
	virtual void OnHostLoadEnd(const FString& Url, int32 Code) override;
	virtual void OnHostTitleChanged(const FString& Title) override;
	virtual void OnHostUrlChanged(const FString& Url) override;
	virtual void OnHostCrash(const FString& Reason) override;
	virtual void OnHostRecovered() override;
	virtual void OnHostScriptResult(uint32 RequestId, bool bSuccess, const FString& Result) override;
	virtual void OnHostBridgeMessage(const FString& Name, const FString& PayloadJson,
		uint32 RequestId) override;
	virtual void OnHostDevTools(const FString& Url) override;

private:
	FUXWebviewResult GuardInitialized() const;
	bool Tick(float DeltaSeconds);

	bool CreateFramePool(int32 InWidth, int32 InHeight);
	void ReleaseFramePool();
	void ReleaseRetiredTextures();

	bool MapSharedMemory();
	void UnmapSharedMemory();
	bool SampleFrame();
	void PollFrameSlotCompletions();
	void DrainCopyStats();
	void ResetRenderSources();
	void RequestFrameIfDue(float TargetRate);
	void PollScriptTimeouts();
	void PollBridgeTimeouts();

	IUXWebviewViewClient* Client = nullptr;
	uint32 InstanceId = 0;
	bool bInitialized = false;
	bool bReady = false;
	bool bHostLost = false;
	bool bAutoRecover = true;
	bool bBeginFrameEnabled = true;

	FIntPoint ViewSize = FIntPoint(1280, 720);
	float DpiScale = 1.0f;
	/** Last scale handed to the host; keeps redundant resizes off the IPC pipe. */
	float SentDpiScale = 1.0f;
	int32 TargetFrameRate = 60;
	FString CurrentUrl;

	void* FrameMapping = nullptr;
	const void* FrameHeader = nullptr;
	FString SharedMemoryName;
	uint32 LastSeq = 0;
	bool bLoggedBadHeader = false;

	FUXWebviewFrameGovernor Governor;
	/** Next BEGIN_FRAME is due at this time; keeps the cadence drift free. */
	double NextBeginFrameSeconds = 0.0;

	TArray<UTexture2D*> PoolTextures;
	TArray<UTexture2D*> RetiredTextures;
	UTexture2D* DisplayTexture = nullptr;
	int32 TextureWidth = 0;
	int32 TextureHeight = 0;
	FUXWebviewFrameSlotRing FrameSlots{3};
	TSharedPtr<FUXWebviewCefRenderState> RenderState;

	uint64 ReceivedFrames = 0;
	uint32 ReceivedPerSecond = 0;
	uint64 WindowFrames = 0;
	double WindowSeconds = 0.0;
	uint64 PoolDrops = 0;
	uint64 PoolSwitches = 0;
	bool bFirstFrameLogged = false;
	uint64 CopyCount = 0;
	uint64 CopyTotalMicros = 0;
	uint32 CopyMaxMicros = 0;

	/** Time from host publish to sampling, i.e. what the view adds to page latency. */
	double FrameAgeTotalMs = 0.0;
	double FrameAgeMaxMs = 0.0;
	uint64 FrameAgeSamples = 0;

	/** 5 second rolling summary window. */
	double SummarySeconds = 0.0;
	uint64 SummaryFrames = 0;

	TMap<uint32, TPair<FUXWebviewScriptCallback, double>> PendingScripts;
	uint32 NextScriptRequestId = 1;

	TMap<uint32, TSharedPtr<FUXWebviewBridgeRequest>> PendingBridge;

	FTSTicker::FDelegateHandle TickerHandle;
};

/** Registers/unregisters the "CEF" provider in the global registry. */
void RegisterCEFProvider();
void UnregisterCEFProvider();

#endif // PLATFORM_WINDOWS
