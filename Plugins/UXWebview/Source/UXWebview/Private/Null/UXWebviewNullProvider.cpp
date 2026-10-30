#include "Null/UXWebviewNullProvider.h"

#include "Engine/Texture2D.h"
#include "Provider/UXWebviewProviderRegistry.h"
#include "TextureResource.h"
#include "UXWebviewLog.h"

namespace
{
constexpr int32 kFrameCount = 8;
constexpr int32 kMaxFrameSize = 512;

FString NullPlatformName()
{
#if PLATFORM_WINDOWS
	return TEXT("Win64");
#elif PLATFORM_MAC
	return TEXT("Mac");
#else
	return TEXT("Linux");
#endif
}

class FUXWebviewNullProviderFactory final : public IUXWebviewProviderFactory
{
public:
	virtual FUXWebviewProviderInfo GetInfo() const override
	{
		FUXWebviewProviderInfo Info;
		Info.Name = TEXT("Null");
		Info.DisplayName = TEXT("Null Provider");
		Info.Version = TEXT("1.0");
		Info.Platform = NullPlatformName();
		Info.bAvailable = true;
		return Info;
	}

	virtual TUniquePtr<IUXWebviewProvider> Create() override
	{
		return MakeUnique<FUXWebviewNullProvider>();
	}
};
} // namespace

FUXWebviewProviderInfo FUXWebviewNullProvider::GetInfo() const
{
	FUXWebviewProviderInfo Info;
	Info.Name = TEXT("Null");
	Info.DisplayName = TEXT("Null Provider");
	Info.Version = TEXT("1.0");
	Info.Platform = NullPlatformName();
	Info.bAvailable = true;
	return Info;
}

FUXWebviewCapabilities FUXWebviewNullProvider::GetCapabilities() const
{
	FUXWebviewCapabilities Capabilities;
	Capabilities.bCpuPixelBuffer = true;
	Capabilities.MaxFrameRate = 60;
	Capabilities.MaxViewportSize = FIntPoint(kMaxFrameSize, kMaxFrameSize);
	return Capabilities;
}

FUXWebviewResult FUXWebviewNullProvider::Initialize(
	const FUXWebviewInitParams& InitParams, IUXWebviewViewClient* InClient)
{
	if (bInitialized)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidState,
			TEXT("null provider is already initialized"));
	}
	if (!InClient)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("null provider needs a view client"));
	}

	Client = InClient;
	ViewSize = FIntPoint(
		FMath::Clamp(InitParams.ViewportSize.X, 16, kMaxFrameSize),
		FMath::Clamp(InitParams.ViewportSize.Y, 16, kMaxFrameSize));
	DpiScale = InitParams.DpiScale > 0.0f ? InitParams.DpiScale : 1.0f;
	FrameRate = InitParams.TargetFrameRate > 0 ? InitParams.TargetFrameRate : 30;
	CurrentUrl = InitParams.InitialUrl;

	if (!CreateFrames())
	{
		Client = nullptr;
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			TEXT("null provider could not create frame textures"));
	}

	bInitialized = true;
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FUXWebviewNullProvider::Tick));

	Client->OnViewReady();
	Client->OnViewFrame(ViewSize.X, ViewSize.Y);
	return FUXWebviewResult::Ok();
}

void FUXWebviewNullProvider::Shutdown()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	ReleaseFrames();
	Client = nullptr;
	bInitialized = false;
}

FUXWebviewResult FUXWebviewNullProvider::GuardReady() const
{
	if (!bInitialized)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidState,
			TEXT("null provider is shut down"));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewNullProvider::LoadURL(const FString& Url)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	CurrentUrl = Url;
	Client->OnViewLoadCompleted(CurrentUrl);
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewNullProvider::LoadHTML(const FString& Html, const FString& BaseUrl)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	CurrentUrl = BaseUrl;
	Client->OnViewLoadCompleted(CurrentUrl);
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewNullProvider::Reload()
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	Client->OnViewLoadCompleted(CurrentUrl);
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewNullProvider::StopLoading()
{
	return GuardReady();
}

FUXWebviewResult FUXWebviewNullProvider::SetViewport(const FIntPoint& Size, float InDpiScale)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}

	const FIntPoint Clamped(
		FMath::Clamp(Size.X, 16, kMaxFrameSize),
		FMath::Clamp(Size.Y, 16, kMaxFrameSize));
	DpiScale = InDpiScale > 0.0f ? InDpiScale : 1.0f;

	if (Clamped != ViewSize)
	{
		ViewSize = Clamped;
		ReleaseFrames();
		if (!CreateFrames())
		{
			return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
				TEXT("null provider could not rebuild frame textures"));
		}
	}

	Client->OnViewFrame(ViewSize.X, ViewSize.Y);
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewNullProvider::SetVisible(bool bInVisible)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	bVisible = bInVisible;
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewNullProvider::SetFrameRate(int32 FramesPerSecond)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	FrameRate = FMath::Clamp(FramesPerSecond, 0, 60);
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewNullProvider::SetZoom(float ZoomFactor)
{
	return GuardReady();
}

FUXWebviewResult FUXWebviewNullProvider::SetUserAgent(const FString& UserAgent)
{
	return GuardReady();
}

FUXWebviewResult FUXWebviewNullProvider::ExecuteScript(
	const FString& Script, FUXWebviewScriptCallback OnResult)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	if (OnResult)
	{
		OnResult(true, TEXT("null"));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewNullProvider::EmitToWeb(const FString& EventName, const FString& PayloadJson)
{
	return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
		TEXT("null provider has no script bridge"));
}

FUXWebviewResult FUXWebviewNullProvider::SendInput(const FUXWebviewInputEvent& Event)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}

	// Logs non-move input so a manual sample run can prove the widget reaches the provider.
	if (Event.Type != EUXWebviewInputType::MouseMove)
	{
		UE_LOG(LogUXWebview, Log,
			TEXT("null input: type=%d pos=(%.1f, %.1f) button=%d wheel=%.2f key=%d"),
			static_cast<int32>(Event.Type), Event.Position.X, Event.Position.Y,
			Event.Button, Event.WheelDelta, Event.KeyCode);
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult FUXWebviewNullProvider::SetFocus(bool bFocused)
{
	return GuardReady();
}

FUXWebviewResult FUXWebviewNullProvider::RequestFrame()
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	++CurrentIndex;
	CurrentIndex %= Frames.Num();
	Client->OnViewFrame(ViewSize.X, ViewSize.Y);
	return FUXWebviewResult::Ok();
}

UTexture2D* FUXWebviewNullProvider::GetTexture() const
{
	return Frames.IsValidIndex(CurrentIndex) ? Frames[CurrentIndex] : nullptr;
}

FUXWebviewResult FUXWebviewNullProvider::OpenDevTools()
{
	return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
		TEXT("null provider has no devtools"));
}

bool FUXWebviewNullProvider::Tick(float DeltaSeconds)
{
	if (!bVisible || FrameRate <= 0 || Frames.Num() == 0)
	{
		return true;
	}

	Accumulator += DeltaSeconds;
	const double Interval = 1.0 / static_cast<double>(FrameRate);
	if (Accumulator < Interval)
	{
		return true;
	}
	Accumulator -= Interval;

	++CurrentIndex;
	CurrentIndex %= Frames.Num();
	if (Client)
	{
		Client->OnViewFrame(ViewSize.X, ViewSize.Y);
	}
	return true;
}

bool FUXWebviewNullProvider::CreateFrames()
{
	const int32 Width = ViewSize.X;
	const int32 Height = ViewSize.Y;

	Frames.Reserve(kFrameCount);
	for (int32 FrameIndex = 0; FrameIndex < kFrameCount; ++FrameIndex)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
		{
			ReleaseFrames();
			return false;
		}

		Texture->SRGB = true;
		Texture->NeverStream = true;

		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		uint8* Pixels = static_cast<uint8*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
		if (!Pixels)
		{
			ReleaseFrames();
			return false;
		}

		const int32 BarWidth = FMath::Max(Width / 8, 8);
		const int32 BarX = (FrameIndex * Width) / kFrameCount;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			uint8* Row = Pixels + Y * Width * 4;
			for (int32 X = 0; X < Width; ++X)
			{
				const bool bInBar = X >= BarX && X < BarX + BarWidth;
				const uint8 Bar = static_cast<uint8>(64 + (Y * 160) / Height);
				uint8* Pixel = Row + X * 4;
				Pixel[0] = bInBar ? Bar : 20; // B
				Pixel[1] = bInBar ? Bar : 14; // G
				Pixel[2] = bInBar ? 230 : 11; // R
				Pixel[3] = 255;               // A
			}
		}

		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		// Transient textures are garbage collected unless they are rooted; the
		// provider keeps only raw pointers because it is a plain C++ object.
		Texture->AddToRoot();
		Frames.Add(Texture);
	}

	return Frames.Num() == kFrameCount;
}

void FUXWebviewNullProvider::ReleaseFrames()
{
	for (UTexture2D* Texture : Frames)
	{
		if (Texture)
		{
			Texture->RemoveFromRoot();
		}
	}
	Frames.Reset();
	CurrentIndex = 0;
}

void RegisterNullProvider()
{
	FUXWebviewProviderRegistry::Get().RegisterFactory(MakeShared<FUXWebviewNullProviderFactory>());
	UE_LOG(LogUXWebview, Log, TEXT("null provider registered"));
}

void UnregisterNullProvider()
{
	FUXWebviewProviderRegistry::Get().UnregisterFactory(TEXT("Null"));
}
