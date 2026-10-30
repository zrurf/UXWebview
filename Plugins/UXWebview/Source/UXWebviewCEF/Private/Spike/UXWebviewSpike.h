#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS

#include "Containers/Array.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "Frame/UXWebviewFrameGovernor.h"
#include "Frame/UXWebviewFrameSlotRing.h"
#include "HAL/PlatformProcess.h"
#include "RHI.h"
#include "Templates/SharedPointer.h"

class UTexture2D;
class SImage;
class FRHITexture;
class FRHIGPUTextureReadback;
struct FSlateBrush;

// Snapshot of one published frame. Copied by value into render commands.
struct FUXWebviewFrameSample
{
	uint32 Seq = 0;
	uint32 Generation = 0;
	uint32 BufferIndex = 0;
	int32 SlotIndex = INDEX_NONE;
	int32 Width = 0;
	int32 Height = 0;
	uint64 Handles[3] = { 0, 0, 0 };
	FRHITexture* DestTexture = nullptr;
};

// Render thread outcome of one copy, drained on the game thread.
struct FUXWebviewCopyResult
{
	int32 SlotIndex = INDEX_NONE;
	bool bSuccess = false;
};

// Spike-only bridge used to validate the CEF shared texture pipeline (M1).
// It is intentionally small and will be replaced by the real provider.
class FUXWebviewSpike
{
public:
	static FUXWebviewSpike& Get();

	bool Start(int32 InWidth, int32 InHeight, const FString& Url, int32 InTargetFps);
	void Stop();
	bool IsRunning() const { return bConnected; }

	// Hiding the widget stops frame requests entirely.
	void SetVisible(bool bVisible);
	bool IsVisible() const { return Governor.IsVisible(); }
	uint64 GetReceivedFrames() const { return ReceivedFrames; }

	FString GetStatus() const;
	void ReadbackPixels();

private:
	FUXWebviewSpike() = default;
	~FUXWebviewSpike() = default;

	bool StartHostProcess();
	bool TryConnectAndSetup();
	bool CreateTextureAndWidget(int32 InWidth, int32 InHeight);
	bool CreateFramePool(int32 InWidth, int32 InHeight);
	void ReleaseRetiredTextures();
	void PollFrameSlotCompletions();
	bool Tick(float DeltaTime);
	bool SampleFrame();
	void SendIpc(uint32 Type, uint32 A = 0, uint32 B = 0);
	void RequestFrameIfDue(float FrameRate);
	void DrainStats();

	void EnqueueFrameCopy(const FUXWebviewFrameSample& Sample);
	void RenderFrameCopy(FRHICommandListImmediate& RHICmdList, FUXWebviewFrameSample Sample);
	void RenderReleaseSources();

	// Debug helper: reads the current texture back and logs a pixel summary.
	void RequestPixelReadback();
	void PollPixelReadback();

	TUniquePtr<FRHIGPUTextureReadback> PixelReadback;
	bool bPixelReadbackPending = false;

	FProcHandle HostProcess;
	void* PipeHandle = nullptr;
	void* FrameMapping = nullptr;
	const void* FrameHeader = nullptr;

	FString PipeName;
	FString SharedMemoryName;
	FString Url;
	FString HostExePath;
	FString HostWorkingDir;
	int32 Width = 1920;
	int32 Height = 1080;
	int32 TargetFps = 0;
	bool bBeginFrameMode = false;
	FUXWebviewFrameGovernor Governor;

	bool bConnected = false;
	bool bHelloSent = false;
	bool bTearingDown = false;

	double LaunchSeconds = 0.0;
	double NextConnectAttempt = 0.0;
	double NextHealthCheck = 0.0;
	double LastBeginFrameSeconds = 0.0;
	uint32 ConnectionAttempts = 0;

	uint32 LastSeq = 0;
	uint32 ReportedGeneration = 0;
	int32 TextureWidth = 0;
	int32 TextureHeight = 0;
	bool bLoggedBadHeader = false;

	FTSTicker::FDelegateHandle TickerHandle;

	// Destination textures are triple buffered; the brush only points at a
	// texture once its copy finished on the render thread.
	TArray<UTexture2D*> PoolTextures;
	// Old pool kept alive until the first frame of the new pool is displayed.
	TArray<UTexture2D*> RetiredTextures;
	UTexture2D* DisplayTexture = nullptr;
	int32 DisplayWidth = 0;
	int32 DisplayHeight = 0;
	FUXWebviewFrameSlotRing FrameSlots{3};
	// Render thread reports finished copies here; drained on the game thread.
	TQueue<FUXWebviewCopyResult, EQueueMode::Spsc> CompletedCopySlots;
	uint64 PoolDrops = 0;
	uint64 PoolSwitches = 0;

	TSharedPtr<SImage> ImageWidget;
	TSharedPtr<FSlateBrush> Brush;

	// Render thread only.
	TArray<FTextureRHIRef> CachedSources;
	uint32 CachedGeneration = 0;
	bool bLoggedOpenFailure = false;

	// Copy cost samples in microseconds, filled on the render thread.
	TQueue<uint32, EQueueMode::Spsc> CopyMicros;
	uint64 CopyCount = 0;
	uint64 CopyTotalMicros = 0;
	uint32 CopyMaxMicros = 0;

	uint64 ReceivedFrames = 0;
	uint32 ReceivedPerSecond = 0;
	uint64 WindowFrames = 0;
	double WindowSeconds = 0.0;
};

#endif // PLATFORM_WINDOWS
