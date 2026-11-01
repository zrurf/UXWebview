#include "CEF/UXWebviewCEFProvider.h"

#if PLATFORM_WINDOWS

#include <atomic>

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "CEF/UXWebviewSharedTexture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/PlatformMisc.h"
#include "ID3D11DynamicRHI.h"
#include "ID3D12DynamicRHI.h"
#include "Misc/Paths.h"
#include "Provider/UXWebviewProviderRegistry.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RHITransition.h"
#include "TextureResource.h"
#include "UXWebviewSettings.h"
#include "Widgets/SWindow.h"
#include "uxwebview_frame.h"
#include "uxwebview_ipc.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewCEFProvider, Log, All);

DECLARE_STATS_GROUP(TEXT("UXWebview"), STATGROUP_UXWebview, STATCAT_Advanced);
DECLARE_DWORD_COUNTER_STAT(TEXT("Frames Received"), STAT_UXWebview_FramesReceived, STATGROUP_UXWebview);
DECLARE_DWORD_COUNTER_STAT(TEXT("Frames Skipped"), STAT_UXWebview_FramesSkipped, STATGROUP_UXWebview);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool Switches"), STAT_UXWebview_PoolSwitches, STATGROUP_UXWebview);
DECLARE_DWORD_COUNTER_STAT(TEXT("Slots Busy"), STAT_UXWebview_SlotsBusy, STATGROUP_UXWebview);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Target FPS"), STAT_UXWebview_TargetFPS, STATGROUP_UXWebview);
DECLARE_CYCLE_STAT(TEXT("Frame Copy"), STAT_UXWebview_FrameCopy, STATGROUP_UXWebview);

namespace
{
constexpr int32 kMaxViewSize = 2048;
/** Overlay windows follow the desktop surface, which may be wider than 2048. */
constexpr int32 kMaxOverlaySize = 8192;
// High refresh displays go well past 120 Hz; the ticker only ever asks for one
// frame per game frame, so a higher ceiling costs nothing when it is unused.
constexpr int32 kMaxFrameRate = 240;
constexpr double kScriptTimeoutSeconds = 30.0;
/** A page query that the game never answers is rejected after this delay. */
constexpr double kBridgeTimeoutSeconds = 30.0;
constexpr double kSummaryIntervalSeconds = 5.0;

/**
 * Opens the pool handles once per generation and moves them to a shader
 * readable state. Shared by the copy and the direct display path.
 */
bool EnsureCachedSources(FRHICommandListImmediate& RHICmdList,
	const TSharedPtr<FUXWebviewCefRenderState>& State, const FUXWebviewCefFrameSample& Sample)
{
	if (!State.IsValid())
	{
		return false;
	}

	if (Sample.Generation != State->CachedGeneration
		|| State->CachedSources.Num() != UXWV_FRAME_POOL_SIZE)
	{
		State->CachedSources.Empty();
		State->CachedSources.Reserve(UXWV_FRAME_POOL_SIZE);
		for (int32 Index = 0; Index < UXWV_FRAME_POOL_SIZE; ++Index)
		{
			State->CachedSources.Add(UXWebviewSharedTexture::Open(Sample.Handles[Index]));
		}
		State->CachedGeneration = Sample.Generation;

		// Opened resources start in COMMON; tell the RHI where they live now.
		for (const FTextureRHIRef& Source : State->CachedSources)
		{
			if (Source.IsValid())
			{
				RHICmdList.Transition(FRHITransitionInfo(Source.GetReference(),
					ERHIAccess::Unknown, ERHIAccess::SRVMask));
			}
		}
	}

	if (!State->CachedSources.IsValidIndex(static_cast<int32>(Sample.BufferIndex))
		|| !State->CachedSources[Sample.BufferIndex].IsValid())
	{
		if (!State->bLoggedOpenFailure)
		{
			State->bLoggedOpenFailure = true;
			UE_LOG(LogUXWebviewCEFProvider, Error,
				TEXT("no shared texture for buffer %u"), Sample.BufferIndex);
		}
		return false;
	}

	return true;
}

void RenderFrameCopy(FRHICommandListImmediate& RHICmdList,
	const TSharedPtr<FUXWebviewCefRenderState>& State, FUXWebviewCefFrameSample Sample)
{
	SCOPE_CYCLE_COUNTER(STAT_UXWebview_FrameCopy);

	FUXWebviewCefCopyResult Result;
	Result.SlotIndex = Sample.SlotIndex;

	if (!State.IsValid() || !Sample.DestTexture)
	{
		if (State.IsValid())
		{
			State->Completed.Enqueue(Result);
		}
		return;
	}

	if (!EnsureCachedSources(RHICmdList, State, Sample))
	{
		State->Completed.Enqueue(Result);
		return;
	}

	const uint64 StartCycles = FPlatformTime::Cycles64();
	RHICmdList.CopyTexture(State->CachedSources[Sample.BufferIndex].GetReference(),
		Sample.DestTexture, FRHICopyTextureInfo());
	const double ElapsedMs = FPlatformTime::ToMilliseconds64(
		FPlatformTime::Cycles64() - StartCycles);

	State->CopyMicros.Enqueue(static_cast<uint32>(FMath::RoundToInt(ElapsedMs * 1000.0)));

	Result.bSuccess = true;
	State->Completed.Enqueue(Result);
}

/**
 * Direct display: points the display texture at the newest shared buffer.
 * Slate re-reads the resource's TextureRHI on every draw, so no copy is needed.
 */
void RenderBindFrame(FRHICommandListImmediate& RHICmdList,
	const TSharedPtr<FUXWebviewCefRenderState>& State, FUXWebviewCefFrameSample Sample,
	UTexture2D* Target)
{
	if (!State.IsValid() || !Target)
	{
		return;
	}

	if (!EnsureCachedSources(RHICmdList, State, Sample))
	{
		return;
	}

	FTextureResource* Resource = Target->GetResource();
	if (!Resource)
	{
		// The resource is created on the render thread; the next frame retries.
		return;
	}

	// The previous RHI texture is released here; the shared one stays alive
	// through State->CachedSources.
	Resource->TextureRHI = State->CachedSources[Sample.BufferIndex];
}

class FUXWebviewCEFFactory final : public IUXWebviewProviderFactory
{
public:
	virtual FUXWebviewProviderInfo GetInfo() const override;
	virtual TUniquePtr<IUXWebviewProvider> Create() override
	{
		return MakeUnique<FUXWebviewCEFProvider>();
	}
};

FUXWebviewProviderInfo MakeProviderInfo()
{
	FUXWebviewProviderInfo Info;
	Info.Name = TEXT("CEF");
	Info.DisplayName = TEXT("CEF (Chromium)");
	Info.Version = TEXT("156.0.14");
	Info.Platform = TEXT("Win64");

	FString Reason;
	Info.bAvailable = FUXWebviewHostClient::Get().IsHostAvailable(&Reason);
	Info.UnavailableReason = Reason;
	return Info;
}

FUXWebviewProviderInfo FUXWebviewCEFFactory::GetInfo() const
{
	return MakeProviderInfo();
}
} // namespace

FUXWebviewProviderInfo FUXWebviewCEFProvider::GetInfo() const
{
	return MakeProviderInfo();
}

FUXWebviewCapabilities FUXWebviewCEFProvider::GetCapabilities() const
{
	FUXWebviewCapabilities Capabilities;
	Capabilities.bGpuSharedTexture = true;
	Capabilities.bOverlayMode = true;
	Capabilities.bExternalBeginFrame = true;
	Capabilities.bOfflineCache = true;
	Capabilities.bDevTools = UUXWebviewSettings::Get().bEnableDevTools;
	Capabilities.MaxFrameRate = kMaxFrameRate;
	Capabilities.MaxViewportSize = FIntPoint(kMaxViewSize, kMaxViewSize);
	return Capabilities;
}

FUXWebviewResult FUXWebviewCEFProvider::Initialize(
	const FUXWebviewInitParams& InitParams, IUXWebviewViewClient* InClient)
{
	if (bInitialized)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidState,
			TEXT("cef provider is already initialized"));
	}
	if (!InClient)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("cef provider needs a view client"));
	}
	if (!IsRHID3D12() && !IsRHID3D11())
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
			TEXT("cef provider requires the D3D12 or D3D11 RHI"));
	}

	Client = InClient;
	bAutoRecover = UUXWebviewSettings::Get().bAutoRecoverOnCrash;
	DpiScale = InitParams.DpiScale > 0.0f ? InitParams.DpiScale : 1.0f;
	ViewSize = FIntPoint(
		FMath::Clamp(InitParams.ViewportSize.X, 64, kMaxViewSize),
		FMath::Clamp(InitParams.ViewportSize.Y, 64, kMaxViewSize));
	TargetFrameRate = FMath::Clamp(InitParams.TargetFrameRate, 0, kMaxFrameRate);
	bBeginFrameEnabled = TargetFrameRate > 0;
	CurrentUrl = InitParams.InitialUrl;

	// Overlay replaces the texture pipeline, so it needs a native parent window
	// and the game window handle to fall back to texture mode when unavailable.
	uint64 ParentHandle = 0;
	if (InitParams.bOverlay)
	{
		ParentHandle = ResolveGameWindowHandle();
		if (ParentHandle == 0)
		{
			UE_LOG(LogUXWebviewCEFProvider, Warning,
				TEXT("overlay was requested but no game window is available; using texture mode"));
		}
	}
	bOverlayMode = ParentHandle != 0;
	if (bOverlayMode)
	{
		// Windowed browsers are paced by the compositor; begin frame stays off.
		bBeginFrameEnabled = false;
	}

	FUXWebviewGovernorConfig GovernorConfig;
	const float ActiveRate = bBeginFrameEnabled ? static_cast<float>(TargetFrameRate) : 60.0f;
	GovernorConfig.ActiveFrameRate = ActiveRate;
	GovernorConfig.MaxFrameRate = FMath::Max(ActiveRate, 120.0f);
	Governor.SetConfig(GovernorConfig);
	Governor.SetVisible(true);
	Governor.SetAnimated(true);

	// Direct display keeps one texture and binds the shared frame into it, so
	// the game thread pays no full-frame copy per frame.
	bDirectDisplay = !bOverlayMode && UUXWebviewSettings::Get().bDirectDisplay;
	if (!bOverlayMode)
	{
		RenderState = MakeShared<FUXWebviewCefRenderState>();
		const bool bTexturesReady = bDirectDisplay
			? CreateDirectTexture(ViewSize.X, ViewSize.Y)
			: CreateFramePool(ViewSize.X, ViewSize.Y);
		if (!bTexturesReady)
		{
			RenderState.Reset();
			Client = nullptr;
			return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
				TEXT("cef provider could not create its frame textures"));
		}
	}

	bInitialized = true;

	FUXWebviewHostInstanceSpec Spec;
	Spec.Size = ViewSize;
	SentDpiScale = DpiScale;
	Spec.DpiScale = DpiScale;
	Spec.Url = CurrentUrl.IsEmpty() ? FString(TEXT("about:blank")) : CurrentUrl;
	Spec.FrameRate = bBeginFrameEnabled ? TargetFrameRate : 60;
	// Overlay stays hidden until the widget reports its rectangle.
	Spec.bVisible = !bOverlayMode;
	Spec.bOverlay = bOverlayMode;
	Spec.ParentHandle = ParentHandle;
	Spec.NewWindowPolicy = InitParams.NewWindowPolicy == EUXWebviewNewWindowPolicy::External
		? TEXT("external")
		: (InitParams.NewWindowPolicy == EUXWebviewNewWindowPolicy::Broadcast
			? TEXT("broadcast") : TEXT("same-view"));
	Spec.DownloadDirectory = InitParams.DownloadDirectory;

	FUXWebviewResult Result = FUXWebviewHostClient::Get().CreateInstance(Spec, this, InstanceId);
	if (!Result.bSuccess)
	{
		bInitialized = false;
		Client = nullptr;
		ReleaseFramePool();
		RenderState.Reset();
		return Result;
	}

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FUXWebviewCEFProvider::Tick));

	UE_LOG(LogUXWebviewCEFProvider, Log,
		TEXT("cef view %u created: %s %dx%d scale=%.2f fps=%d url='%s'"),
		InstanceId, bOverlayMode ? TEXT("overlay")
			: (bDirectDisplay ? TEXT("direct") : TEXT("texture")),
		ViewSize.X, ViewSize.Y, Spec.DpiScale, Spec.FrameRate, *Spec.Url);
	return FUXWebviewResult::Ok();
}

uint64 FUXWebviewCEFProvider::ResolveGameWindowHandle()
{
	if (!GEngine || !GEngine->GameViewport)
	{
		return 0;
	}

	TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow();
	if (!Window.IsValid())
	{
		return 0;
	}

	TSharedPtr<FGenericWindow> Native = Window->GetNativeWindow();
	if (!Native.IsValid())
	{
		return 0;
	}
	return static_cast<uint64>(reinterpret_cast<uintptr_t>(Native->GetOSWindowHandle()));
}

void FUXWebviewCEFProvider::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}
	bInitialized = false;

	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (InstanceId != 0)
	{
		FUXWebviewHostClient::Get().DestroyInstance(InstanceId);
		InstanceId = 0;
	}

	UnmapSharedMemory();
	ResetRenderSources();
	ReleaseRetiredTextures();
	ReleaseFramePool();
	FrameSlots.Reset(3);
	PendingScripts.Reset();
	PendingBridge.Reset();
	DisplayTexture = nullptr;
	Client = nullptr;
	LastSeq = 0;

	UE_LOG(LogUXWebviewCEFProvider, Log, TEXT("cef provider shut down"));
}

FUXWebviewResult FUXWebviewCEFProvider::GuardInitialized() const
{
	if (!bInitialized || bHostLost)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidState,
			TEXT("cef provider is not running"));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewCEFProvider::LoadURL(const FString& Url)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	CurrentUrl = Url;
	return FUXWebviewHostClient::Get().Navigate(InstanceId, Url);
}

FUXWebviewResult FUXWebviewCEFProvider::LoadHTML(const FString& Html, const FString& BaseUrl)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	CurrentUrl = BaseUrl;
	return FUXWebviewHostClient::Get().LoadHtml(InstanceId, Html, BaseUrl);
}

FUXWebviewResult FUXWebviewCEFProvider::Reload()
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	return FUXWebviewHostClient::Get().Reload(InstanceId);
}

FUXWebviewResult FUXWebviewCEFProvider::StopLoading()
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	return FUXWebviewHostClient::Get().StopLoading(InstanceId);
}

FUXWebviewResult FUXWebviewCEFProvider::SetViewport(const FIntPoint& Size, float InDpiScale)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	DpiScale = InDpiScale > 0.0f ? FMath::Clamp(InDpiScale, 0.25f, 4.0f) : 1.0f;
	if (bOverlayMode)
	{
		// Overlay geometry comes from SetOverlayBounds; the widget size is not
		// authoritative for a native window.
		return FUXWebviewResult::Ok();
	}

	const FIntPoint Clamped(
		FMath::Clamp(Size.X, 64, kMaxViewSize),
		FMath::Clamp(Size.Y, 64, kMaxViewSize));
	const bool bScaleChanged = !FMath::IsNearlyEqual(DpiScale, SentDpiScale, 0.001f);
	if (Clamped != ViewSize || bScaleChanged)
	{
		ViewSize = Clamped;
		SentDpiScale = DpiScale;
		// The new texture pool is built once the host publishes the new size.
		if (InstanceId != 0)
		{
			FUXWebviewHostClient::Get().Resize(InstanceId, Clamped, DpiScale);
		}
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewCEFProvider::SetOverlayBounds(const FIntPoint& Position,
	const FIntPoint& Size, float InDpiScale)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	if (!bOverlayMode)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
			TEXT("the cef view is not running in overlay mode"));
	}

	const float Scale = InDpiScale > 0.0f ? FMath::Clamp(InDpiScale, 0.25f, 4.0f) : 1.0f;
	const FIntPoint ClampedSize(
		FMath::Clamp(Size.X, 64, kMaxOverlaySize),
		FMath::Clamp(Size.Y, 64, kMaxOverlaySize));
	const bool bChanged = ClampedSize != OverlaySize || Position != OverlayPosition
		|| !FMath::IsNearlyEqual(Scale, OverlayDpiScale, 0.001f);
	if (!bChanged)
	{
		return FUXWebviewResult::Ok();
	}

	OverlaySize = ClampedSize;
	OverlayPosition = Position;
	OverlayDpiScale = Scale;

	if (InstanceId == 0)
	{
		return FUXWebviewResult::Ok();
	}

	FUXWebviewResult Result = ApplyOverlayBounds();
	UE_LOG(LogUXWebviewCEFProvider, Log,
		TEXT("instance %u overlay bounds %d,%d %dx%d scale=%.2f"),
		InstanceId, Position.X, Position.Y, ClampedSize.X, ClampedSize.Y, Scale);
	return Result;
}

FUXWebviewResult FUXWebviewCEFProvider::ApplyOverlayBounds()
{
	// Re-sends the last known rectangle; the host may not have been connected
	// yet when the widget first reported it.
	if (!bOverlayMode || InstanceId == 0 || OverlaySize == FIntPoint::ZeroValue)
	{
		return FUXWebviewResult::Ok();
	}

	const FUXWebviewResult Result = FUXWebviewHostClient::Get().SetBounds(
		InstanceId, OverlayPosition, OverlaySize, OverlayDpiScale);
	if (Result.bSuccess && !bOverlayShown)
	{
		// The native child window is created hidden so it never flashes at the
		// origin; the first applied rectangle is what makes it appear.
		bOverlayShown = true;
		FUXWebviewHostClient::Get().SetVisible(InstanceId, true);
	}
	return Result;
}

FUXWebviewResult FUXWebviewCEFProvider::SetVisible(bool bInVisible)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	Governor.SetVisible(bInVisible);
	if (InstanceId != 0)
	{
		FUXWebviewHostClient::Get().SetVisible(InstanceId, bInVisible);
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewCEFProvider::SetFrameRate(int32 FramesPerSecond)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	TargetFrameRate = FMath::Clamp(FramesPerSecond, 0, kMaxFrameRate);
	if (bOverlayMode)
	{
		// The compositor owns the cadence of a windowed browser.
		return FUXWebviewResult::Ok();
	}
	bBeginFrameEnabled = TargetFrameRate > 0;

	FUXWebviewGovernorConfig GovernorConfig = Governor.GetConfig();
	const float ActiveRate = bBeginFrameEnabled ? static_cast<float>(TargetFrameRate) : 60.0f;
	GovernorConfig.ActiveFrameRate = ActiveRate;
	GovernorConfig.MaxFrameRate = FMath::Max(ActiveRate, 120.0f);
	Governor.SetConfig(GovernorConfig);

	if (InstanceId != 0 && bBeginFrameEnabled)
	{
		FUXWebviewHostClient::Get().SetFrameRate(InstanceId, TargetFrameRate);
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewCEFProvider::SetZoom(float ZoomFactor)
{
	(void)ZoomFactor;
	return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
		TEXT("cef zoom is not wired yet"));
}

FUXWebviewResult FUXWebviewCEFProvider::SetUserAgent(const FString& UserAgent)
{
	(void)UserAgent;
	return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
		TEXT("cef user agent is not wired yet"));
}

FUXWebviewResult FUXWebviewCEFProvider::ExecuteScript(
	const FString& Script, FUXWebviewScriptCallback OnResult)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	const uint32 RequestId = NextScriptRequestId++;
	const FUXWebviewResult Sent = FUXWebviewHostClient::Get().ExecuteScript(
		InstanceId, RequestId, Script);
	if (!Sent.bSuccess)
	{
		return Sent;
	}

	if (OnResult)
	{
		PendingScripts.Add(RequestId, TPair<FUXWebviewScriptCallback, double>(
			MoveTemp(OnResult), FPlatformTime::Seconds()));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewCEFProvider::EmitToWeb(const FString& EventName, const FString& PayloadJson)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	return FUXWebviewHostClient::Get().EmitToWeb(InstanceId, EventName, PayloadJson);
}

FUXWebviewResult FUXWebviewCEFProvider::SendInput(const FUXWebviewInputEvent& Event)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	Governor.NotifyInteraction();
	// Overlay instances forward as well: real input reaches their native window
	// through the OS, and SendInput calls arrive over the IPC path, so one
	// physical event never produces two page events.
	return FUXWebviewHostClient::Get().SendInput(InstanceId, Event);
}

FUXWebviewResult FUXWebviewCEFProvider::SetFocus(bool bFocused)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	return FUXWebviewHostClient::Get().SetFocus(InstanceId, bFocused);
}

FUXWebviewResult FUXWebviewCEFProvider::RequestFrame()
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	if (bOverlayMode)
	{
		return FUXWebviewResult::Ok();
	}
	return FUXWebviewHostClient::Get().BeginFrame(InstanceId);
}

UTexture2D* FUXWebviewCEFProvider::GetTexture() const
{
	return DisplayTexture;
}

FUXWebviewResult FUXWebviewCEFProvider::OpenDevTools()
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}
	if (!UUXWebviewSettings::Get().bEnableDevTools)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
			TEXT("devtools are disabled in the UXWebview settings"));
	}
	return FUXWebviewHostClient::Get().RequestDevTools(InstanceId);
}

void FUXWebviewCEFProvider::OnHostReady()
{
	bReady = true;
	if (!bOverlayMode)
	{
		MapSharedMemory();
		UE_LOG(LogUXWebviewCEFProvider, Log, TEXT("host instance %u ready"), InstanceId);
	}
	else
	{
		UE_LOG(LogUXWebviewCEFProvider, Log,
			TEXT("host instance %u ready (overlay compositing)"), InstanceId);
		// The widget usually reports its rectangle before the host connects.
		ApplyOverlayBounds();
	}

	if (Client)
	{
		Client->OnViewReady();
	}
}

void FUXWebviewCEFProvider::OnHostLoadStart()
{
	UE_LOG(LogUXWebviewCEFProvider, Verbose, TEXT("instance %u load started"), InstanceId);
}

void FUXWebviewCEFProvider::OnHostLoadEnd(const FString& Url, int32 Code)
{
	if (!Url.IsEmpty())
	{
		CurrentUrl = Url;
	}

	if (Code >= 400)
	{
		UE_LOG(LogUXWebviewCEFProvider, Warning,
			TEXT("instance %u finished loading with code %d"), InstanceId, Code);
	}

	const FString LoadedUrl = CurrentUrl;
	if (Client)
	{
		Client->OnViewLoadCompleted(LoadedUrl);
	}
}

void FUXWebviewCEFProvider::OnHostTitleChanged(const FString& Title)
{
	UE_LOG(LogUXWebviewCEFProvider, Verbose, TEXT("instance %u title '%s'"), InstanceId, *Title);
}

void FUXWebviewCEFProvider::OnHostUrlChanged(const FString& Url)
{
	CurrentUrl = Url;
	UE_LOG(LogUXWebviewCEFProvider, Verbose, TEXT("instance %u url '%s'"), InstanceId, *Url);
}

void FUXWebviewCEFProvider::OnHostCrash(const FString& Reason)
{
	bReady = false;

	UE_LOG(LogUXWebviewCEFProvider, Warning, TEXT("instance %u host crash: %s"), InstanceId, *Reason);

	// Stop the host client recreation before reporting, in case the listener
	// destroys this provider from inside the callback.
	if (!bAutoRecover && InstanceId != 0)
	{
		FUXWebviewHostClient::Get().DestroyInstance(InstanceId);
		InstanceId = 0;
		bHostLost = true;
	}

	if (Client)
	{
		Client->OnViewCrash(Reason);
	}
}

void FUXWebviewCEFProvider::OnHostRecovered()
{
	UnmapSharedMemory();
	ResetRenderSources();
	FrameSlots.Reset(3);
	LastSeq = 0;
	SharedMemoryName.Reset();
	bFirstFrameLogged = false;

	bReady = true;
	if (!bOverlayMode)
	{
		MapSharedMemory();
	}
	else
	{
		// The recreated child window starts hidden and may sit at the old
		// rectangle, so apply the last known geometry again.
		bOverlayShown = false;
		ApplyOverlayBounds();
	}

	UE_LOG(LogUXWebviewCEFProvider, Log, TEXT("instance %u recovered on a new host"), InstanceId);

	if (Client)
	{
		Client->OnViewReady();
	}
}

void FUXWebviewCEFProvider::OnHostScriptResult(uint32 RequestId, bool bSuccess, const FString& Result)
{
	TPair<FUXWebviewScriptCallback, double>* Found = PendingScripts.Find(RequestId);
	if (!Found)
	{
		return;
	}

	FUXWebviewScriptCallback Callback = MoveTemp(Found->Key);
	PendingScripts.Remove(RequestId);

	if (Callback)
	{
		Callback(bSuccess, Result);
	}
}

void FUXWebviewCEFProvider::OnHostBridgeMessage(const FString& Name,
	const FString& PayloadJson, uint32 RequestId)
{
	if (!Client)
	{
		FUXWebviewHostClient::Get().ReplyBridge(InstanceId, RequestId, false,
			TEXT("no view client"));
		return;
	}

	// The shared flag lets a game reply and the timeout race safely: whichever
	// resolves first wins, the other is dropped.
	TSharedPtr<FUXWebviewBridgeRequest> Request = MakeShared<FUXWebviewBridgeRequest>();
	Request->Deadline = FPlatformTime::Seconds() + kBridgeTimeoutSeconds;
	PendingBridge.Add(RequestId, Request);

	const uint32 ReplyInstance = InstanceId;
	Client->OnViewScriptMessage(Name, PayloadJson, static_cast<int32>(RequestId),
		[ReplyInstance, RequestId, Request](bool bSuccess, const FString& Result)
		{
			if (Request->bResolved)
			{
				return;
			}
			Request->bResolved = true;
			FUXWebviewHostClient::Get().ReplyBridge(
				ReplyInstance, RequestId, bSuccess, Result);
		});
}

void FUXWebviewCEFProvider::OnHostDevTools(const FString& Url)
{
	UE_LOG(LogUXWebviewCEFProvider, Log, TEXT("instance %u devtools at '%s'"), InstanceId, *Url);
}

void FUXWebviewCEFProvider::OnHostNewWindow(const FString& Url, const FString& FrameName)
{
	if (Client)
	{
		Client->OnViewNewWindow(Url, FrameName);
	}
}

void FUXWebviewCEFProvider::OnHostDownload(const FUXWebviewDownloadInfo& Info)
{
	if (Client)
	{
		Client->OnViewDownload(Info);
	}
}

bool FUXWebviewCEFProvider::Tick(float DeltaSeconds)
{
	if (!bInitialized)
	{
		return false;
	}

	PollScriptTimeouts();
	PollBridgeTimeouts();

	// Overlay browsers are composited by Chromium inside the game window, so
	// there is no shared memory, no copy and nothing to pace from here.
	if (bOverlayMode)
	{
		return true;
	}

	if (!bHostLost)
	{
		if (!FrameHeader && InstanceId != 0)
		{
			MapSharedMemory();
		}
		if (FrameHeader)
		{
			SampleFrame();
		}

		PollFrameSlotCompletions();
		DrainCopyStats();

		const float TargetRate = Governor.Update(DeltaSeconds);
		RequestFrameIfDue(TargetRate);

		SET_FLOAT_STAT(STAT_UXWebview_TargetFPS, TargetRate);
		SET_DWORD_STAT(STAT_UXWebview_FramesReceived, ReceivedPerSecond);
		SET_DWORD_STAT(STAT_UXWebview_FramesSkipped, static_cast<uint32>(PoolDrops));
		SET_DWORD_STAT(STAT_UXWebview_PoolSwitches, static_cast<uint32>(PoolSwitches));
		SET_DWORD_STAT(STAT_UXWebview_SlotsBusy, static_cast<uint32>(
			FrameSlots.NumSlots() - FrameSlots.CountInState(EUXWebviewFrameSlotState::Free)));
	}

	const double Now = FPlatformTime::Seconds();
	if (WindowSeconds == 0.0)
	{
		WindowSeconds = Now;
	}
	else if (Now - WindowSeconds >= 1.0)
	{
		ReceivedPerSecond = static_cast<uint32>(ReceivedFrames - WindowFrames);
		WindowFrames = ReceivedFrames;
		WindowSeconds = Now;
	}

	if (SummarySeconds == 0.0)
	{
		SummarySeconds = Now;
		SummaryFrames = ReceivedFrames;
	}
	else if (Now - SummarySeconds >= kSummaryIntervalSeconds)
	{
		const double Elapsed = Now - SummarySeconds;
		const double Fps = static_cast<double>(ReceivedFrames - SummaryFrames) / Elapsed;
		const double CopyAvgMs = CopyCount > 0
			? static_cast<double>(CopyTotalMicros) / static_cast<double>(CopyCount) * 0.001 : 0.0;
		const double AgeAvgMs = FrameAgeSamples > 0
			? FrameAgeTotalMs / static_cast<double>(FrameAgeSamples) : 0.0;

		UE_LOG(LogUXWebviewCEFProvider, Log,
			TEXT("cef view %u: %.1f fps (target %.0f), copyAvg=%.2fms copyMax=%.2fms, "
				"ageAvg=%.2fms ageMax=%.2fms, drops=%llu"),
			InstanceId, Fps, Governor.GetTargetFrameRate(),
			CopyAvgMs, static_cast<double>(CopyMaxMicros) * 0.001,
			AgeAvgMs, FrameAgeMaxMs,
			static_cast<unsigned long long>(PoolDrops));

		SummarySeconds = Now;
		SummaryFrames = ReceivedFrames;
		CopyCount = 0;
		CopyTotalMicros = 0;
		CopyMaxMicros = 0;
		FrameAgeTotalMs = 0.0;
		FrameAgeMaxMs = 0.0;
		FrameAgeSamples = 0;
	}

	return true;
}

bool FUXWebviewCEFProvider::MapSharedMemory()
{
	if (FrameHeader)
	{
		return true;
	}
	if (InstanceId == 0)
	{
		return false;
	}

	if (SharedMemoryName.IsEmpty())
	{
		SharedMemoryName = FUXWebviewHostClient::Get().GetSharedMemoryName(InstanceId);
	}
	if (SharedMemoryName.IsEmpty())
	{
		return false;
	}

	FrameMapping = ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, 0, *SharedMemoryName);
	if (!FrameMapping)
	{
		return false;
	}

	const void* View = ::MapViewOfFile(FrameMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
		sizeof(UxFrameHeader));
	if (!View)
	{
		::CloseHandle(static_cast<HANDLE>(FrameMapping));
		FrameMapping = nullptr;
		return false;
	}

	FrameHeader = const_cast<void*>(View);
	LastSeq = 0;
	bLoggedBadHeader = false;
	HeldBuffer = INDEX_NONE;
	PreviousHeldBuffer = INDEX_NONE;

	// Direct display keeps pool buffers on screen, so the host must skip them
	// when it picks a copy target; the copy path leaves this at zero.
	UxFrameHeader* Header = static_cast<UxFrameHeader*>(FrameHeader);
	if (Header->Magic == UXWV_FRAME_MAGIC && Header->Version == UXWV_FRAME_VERSION)
	{
		Header->HoldBuffers = bDirectDisplay ? 1u : 0u;
		Header->ClientHeld = (static_cast<uint64>(UXWV_BUFFER_NONE) << 32) | UXWV_BUFFER_NONE;
	}

	UE_LOG(LogUXWebviewCEFProvider, Log,
		TEXT("instance %u mapped frame memory '%s' (%s)"), InstanceId, *SharedMemoryName,
		bDirectDisplay ? TEXT("direct hold") : TEXT("copy"));
	return true;
}

void FUXWebviewCEFProvider::UnmapSharedMemory()
{
	if (FrameHeader)
	{
		::UnmapViewOfFile(FrameHeader);
		FrameHeader = nullptr;
	}
	if (FrameMapping)
	{
		::CloseHandle(static_cast<HANDLE>(FrameMapping));
		FrameMapping = nullptr;
	}
}

void FUXWebviewCEFProvider::PublishHeldBuffer(uint32 BufferIndex)
{
	UxFrameHeader* Header = static_cast<UxFrameHeader*>(FrameHeader);
	if (!Header || !bDirectDisplay)
	{
		return;
	}

	const int32 Current = static_cast<int32>(BufferIndex);
	if (Current == HeldBuffer)
	{
		return;
	}

	PreviousHeldBuffer = HeldBuffer;
	HeldBuffer = Current;

	// The previous buffer can still be read by in flight GPU work, so the host
	// gets both and never copies into the two most recent ones.
	const uint32 PackedPrevious = PreviousHeldBuffer >= 0
		? static_cast<uint32>(PreviousHeldBuffer) : UXWV_BUFFER_NONE;
	const uint64 Packed = (static_cast<uint64>(PackedPrevious) << 32)
		| static_cast<uint32>(Current);
	FPlatformAtomics::InterlockedExchange(reinterpret_cast<volatile int64*>(&Header->ClientHeld),
		static_cast<int64>(Packed));
}

bool FUXWebviewCEFProvider::SampleFrame()
{
	if (!FrameHeader)
	{
		return false;
	}

	const UxFrameHeader* Header = static_cast<const UxFrameHeader*>(FrameHeader);
	const volatile uint32* SeqPtr = reinterpret_cast<const volatile uint32*>(&Header->Seq);
	const uint32 SeqA = *SeqPtr;
	if (SeqA == LastSeq)
	{
		return false;
	}

	// A torn header is retried on the next tick.
	const UxFrameHeader Local = *Header;
	std::atomic_thread_fence(std::memory_order_acquire);
	const uint32 SeqB = *SeqPtr;
	if (SeqA != SeqB)
	{
		return false;
	}

	if (Local.Magic != UXWV_FRAME_MAGIC || Local.Version != UXWV_FRAME_VERSION || Local.Seq == 0)
	{
		if (!bLoggedBadHeader)
		{
			bLoggedBadHeader = true;
			UE_LOG(LogUXWebviewCEFProvider, Warning,
				TEXT("unexpected frame header magic=0x%08X version=%u seq=%u"),
				Local.Magic, Local.Version, Local.Seq);
		}
		return false;
	}

	if (Local.PoolSize != UXWV_FRAME_POOL_SIZE || Local.BufferIndex >= UXWV_FRAME_POOL_SIZE
		|| Local.Width == 0 || Local.Height == 0)
	{
		return false;
	}

	const int32 FrameWidth = static_cast<int32>(Local.Width);
	const int32 FrameHeight = static_cast<int32>(Local.Height);

	FUXWebviewCefFrameSample Sample;
	Sample.Seq = Local.Seq;
	Sample.Generation = Local.Generation;
	Sample.BufferIndex = Local.BufferIndex;
	Sample.Width = FrameWidth;
	Sample.Height = FrameHeight;
	for (int32 Index = 0; Index < UXWV_FRAME_POOL_SIZE; ++Index)
	{
		Sample.Handles[Index] = Local.SharedHandles[Index];
	}

	if (bDirectDisplay)
	{
		if (!DirectTexture || FrameWidth != TextureWidth || FrameHeight != TextureHeight)
		{
			if (!CreateDirectTexture(FrameWidth, FrameHeight))
			{
				return false;
			}
		}
	}
	else
	{
		if (PoolTextures.Num() == 0 || FrameWidth != TextureWidth || FrameHeight != TextureHeight)
		{
			if (!CreateFramePool(FrameWidth, FrameHeight))
			{
				return false;
			}
		}

		// A full pool means the previous copies are still running; keep the old
		// display and retry this frame on the next tick.
		const int32 SlotIndex = FrameSlots.AcquireCopySlot();
		if (SlotIndex == INDEX_NONE || !PoolTextures.IsValidIndex(SlotIndex))
		{
			++PoolDrops;
			return false;
		}

		// A fresh pool publishes its RHI textures on the render thread, so keep
		// this frame pending until the destination exists instead of dropping it.
		UTexture2D* DestTexture = PoolTextures[SlotIndex];
		if (!DestTexture->GetResource() || !DestTexture->GetResource()->TextureRHI)
		{
			FrameSlots.CancelCopy(SlotIndex);
			return false;
		}

		Sample.SlotIndex = SlotIndex;
		Sample.DestTexture = DestTexture->GetResource()->TextureRHI;
	}

	if (Local.PublishTicks != 0)
	{
		const uint64 NowCycles = FPlatformTime::Cycles64();
		if (NowCycles > Local.PublishTicks)
		{
			const double AgeMs = FPlatformTime::ToMilliseconds64(NowCycles - Local.PublishTicks);
			FrameAgeTotalMs += AgeMs;
			FrameAgeMaxMs = FMath::Max(FrameAgeMaxMs, AgeMs);
			++FrameAgeSamples;
		}
	}

	// Only now is the frame consumed; earlier exits leave the seq untouched so
	// the next tick retries the same frame.
	LastSeq = SeqA;

	TSharedPtr<FUXWebviewCefRenderState> State = RenderState;
	if (bDirectDisplay)
	{
		// Claim the buffer before the bind command runs so the host does not
		// recycle it while this frame is on screen.
		PublishHeldBuffer(Sample.BufferIndex);

		UTexture2D* Target = DirectTexture;
		ENQUEUE_RENDER_COMMAND(UXWebviewCefBindFrame)(
			[State, Sample, Target](FRHICommandListImmediate& RHICmdList)
			{
				RenderBindFrame(RHICmdList, State, Sample, Target);
			});

		++ReceivedFrames;
		// The bind command runs before Slate draws this frame, so the frame is
		// on screen as soon as the tick finishes.
		ReleaseRetiredTextures();
		NotifyFrameShown();
		return true;
	}

	ENQUEUE_RENDER_COMMAND(UXWebviewCefCopyFrame)(
		[State, Sample](FRHICommandListImmediate& RHICmdList)
		{
			RenderFrameCopy(RHICmdList, State, Sample);
		});

	++ReceivedFrames;
	return true;
}

void FUXWebviewCEFProvider::PollFrameSlotCompletions()
{
	if (!RenderState.IsValid())
	{
		return;
	}

	FUXWebviewCefCopyResult Result;
	while (RenderState->Completed.Dequeue(Result))
	{
		if (Result.bSuccess)
		{
			FrameSlots.CompleteCopy(Result.SlotIndex);
		}
		else
		{
			FrameSlots.CancelCopy(Result.SlotIndex);
		}
	}

	const int32 NewDisplay = FrameSlots.TakeNewestReadySlot();
	if (NewDisplay == INDEX_NONE || !PoolTextures.IsValidIndex(NewDisplay))
	{
		return;
	}

	++PoolSwitches;
	DisplayTexture = PoolTextures[NewDisplay];

	// Release the old pool before notifying: the listener may destroy us.
	ReleaseRetiredTextures();
	NotifyFrameShown();
}

void FUXWebviewCEFProvider::NotifyFrameShown()
{
	if (!bFirstFrameLogged)
	{
		bFirstFrameLogged = true;
		UE_LOG(LogUXWebviewCEFProvider, Log, TEXT("instance %u first frame %dx%d"),
			InstanceId, TextureWidth, TextureHeight);
	}

	if (Client)
	{
		Client->OnViewFrame(TextureWidth, TextureHeight);
	}
}

void FUXWebviewCEFProvider::DrainCopyStats()
{
	if (!RenderState.IsValid())
	{
		return;
	}

	uint32 Micros = 0;
	while (RenderState->CopyMicros.Dequeue(Micros))
	{
		++CopyCount;
		CopyTotalMicros += Micros;
		CopyMaxMicros = FMath::Max(CopyMaxMicros, Micros);
	}
}

void FUXWebviewCEFProvider::ResetRenderSources()
{
	if (!RenderState.IsValid())
	{
		return;
	}

	TSharedPtr<FUXWebviewCefRenderState> State = RenderState;
	if (IsInGameThread() && GDynamicRHI)
	{
		ENQUEUE_RENDER_COMMAND(UXWebviewCefResetSources)(
			[State](FRHICommandListImmediate&)
			{
				State->CachedSources.Empty();
				State->CachedGeneration = 0;
				State->bLoggedOpenFailure = false;
			});
		FlushRenderingCommands();
	}

	FUXWebviewCefCopyResult Result;
	while (State->Completed.Dequeue(Result))
	{
	}
	uint32 Micros = 0;
	while (State->CopyMicros.Dequeue(Micros))
	{
	}
	State->CachedSources.Empty();
	State->CachedGeneration = 0;
}

void FUXWebviewCEFProvider::RequestFrameIfDue(float TargetRate)
{
	if (!bBeginFrameEnabled || !bReady || InstanceId == 0 || TargetRate <= 0.0f)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	const double Interval = 1.0 / static_cast<double>(TargetRate);
	if (NextBeginFrameSeconds > 0.0 && Now < NextBeginFrameSeconds)
	{
		return;
	}

	// This tick can display at most one new frame, so never request more than
	// one per tick; a higher target simply means "every game frame".
	FUXWebviewHostClient::Get().BeginFrame(InstanceId);

	// Advancing by the interval (instead of resetting to Now) keeps the cadence
	// on a stable grid; Max() discards the backlog if a tick was very late.
	NextBeginFrameSeconds = FMath::Max(NextBeginFrameSeconds + Interval, Now);
}

void FUXWebviewCEFProvider::PollScriptTimeouts()
{
	if (PendingScripts.Num() == 0)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	TArray<uint32> Expired;
	for (const TPair<uint32, TPair<FUXWebviewScriptCallback, double>>& Pair : PendingScripts)
	{
		if (Now - Pair.Value.Value >= kScriptTimeoutSeconds)
		{
			Expired.Add(Pair.Key);
		}
	}

	for (const uint32 RequestId : Expired)
	{
		TPair<FUXWebviewScriptCallback, double>* Found = PendingScripts.Find(RequestId);
		if (!Found)
		{
			continue;
		}

		FUXWebviewScriptCallback Callback = MoveTemp(Found->Key);
		PendingScripts.Remove(RequestId);
		if (Callback)
		{
			Callback(false, TEXT("timeout"));
		}
	}
}

void FUXWebviewCEFProvider::PollBridgeTimeouts()
{
	if (PendingBridge.Num() == 0)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	TArray<uint32> Finished;
	for (const TPair<uint32, TSharedPtr<FUXWebviewBridgeRequest>>& Pair : PendingBridge)
	{
		if (!Pair.Value.IsValid() || Pair.Value->bResolved || Now >= Pair.Value->Deadline)
		{
			Finished.Add(Pair.Key);
		}
	}

	for (const uint32 RequestId : Finished)
	{
		TSharedPtr<FUXWebviewBridgeRequest> Request = PendingBridge.FindRef(RequestId);
		PendingBridge.Remove(RequestId);
		if (Request.IsValid() && !Request->bResolved)
		{
			Request->bResolved = true;
			FUXWebviewHostClient::Get().ReplyBridge(
				InstanceId, RequestId, false, TEXT("timeout"));
		}
	}
}

bool FUXWebviewCEFProvider::CreateDirectTexture(int32 InWidth, int32 InHeight)
{
	if (InWidth <= 0 || InHeight <= 0)
	{
		return false;
	}

	UTexture2D* NewTexture = UTexture2D::CreateTransient(InWidth, InHeight, PF_B8G8R8A8);
	if (!NewTexture)
	{
		UE_LOG(LogUXWebviewCEFProvider, Error,
			TEXT("CreateTransient failed for direct display %dx%d"), InWidth, InHeight);
		return false;
	}

	NewTexture->AddToRoot();
	NewTexture->SRGB = true;
	NewTexture->Filter = TF_Bilinear;
	NewTexture->AddressX = TA_Clamp;
	NewTexture->AddressY = TA_Clamp;
	NewTexture->UpdateResource();

	// The old texture stays on screen until the new one has a frame, and the
	// transient allocation of the placeholder is released on the first bind.
	if (DirectTexture)
	{
		RetiredTextures.Add(DirectTexture);
	}

	DirectTexture = NewTexture;
	DisplayTexture = NewTexture;
	TextureWidth = InWidth;
	TextureHeight = InHeight;
	return true;
}

bool FUXWebviewCEFProvider::CreateFramePool(int32 InWidth, int32 InHeight)
{
	if (InWidth <= 0 || InHeight <= 0)
	{
		return false;
	}

	// The old pool stays on screen until a frame of the new pool is finished.
	RetiredTextures.Append(PoolTextures);
	PoolTextures.Reset();

	for (int32 Index = 0; Index < 3; ++Index)
	{
		UTexture2D* NewTexture = UTexture2D::CreateTransient(InWidth, InHeight, PF_B8G8R8A8);
		if (!NewTexture)
		{
			UE_LOG(LogUXWebviewCEFProvider, Error,
				TEXT("CreateTransient failed for %dx%d"), InWidth, InHeight);
			for (UTexture2D* Texture : PoolTextures)
			{
				Texture->RemoveFromRoot();
				Texture->MarkAsGarbage();
			}
			PoolTextures.Reset();
			return false;
		}

		NewTexture->AddToRoot();
		NewTexture->SRGB = true;
		NewTexture->Filter = TF_Bilinear;
		NewTexture->AddressX = TA_Clamp;
		NewTexture->AddressY = TA_Clamp;
		NewTexture->UpdateResource();
		PoolTextures.Add(NewTexture);
	}

	FrameSlots.Reset(PoolTextures.Num());
	TextureWidth = InWidth;
	TextureHeight = InHeight;
	return true;
}

void FUXWebviewCEFProvider::ReleaseRetiredTextures()
{
	if (RetiredTextures.Num() == 0)
	{
		return;
	}

	// Copies into the retired pool are already flushed; drop them safely.
	if (IsInGameThread())
	{
		FlushRenderingCommands();
	}
	for (UTexture2D* Texture : RetiredTextures)
	{
		if (Texture)
		{
			Texture->RemoveFromRoot();
			Texture->MarkAsGarbage();
		}
	}
	RetiredTextures.Reset();
}

void FUXWebviewCEFProvider::ReleaseFramePool()
{
	for (UTexture2D* Texture : PoolTextures)
	{
		if (Texture)
		{
			Texture->RemoveFromRoot();
			Texture->MarkAsGarbage();
		}
	}
	PoolTextures.Reset();

	if (DirectTexture)
	{
		DirectTexture->RemoveFromRoot();
		DirectTexture->MarkAsGarbage();
		DirectTexture = nullptr;
	}

	TextureWidth = 0;
	TextureHeight = 0;
}

void RegisterCEFProvider()
{
	FUXWebviewProviderRegistry::Get().RegisterFactory(MakeShared<FUXWebviewCEFFactory>());
	UE_LOG(LogUXWebviewCEFProvider, Log, TEXT("cef provider registered"));
}

void UnregisterCEFProvider()
{
	FUXWebviewProviderRegistry::Get().UnregisterFactory(TEXT("CEF"));
}

#endif // PLATFORM_WINDOWS
