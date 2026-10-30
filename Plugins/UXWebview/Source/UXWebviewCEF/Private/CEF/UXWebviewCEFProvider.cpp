#include "CEF/UXWebviewCEFProvider.h"

#if PLATFORM_WINDOWS

#include <atomic>

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "CEF/UXWebviewSharedTexture.h"
#include "Engine/Texture2D.h"
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
// High refresh displays go well past 120 Hz; the ticker only ever asks for one
// frame per game frame, so a higher ceiling costs nothing when it is unused.
constexpr int32 kMaxFrameRate = 240;
constexpr double kScriptTimeoutSeconds = 5.0;
constexpr double kSummaryIntervalSeconds = 5.0;

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

	FUXWebviewGovernorConfig GovernorConfig;
	const float ActiveRate = bBeginFrameEnabled ? static_cast<float>(TargetFrameRate) : 60.0f;
	GovernorConfig.ActiveFrameRate = ActiveRate;
	GovernorConfig.MaxFrameRate = FMath::Max(ActiveRate, 120.0f);
	Governor.SetConfig(GovernorConfig);
	Governor.SetVisible(true);
	Governor.SetAnimated(true);

	RenderState = MakeShared<FUXWebviewCefRenderState>();
	if (!CreateFramePool(ViewSize.X, ViewSize.Y))
	{
		RenderState.Reset();
		Client = nullptr;
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			TEXT("cef provider could not create its frame textures"));
	}

	bInitialized = true;

	FUXWebviewHostInstanceSpec Spec;
	Spec.Size = ViewSize;
	SentDpiScale = DpiScale;
	Spec.DpiScale = DpiScale;
	Spec.Url = CurrentUrl.IsEmpty() ? FString(TEXT("about:blank")) : CurrentUrl;
	Spec.FrameRate = bBeginFrameEnabled ? TargetFrameRate : 60;
	Spec.bVisible = true;

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
		TEXT("cef view %u created: %dx%d scale=%.2f fps=%d url='%s'"),
		InstanceId, ViewSize.X, ViewSize.Y, Spec.DpiScale, Spec.FrameRate, *Spec.Url);
	return FUXWebviewResult::Ok();
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
	(void)EventName;
	(void)PayloadJson;
	return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
		TEXT("cef script bridge arrives with M6"));
}

FUXWebviewResult FUXWebviewCEFProvider::SendInput(const FUXWebviewInputEvent& Event)
{
	const FUXWebviewResult Guard = GuardInitialized();
	if (!Guard.bSuccess)
	{
		return Guard;
	}

	Governor.NotifyInteraction();
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
	MapSharedMemory();
	UE_LOG(LogUXWebviewCEFProvider, Log, TEXT("host instance %u ready"), InstanceId);

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
	MapSharedMemory();

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

void FUXWebviewCEFProvider::OnHostDevTools(const FString& Url)
{
	UE_LOG(LogUXWebviewCEFProvider, Log, TEXT("instance %u devtools at '%s'"), InstanceId, *Url);
}

bool FUXWebviewCEFProvider::Tick(float DeltaSeconds)
{
	if (!bInitialized)
	{
		return false;
	}

	PollScriptTimeouts();

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

	FrameMapping = ::OpenFileMappingW(FILE_MAP_READ, 0, *SharedMemoryName);
	if (!FrameMapping)
	{
		return false;
	}

	const void* View = ::MapViewOfFile(FrameMapping, FILE_MAP_READ, 0, 0, sizeof(UxFrameHeader));
	if (!View)
	{
		::CloseHandle(static_cast<HANDLE>(FrameMapping));
		FrameMapping = nullptr;
		return false;
	}

	FrameHeader = View;
	LastSeq = 0;
	bLoggedBadHeader = false;
	UE_LOG(LogUXWebviewCEFProvider, Log,
		TEXT("instance %u mapped frame memory '%s'"), InstanceId, *SharedMemoryName);
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

	FUXWebviewCefFrameSample Sample;
	Sample.Seq = Local.Seq;
	Sample.Generation = Local.Generation;
	Sample.BufferIndex = Local.BufferIndex;
	Sample.SlotIndex = SlotIndex;
	Sample.Width = FrameWidth;
	Sample.Height = FrameHeight;
	for (int32 Index = 0; Index < UXWV_FRAME_POOL_SIZE; ++Index)
	{
		Sample.Handles[Index] = Local.SharedHandles[Index];
	}
	Sample.DestTexture = DestTexture->GetResource()->TextureRHI;

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

	if (!bFirstFrameLogged)
	{
		bFirstFrameLogged = true;
		UE_LOG(LogUXWebviewCEFProvider, Log, TEXT("instance %u first frame %dx%d"),
			InstanceId, TextureWidth, TextureHeight);
	}

	// Release the old pool before notifying: the listener may destroy us.
	ReleaseRetiredTextures();

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
