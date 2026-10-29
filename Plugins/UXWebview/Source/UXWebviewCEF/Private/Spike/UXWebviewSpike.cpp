#include "Spike/UXWebviewSpike.h"

#if PLATFORM_WINDOWS

#include <atomic>

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RHITransition.h"
#include "Styling/SlateBrush.h"
#include "RenderingThread.h"
#include "Widgets/Images/SImage.h"

#include "ID3D11DynamicRHI.h"
#include "ID3D12DynamicRHI.h"

#include "uxwebview_frame.h"
#include "uxwebview_ipc.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewSpike, Log, All);

namespace
{
constexpr int32 kMaxViewSize = 4096;
constexpr double kHostStartTimeoutSeconds = 20.0;
uint32 GSpikeSessionCounter = 0;

FString QuoteArg(const FString& Value)
{
	return Value.Contains(TEXT(" ")) ? FString::Printf(TEXT("\"%s\""), *Value) : Value;
}

FString MakeFileUrl(const FString& Path)
{
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	Full.ReplaceInline(TEXT("\\"), TEXT("/"));
	return FString(TEXT("file:///")) + Full;
}

FString DefaultBenchUrl()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UXWebview"));
	if (Plugin.IsValid())
	{
		return MakeFileUrl(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Web/bench.html")));
	}
	return FString();
}

uint64 GetAdapterLuid()
{
	ID3D12DynamicRHI* RHI = GetID3D12DynamicRHI();
	ID3D12Device* Device = RHI ? RHI->RHIGetDevice_NoMGPU() : nullptr;
	if (!Device)
	{
		return 0;
	}

	// LUID from the device itself: IDXGIDevice is not exposed by D3D12 devices.
	const LUID Luid = Device->GetAdapterLuid();
	return (static_cast<uint64>(Luid.HighPart) << 32) | Luid.LowPart;
}

FTextureRHIRef WrapSharedTextureD3D12(uint64 HandleValue)
{
	ID3D12DynamicRHI* RHI = GetID3D12DynamicRHI();
	ID3D12Device* Device = RHI->RHIGetDevice_NoMGPU();
	if (!Device)
	{
		UE_LOG(LogUXWebviewSpike, Error, TEXT("D3D12 device is null"));
		return nullptr;
	}

	ID3D12Resource* Resource = nullptr;
	const HRESULT Hr = Device->OpenSharedHandle(
		reinterpret_cast<HANDLE>(HandleValue), IID_PPV_ARGS(&Resource));
	if (FAILED(Hr) || Resource == nullptr)
	{
		UE_LOG(LogUXWebviewSpike, Error,
			TEXT("D3D12 OpenSharedHandle failed, hr=0x%08X"), static_cast<uint32>(Hr));
		return nullptr;
	}

	FTextureRHIRef Wrapped = RHI->RHICreateTexture2DFromResource(
		PF_B8G8R8A8, TexCreate_ShaderResource, FClearValueBinding::None, Resource);
	Resource->Release();

	if (!Wrapped)
	{
		UE_LOG(LogUXWebviewSpike, Error,
			TEXT("D3D12 RHICreateTexture2DFromResource returned null"));
	}
	return Wrapped;
}

FTextureRHIRef WrapSharedTextureD3D11(uint64 HandleValue)
{
	ID3D11DynamicRHI* RHI = GetID3D11DynamicRHI();
	ID3D11Device* Device = RHI->RHIGetDevice();
	if (!Device)
	{
		UE_LOG(LogUXWebviewSpike, Error, TEXT("D3D11 device is null"));
		return nullptr;
	}

	ID3D11Device1* Device1 = nullptr;
	const HRESULT QueryHr = Device->QueryInterface(IID_PPV_ARGS(&Device1));
	if (FAILED(QueryHr) || Device1 == nullptr)
	{
		UE_LOG(LogUXWebviewSpike, Error,
			TEXT("ID3D11Device1 unavailable, hr=0x%08X"), static_cast<uint32>(QueryHr));
		return nullptr;
	}

	ID3D11Texture2D* Resource = nullptr;
	const HRESULT OpenHr = Device1->OpenSharedResource1(
		reinterpret_cast<HANDLE>(HandleValue), IID_PPV_ARGS(&Resource));
	Device1->Release();
	if (FAILED(OpenHr) || Resource == nullptr)
	{
		UE_LOG(LogUXWebviewSpike, Error,
			TEXT("D3D11 OpenSharedResource1 failed, hr=0x%08X"), static_cast<uint32>(OpenHr));
		return nullptr;
	}

	FTextureRHIRef Wrapped = RHI->RHICreateTexture2DFromResource(
		PF_B8G8R8A8, TexCreate_ShaderResource, FClearValueBinding::None, Resource);
	Resource->Release();
	return Wrapped;
}

FTextureRHIRef WrapSharedTexture(uint64 HandleValue)
{
	if (IsRHID3D12())
	{
		return WrapSharedTextureD3D12(HandleValue);
	}
	if (IsRHID3D11())
	{
		return WrapSharedTextureD3D11(HandleValue);
	}
	UE_LOG(LogUXWebviewSpike, Error, TEXT("spike requires a D3D12 or D3D11 RHI"));
	return nullptr;
}
} // namespace

FUXWebviewSpike& FUXWebviewSpike::Get()
{
	static FUXWebviewSpike Instance;
	return Instance;
}

bool FUXWebviewSpike::Start(int32 InWidth, int32 InHeight, const FString& InUrl, int32 InTargetFps)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogUXWebviewSpike, Error, TEXT("SpikeStart must run on the game thread"));
		return false;
	}
	if (HostProcess.IsValid() || bConnected || bTearingDown)
	{
		UE_LOG(LogUXWebviewSpike, Warning, TEXT("spike is already running"));
		return false;
	}

	Width = FMath::Clamp(InWidth, 64, kMaxViewSize);
	Height = FMath::Clamp(InHeight, 64, kMaxViewSize);
	TargetFps = FMath::Clamp(InTargetFps, 0, 240);
	Url = InUrl.IsEmpty() ? DefaultBenchUrl() : InUrl;

	if (!Url.Contains(TEXT("://")) && FPaths::FileExists(Url))
	{
		Url = MakeFileUrl(Url);
	}
	if (Url.IsEmpty())
	{
		UE_LOG(LogUXWebviewSpike, Error, TEXT("no url for the spike"));
		return false;
	}

	LastSeq = 0;
	ReportedGeneration = 0;
	CopyCount = 0;
	CopyTotalMicros = 0;
	CopyMaxMicros = 0;
	ReceivedFrames = 0;
	ReceivedPerSecond = 0;
	WindowFrames = 0;
	WindowSeconds = 0.0;
	LastBeginFrameSeconds = 0.0;
	ConnectionAttempts = 0;
	bHelloSent = false;
	bConnected = false;
	bLoggedBadHeader = false;

	if (!StartHostProcess())
	{
		return false;
	}

	LaunchSeconds = FPlatformTime::Seconds();
	NextConnectAttempt = LaunchSeconds;
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FUXWebviewSpike::Tick));

	UE_LOG(LogUXWebviewSpike, Log, TEXT("host launched, waiting for IPC '%s'"), *PipeName);
	return true;
}

bool FUXWebviewSpike::StartHostProcess()
{
	TArray<FString> Candidates;

	const FString EnvHost = FPlatformMisc::GetEnvironmentVariable(TEXT("UXWEBVIEW_HOST_EXE"));
	if (!EnvHost.IsEmpty())
	{
		Candidates.Add(EnvHost);
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UXWebview"));
	if (Plugin.IsValid())
	{
		const FString PluginBase = Plugin->GetBaseDir();
		Candidates.Add(FPaths::Combine(PluginBase, TEXT("Binaries/Win64/UXWebviewHost.exe")));
		Candidates.Add(FPaths::Combine(PluginBase, TEXT("../../native/build/Release/UXWebviewHost.exe")));
	}
	Candidates.Add(FPaths::Combine(FPaths::ProjectDir(),
		TEXT("Binaries/ThirdParty/UXWebviewHost/Win64/UXWebviewHost.exe")));

	for (const FString& Candidate : Candidates)
	{
		const FString FullPath = FPaths::ConvertRelativePathToFull(Candidate);
		if (FPaths::FileExists(FullPath))
		{
			HostExePath = FullPath;
			break;
		}
	}

	if (HostExePath.IsEmpty())
	{
		UE_LOG(LogUXWebviewSpike, Error,
			TEXT("UXWebviewHost.exe not found; set UXWEBVIEW_HOST_EXE or build native/host"));
		return false;
	}

	HostWorkingDir = FPaths::GetPath(HostExePath);

	const FString CacheDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UXWebviewHostCache"));
	const FString HostLogPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs/UXWebviewHost.log"));
	IFileManager::Get().MakeDirectory(*CacheDir, true);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(HostLogPath), true);

	++GSpikeSessionCounter;
	const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
	PipeName = FString::Printf(TEXT("UXWebviewSpike-%u-%u"), Pid, GSpikeSessionCounter);
	SharedMemoryName = FString::Printf(TEXT("UXWebviewFrame-%u-%u"), Pid, GSpikeSessionCounter);

	const uint64 AdapterLuid = GetAdapterLuid();
	const FString Args = FString::Printf(
		TEXT("--ipc %s --shmem %s --url %s --width %d --height %d --fps %d --owner-pid %u --luid 0x%llX --cache %s --log %s --log-level 2 --no-sandbox"),
		*PipeName, *SharedMemoryName, *QuoteArg(Url), Width, Height, TargetFps,
		Pid, AdapterLuid, *QuoteArg(CacheDir), *QuoteArg(HostLogPath));

	UE_LOG(LogUXWebviewSpike, Log, TEXT("RHI adapter LUID 0x%llX"), AdapterLuid);

	uint32 ProcessId = 0;
	HostProcess = FPlatformProcess::CreateProc(
		*HostExePath, *Args, true, false, false, &ProcessId, 0, *HostWorkingDir, nullptr, nullptr);

	if (!HostProcess.IsValid())
	{
		UE_LOG(LogUXWebviewSpike, Error, TEXT("failed to launch '%s'"), *HostExePath);
		return false;
	}

	UE_LOG(LogUXWebviewSpike, Log,
		TEXT("launched '%s' pid=%u args='%s'"), *HostExePath, ProcessId, *Args);
	return true;
}

bool FUXWebviewSpike::TryConnectAndSetup()
{
	if (!HostProcess.IsValid() || !FPlatformProcess::IsProcRunning(HostProcess))
	{
		UE_LOG(LogUXWebviewSpike, Error, TEXT("host process exited during startup"));
		Stop();
		return false;
	}

	if (!PipeHandle)
	{
		const FString FullPipe = FString::Printf(TEXT("\\\\.\\pipe\\%s"), *PipeName);
		HANDLE Pipe = ::CreateFileW(*FullPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
			OPEN_EXISTING, 0, nullptr);
		if (Pipe == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		PipeHandle = Pipe;
	}

	if (!bHelloSent)
	{
		SendIpc(UXWV_IPC_HELLO);
		bHelloSent = true;
		UE_LOG(LogUXWebviewSpike, Log, TEXT("IPC connected"));
	}

	if (!FrameHeader)
	{
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
		UE_LOG(LogUXWebviewSpike, Log, TEXT("frame shared memory mapped"));
	}

	if (!Texture)
	{
		if (!GEngine || !GEngine->GameViewport)
		{
			// The game viewport may still be loading; keep retrying from Tick.
			return false;
		}
		if (!CreateTextureAndWidget(Width, Height))
		{
			Stop();
			return false;
		}
	}

	bConnected = true;
	NextHealthCheck = FPlatformTime::Seconds() + 1.0;
	WindowSeconds = FPlatformTime::Seconds();
	UE_LOG(LogUXWebviewSpike, Log, TEXT("spike running: %dx%d url='%s'"), Width, Height, *Url);
	return true;
}

bool FUXWebviewSpike::CreateTextureAndWidget(int32 InWidth, int32 InHeight)
{
	if (!GEngine || !GEngine->GameViewport)
	{
		UE_LOG(LogUXWebviewSpike, Error, TEXT("no game viewport; run inside PIE"));
		return false;
	}

	UTexture2D* NewTexture = UTexture2D::CreateTransient(InWidth, InHeight, PF_B8G8R8A8);
	if (!NewTexture)
	{
		UE_LOG(LogUXWebviewSpike, Error, TEXT("CreateTransient failed for %dx%d"), InWidth, InHeight);
		return false;
	}
	NewTexture->AddToRoot();
	NewTexture->SRGB = true;
	NewTexture->Filter = TF_Bilinear;
	NewTexture->AddressX = TA_Clamp;
	NewTexture->AddressY = TA_Clamp;
	NewTexture->UpdateResource();

	if (Texture)
	{
		// Frame copies may still reference the old texture on the render thread.
		FlushRenderingCommands();
		Texture->RemoveFromRoot();
		Texture->MarkAsGarbage();
	}

	Texture = NewTexture;
	TextureWidth = InWidth;
	TextureHeight = InHeight;

	if (!Brush)
	{
		Brush = MakeShared<FSlateBrush>();
		Brush->DrawAs = ESlateBrushDrawType::Image;
	}
	Brush->ImageSize = FVector2D(InWidth, InHeight);
	Brush->SetResourceObject(Texture);

	if (!ImageWidget)
	{
		SAssignNew(ImageWidget, SImage).Image(Brush.Get());
		GEngine->GameViewport->AddViewportWidgetContent(ImageWidget.ToSharedRef(), 0);
	}

	UE_LOG(LogUXWebviewSpike, Log, TEXT("frame texture %dx%d ready"), InWidth, InHeight);
	return true;
}

bool FUXWebviewSpike::Tick(float DeltaTime)
{
	if (bTearingDown)
	{
		return false;
	}

	const double Now = FPlatformTime::Seconds();

	if (!bConnected)
	{
		if (Now - LaunchSeconds > kHostStartTimeoutSeconds)
		{
			UE_LOG(LogUXWebviewSpike, Error,
				TEXT("timed out waiting for host after %u attempts"), ConnectionAttempts);
			Stop();
			return false;
		}
		if (Now >= NextConnectAttempt)
		{
			NextConnectAttempt = Now + 0.05;
			++ConnectionAttempts;
			TryConnectAndSetup();
		}
		return true;
	}

	if (!GEngine || !GEngine->GameViewport)
	{
		UE_LOG(LogUXWebviewSpike, Warning, TEXT("game viewport went away, stopping spike"));
		Stop();
		return false;
	}

	bool bCopied = false;
	SampleFrame(bCopied);
	DrainStats();
	PollPixelReadback();

	if (bCopied && ImageWidget.IsValid() && FSlateApplication::IsInitialized())
	{
		ImageWidget->Invalidate(EInvalidateWidgetReason::Paint);
	}

	RequestFrameIfDue();

	if (Now >= NextHealthCheck)
	{
		NextHealthCheck = Now + 1.0;
		DWORD Available = 0;
		if (PipeHandle && !::PeekNamedPipe(static_cast<HANDLE>(PipeHandle), nullptr, 0,
			nullptr, &Available, nullptr))
		{
			UE_LOG(LogUXWebviewSpike, Warning, TEXT("IPC pipe closed, stopping spike"));
			Stop();
			return false;
		}
	}

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

	return true;
}

bool FUXWebviewSpike::SampleFrame(bool& bOutCopiedThisTick)
{
	bOutCopiedThisTick = false;
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

	// Double-read the sequence so a torn header is retried on the next tick.
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
			UE_LOG(LogUXWebviewSpike, Warning,
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

	LastSeq = SeqA;

	const int32 FrameWidth = static_cast<int32>(Local.Width);
	const int32 FrameHeight = static_cast<int32>(Local.Height);
	if (!Texture || FrameWidth != TextureWidth || FrameHeight != TextureHeight)
	{
		if (!CreateTextureAndWidget(FrameWidth, FrameHeight))
		{
			return false;
		}
	}

	if (!Texture->GetResource() || !Texture->GetResource()->TextureRHI)
	{
		return false;
	}

	FUXWebviewFrameSample Sample;
	Sample.Seq = Local.Seq;
	Sample.Generation = Local.Generation;
	Sample.BufferIndex = Local.BufferIndex;
	Sample.Width = FrameWidth;
	Sample.Height = FrameHeight;
	for (int32 Index = 0; Index < UXWV_FRAME_POOL_SIZE; ++Index)
	{
		Sample.Handles[Index] = Local.SharedHandles[Index];
	}
	Sample.DestTexture = Texture->GetResource()->TextureRHI;

	ReportedGeneration = Local.Generation;
	EnqueueFrameCopy(Sample);

	++ReceivedFrames;
	bOutCopiedThisTick = true;
	return true;
}

void FUXWebviewSpike::EnqueueFrameCopy(const FUXWebviewFrameSample& Sample)
{
	ENQUEUE_RENDER_COMMAND(UXWebviewSpikeCopyFrame)(
		[this, Sample](FRHICommandListImmediate& RHICmdList)
		{
			RenderFrameCopy(RHICmdList, Sample);
		});
}

void FUXWebviewSpike::RenderFrameCopy(FRHICommandListImmediate& RHICmdList, FUXWebviewFrameSample Sample)
{
	if (!Sample.DestTexture)
	{
		return;
	}

	if (Sample.Generation != CachedGeneration || CachedSources.Num() != UXWV_FRAME_POOL_SIZE)
	{
		CachedSources.Empty();
		CachedSources.Reserve(UXWV_FRAME_POOL_SIZE);
		for (int32 Index = 0; Index < UXWV_FRAME_POOL_SIZE; ++Index)
		{
			CachedSources.Add(WrapSharedTexture(Sample.Handles[Index]));
		}
		CachedGeneration = Sample.Generation;

		// The opened resources start in COMMON; tell the RHI where they are now.
		for (const FTextureRHIRef& Source : CachedSources)
		{
			if (Source.IsValid())
			{
				RHICmdList.Transition(FRHITransitionInfo(Source.GetReference(),
					ERHIAccess::Unknown, ERHIAccess::SRVMask));
			}
		}
	}

	if (!CachedSources.IsValidIndex(static_cast<int32>(Sample.BufferIndex))
		|| !CachedSources[Sample.BufferIndex].IsValid())
	{
		if (!bLoggedOpenFailure)
		{
			bLoggedOpenFailure = true;
			UE_LOG(LogUXWebviewSpike, Error, TEXT("no shared texture for buffer %u"), Sample.BufferIndex);
		}
		return;
	}

	const uint64 StartCycles = FPlatformTime::Cycles64();
	RHICmdList.CopyTexture(CachedSources[Sample.BufferIndex].GetReference(),
		Sample.DestTexture, FRHICopyTextureInfo());
	const double ElapsedMs = FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles);

	CopyMicros.Enqueue(static_cast<uint32>(FMath::RoundToInt(ElapsedMs * 1000.0)));
}

void FUXWebviewSpike::ReadbackPixels()
{
	RequestPixelReadback();
}

void FUXWebviewSpike::RequestPixelReadback()
{
	if (!Texture || !Texture->GetResource())
	{
		UE_LOG(LogUXWebviewSpike, Warning, TEXT("pixel readback skipped: no texture"));
		return;
	}

	if (!PixelReadback.IsValid())
	{
		PixelReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("UXWebviewSpikePixels"));
	}
	PixelReadback->ResetReady();

	FRHITexture* Source = Texture->GetResource()->TextureRHI;
	FRHIGPUTextureReadback* Readback = PixelReadback.Get();
	const int32 ReadWidth = TextureWidth;
	const int32 ReadHeight = TextureHeight;

	ENQUEUE_RENDER_COMMAND(UXWebviewSpikePixelReadback)(
		[Readback, Source, ReadWidth, ReadHeight](FRHICommandListImmediate& RHICmdList)
		{
			Readback->EnqueueCopy(RHICmdList, Source, FIntVector::ZeroValue, 0,
				FIntVector(ReadWidth, ReadHeight, 1));
		});

	bPixelReadbackPending = true;
	UE_LOG(LogUXWebviewSpike, Log, TEXT("pixel readback requested (%dx%d)"), ReadWidth, ReadHeight);
}

void FUXWebviewSpike::PollPixelReadback()
{
	if (!bPixelReadbackPending || !PixelReadback.IsValid() || !PixelReadback->IsReady())
	{
		return;
	}

	bPixelReadbackPending = false;

	// Mapping a staging surface requires the render thread.
	FRHIGPUTextureReadback* Readback = PixelReadback.Get();
	const int32 ReadWidth = TextureWidth;
	const int32 ReadHeight = TextureHeight;

	ENQUEUE_RENDER_COMMAND(UXWebviewSpikePixelSample)(
		[Readback, ReadWidth, ReadHeight](FRHICommandListImmediate&)
		{
			int32 RowPitchPixels = 0;
			uint8* Data = static_cast<uint8*>(Readback->Lock(RowPitchPixels));
			if (!Data || RowPitchPixels <= 0)
			{
				UE_LOG(LogUXWebviewSpike, Error, TEXT("pixel readback lock failed"));
				return;
			}

			uint64 SumR = 0;
			uint64 SumG = 0;
			uint64 SumB = 0;
			uint64 Samples = 0;
			uint64 NonBlack = 0;
			const uint8* FirstPixel = Data;

			for (int32 Y = 0; Y < ReadHeight; Y += 16)
			{
				const uint8* Row = Data + static_cast<int64>(Y) * RowPitchPixels * 4;
				for (int32 X = 0; X < ReadWidth; X += 16)
				{
					const uint8 B = Row[X * 4 + 0];
					const uint8 G = Row[X * 4 + 1];
					const uint8 R = Row[X * 4 + 2];
					SumB += B;
					SumG += G;
					SumR += R;
					++Samples;
					if (R > 6 || G > 6 || B > 6)
					{
						++NonBlack;
					}
				}
			}

			Readback->Unlock();

			const uint32 AvgR = Samples ? static_cast<uint32>(SumR / Samples) : 0;
			const uint32 AvgG = Samples ? static_cast<uint32>(SumG / Samples) : 0;
			const uint32 AvgB = Samples ? static_cast<uint32>(SumB / Samples) : 0;
			const uint32 NonBlackPercent =
				Samples ? static_cast<uint32>(NonBlack * 100 / Samples) : 0;

			UE_LOG(LogUXWebviewSpike, Log,
				TEXT("pixel readback %dx%d: avg=(%u,%u,%u) nonBlack=%u%% first=(%u,%u,%u)"),
				ReadWidth, ReadHeight, AvgR, AvgG, AvgB, NonBlackPercent,
				FirstPixel[2], FirstPixel[1], FirstPixel[0]);
		});
}

void FUXWebviewSpike::DrainStats()
{
	uint32 Micros = 0;
	while (CopyMicros.Dequeue(Micros))
	{
		++CopyCount;
		CopyTotalMicros += Micros;
		CopyMaxMicros = FMath::Max(CopyMaxMicros, Micros);
	}
}

void FUXWebviewSpike::RequestFrameIfDue()
{
	if (TargetFps <= 0 || !PipeHandle)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	const double Interval = 1.0 / static_cast<double>(TargetFps);
	if (LastBeginFrameSeconds == 0.0 || Now - LastBeginFrameSeconds >= Interval)
	{
		SendIpc(UXWV_IPC_BEGIN_FRAME);
		LastBeginFrameSeconds = Now;
	}
}

void FUXWebviewSpike::SendIpc(uint32 Type, uint32 A, uint32 B)
{
	if (!PipeHandle)
	{
		return;
	}

	UxIpcMessage Message{};
	Message.Type = Type;
	Message.ProtocolVersion = UXWV_IPC_VERSION;
	Message.A = A;
	Message.B = B;
	Message.Instance = 1;

	DWORD Written = 0;
	if (!::WriteFile(static_cast<HANDLE>(PipeHandle), &Message, sizeof(Message), &Written, nullptr))
	{
		UE_LOG(LogUXWebviewSpike, Warning, TEXT("IPC write failed, type=%u"), Type);
	}
}

void FUXWebviewSpike::Stop()
{
	if (bTearingDown)
	{
		return;
	}
	bTearingDown = true;

	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (PipeHandle)
	{
		SendIpc(UXWV_IPC_SHUTDOWN);
		::CloseHandle(static_cast<HANDLE>(PipeHandle));
		PipeHandle = nullptr;
	}

	if (HostProcess.IsValid())
	{
		for (int32 Attempt = 0; Attempt < 50 && FPlatformProcess::IsProcRunning(HostProcess); ++Attempt)
		{
			FPlatformProcess::Sleep(0.02f);
		}
		if (FPlatformProcess::IsProcRunning(HostProcess))
		{
			UE_LOG(LogUXWebviewSpike, Warning, TEXT("host did not exit, terminating"));
			FPlatformProcess::TerminateProc(HostProcess, true);
		}
		FPlatformProcess::CloseProc(HostProcess);
		HostProcess.Reset();
	}

	if (ImageWidget.IsValid())
	{
		if (GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->RemoveViewportWidgetContent(ImageWidget.ToSharedRef());
		}
		ImageWidget.Reset();
	}

	if (Brush.IsValid())
	{
		Brush->SetResourceObject(nullptr);
		Brush.Reset();
	}

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

	if (Texture)
	{
		// Release the wrapped shared textures on the render thread first.
		if (IsInGameThread() && GDynamicRHI)
		{
			ENQUEUE_RENDER_COMMAND(UXWebviewSpikeReleaseSources)(
				[this](FRHICommandListImmediate&)
				{
					CachedSources.Empty();
					CachedGeneration = 0;
				});
			FlushRenderingCommands();
		}

		Texture->RemoveFromRoot();
		Texture->MarkAsGarbage();
		Texture = nullptr;
	}

	uint32 Dummy = 0;
	while (CopyMicros.Dequeue(Dummy))
	{
	}

	bConnected = false;
	bHelloSent = false;
	LastSeq = 0;
	ReportedGeneration = 0;
	TextureWidth = 0;
	TextureHeight = 0;
	CopyCount = 0;
	CopyTotalMicros = 0;
	CopyMaxMicros = 0;
	ReceivedFrames = 0;
	ReceivedPerSecond = 0;
	WindowFrames = 0;
	WindowSeconds = 0.0;
	LastBeginFrameSeconds = 0.0;
	bLoggedOpenFailure = false;
	PixelReadback.Reset();
	bPixelReadbackPending = false;
	bTearingDown = false;

	UE_LOG(LogUXWebviewSpike, Log, TEXT("spike stopped"));
}

FString FUXWebviewSpike::GetStatus() const
{
	const double AvgMicros = CopyCount > 0
		? static_cast<double>(CopyTotalMicros) / static_cast<double>(CopyCount)
		: 0.0;

	return FString::Printf(
		TEXT("running=%s connected=%s size=%dx%d seq=%u recv=%llu recv/s=%u copies=%llu copyAvg=%.1fus copyMax=%u us host='%s'"),
		IsRunning() ? TEXT("yes") : TEXT("no"),
		bConnected ? TEXT("yes") : TEXT("no"),
		Width, Height, LastSeq, ReceivedFrames, ReceivedPerSecond,
		CopyCount, AvgMicros, CopyMaxMicros, *HostExePath);
}

#endif // PLATFORM_WINDOWS
