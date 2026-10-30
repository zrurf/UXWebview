#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Spike/UXWebviewSpike.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewCEF, Log, All);

#if PLATFORM_WINDOWS
namespace
{
void SpikeStart(const TArray<FString>& Args, UWorld* World)
{
	(void)World;

	const int32 Width = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 1920;
	const int32 Height = Args.Num() > 2 ? FCString::Atoi(*Args[2]) : 1080;
	const int32 Fps = Args.Num() > 3 ? FCString::Atoi(*Args[3]) : 0;
	const FString Url = Args.Num() > 0 ? Args[0] : FString();

	if (!FUXWebviewSpike::Get().Start(Width, Height, Url, Fps))
	{
		UE_LOG(LogUXWebviewCEF, Warning, TEXT("spike start failed"));
	}
}

void SpikeStop(const TArray<FString>& Args, UWorld* World)
{
	(void)Args;
	(void)World;
	FUXWebviewSpike::Get().Stop();
}

void SpikeStats(const TArray<FString>& Args, UWorld* World)
{
	(void)Args;
	(void)World;
	UE_LOG(LogUXWebviewCEF, Log, TEXT("%s"), *FUXWebviewSpike::Get().GetStatus());
}

void SpikeRun(const TArray<FString>& Args, UWorld* World)
{
	(void)World;

	const float Seconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 10.0f;
	const int32 Width = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 1920;
	const int32 Height = Args.Num() > 2 ? FCString::Atoi(*Args[2]) : 1080;
	const int32 Fps = Args.Num() > 3 ? FCString::Atoi(*Args[3]) : 0;
	// '-' keeps the bench page; the optional scale renders below logical size.
	const FString Url = (Args.Num() > 4 && Args[4] != TEXT("-")) ? Args[4] : FString();
	const float Scale = Args.Num() > 5 ? FCString::Atof(*Args[5]) : 0.0f;
	if (Scale > 0.0f)
	{
		FUXWebviewSpike::Get().SetResolutionScale(Scale);
	}

	if (!FUXWebviewSpike::Get().Start(Width, Height, Url, Fps))
	{
		UE_LOG(LogUXWebviewCEF, Warning, TEXT("spike start failed"));
		return;
	}

	// Timed run for automated validation; exits the game when finished.
	TSharedPtr<float> Remaining = MakeShared<float>(FMath::Max(Seconds, 1.0f));
	TSharedPtr<bool> bReadbackRequested = MakeShared<bool>(false);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Remaining, bReadbackRequested](float DeltaTime)
		{
			*Remaining -= DeltaTime;

			// Give the pipeline time to settle, then read the final frame back.
			if (!*bReadbackRequested && *Remaining <= 2.0f)
			{
				*bReadbackRequested = true;
				FUXWebviewSpike::Get().ReadbackPixels();
			}

			if (*Remaining > 0.0f)
			{
				return true;
			}

			UE_LOG(LogUXWebviewCEF, Log, TEXT("spike result: %s"),
				*FUXWebviewSpike::Get().GetStatus());
			FUXWebviewSpike::Get().Stop();

			if (!GIsEditor)
			{
				FPlatformMisc::RequestExit(false);
			}
			return false;
		}));
}

void SpikeVisible(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const bool bVisible = Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : true;
	FUXWebviewSpike::Get().SetVisible(bVisible);
}

// Starts the spike, hides the widget after a warm-up and reports how many
// frames the host produced while nothing was visible.
void SpikeHiddenRun(const TArray<FString>& Args, UWorld* World)
{
	(void)World;

	const float HideSeconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 5.0f;
	// First paint takes a few seconds on a cold page, so keep the widget visible long enough.
	const float WarmUpSeconds = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 6.0f;
	if (!FUXWebviewSpike::Get().Start(1920, 1080, FString(), 60))
	{
		UE_LOG(LogUXWebviewCEF, Warning, TEXT("spike start failed"));
		return;
	}

	TSharedPtr<float> Elapsed = MakeShared<float>(0.0f);
	TSharedPtr<uint64> HiddenStartFrames = MakeShared<uint64>(0);
	TSharedPtr<uint64> VisibleStartFrames = MakeShared<uint64>(0);
	TSharedPtr<bool> bHidden = MakeShared<bool>(false);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Elapsed, HiddenStartFrames, VisibleStartFrames, bHidden, HideSeconds, WarmUpSeconds](float DeltaTime)
		{
			*Elapsed += DeltaTime;

			if (!*bHidden && *Elapsed >= WarmUpSeconds)
			{
				const uint64 VisibleDelta =
					FUXWebviewSpike::Get().GetReceivedFrames() - *VisibleStartFrames;
				UE_LOG(LogUXWebviewCEF, Log, TEXT("visible window %.1fs: %llu frames received"),
					WarmUpSeconds, VisibleDelta);

				*bHidden = true;
				*HiddenStartFrames = FUXWebviewSpike::Get().GetReceivedFrames();
				FUXWebviewSpike::Get().SetVisible(false);
			}

			if (*bHidden && *Elapsed >= WarmUpSeconds + HideSeconds)
			{
				const uint64 DeltaFrames =
					FUXWebviewSpike::Get().GetReceivedFrames() - *HiddenStartFrames;
				UE_LOG(LogUXWebviewCEF, Log, TEXT("hidden window %.1fs: %llu frames received"),
					HideSeconds, DeltaFrames);

				FUXWebviewSpike::Get().SetVisible(true);
				FUXWebviewSpike::Get().Stop();
				if (!GIsEditor)
				{
					FPlatformMisc::RequestExit(false);
				}
				return false;
			}
			return true;
		}));
}

FAutoConsoleCommandWithWorldAndArgs GSpikeStartCommand(
	TEXT("UXWebview.SpikeStart"),
	TEXT("UXWebview.SpikeStart [url] [width] [height] [fps]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpikeStart));

FAutoConsoleCommandWithWorldAndArgs GSpikeStopCommand(
	TEXT("UXWebview.SpikeStop"),
	TEXT("Stops the shared texture spike and the host process"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpikeStop));

FAutoConsoleCommandWithWorldAndArgs GSpikeStatsCommand(
	TEXT("UXWebview.SpikeStats"),
	TEXT("Prints spike frame and copy timing"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpikeStats));

void SpikePixels(const TArray<FString>& Args, UWorld* World)
{
	(void)Args;
	(void)World;
	FUXWebviewSpike::Get().ReadbackPixels();
}

FAutoConsoleCommandWithWorldAndArgs GSpikePixelsCommand(
	TEXT("UXWebview.SpikePixels"),
	TEXT("Reads the current frame back and logs a pixel summary"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpikePixels));

FAutoConsoleCommandWithWorldAndArgs GSpikeRunCommand(
	TEXT("UXWebview.SpikeRun"),
	TEXT("UXWebview.SpikeRun [seconds] [width] [height] [fps] [url|-] [renderScale]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpikeRun));

FAutoConsoleCommandWithWorldAndArgs GSpikeVisibleCommand(
	TEXT("UXWebview.SpikeVisible"),
	TEXT("UXWebview.SpikeVisible <0|1> hides or shows the spike widget"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpikeVisible));

FAutoConsoleCommandWithWorldAndArgs GSpikeRenderScaleCommand(
	TEXT("UXWebview.SpikeRenderScale"),
	TEXT("UXWebview.SpikeRenderScale <0.5-1.0> renders below the logical size and upscales"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			(void)World;
			const float Scale = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 1.0f;
			FUXWebviewSpike::Get().SetResolutionScale(Scale);
		}));

FAutoConsoleCommandWithWorldAndArgs GSpikeHiddenRunCommand(
	TEXT("UXWebview.SpikeHiddenRun"),
	TEXT("UXWebview.SpikeHiddenRun [hiddenSeconds] [warmUpSeconds] measures frames produced while hidden"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpikeHiddenRun));
} // namespace
#endif

class FUXWebviewCEFModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogUXWebviewCEF, Log, TEXT("UXWebviewCEF module loaded"));
	}

	virtual void ShutdownModule() override
	{
#if PLATFORM_WINDOWS
		FUXWebviewSpike::Get().Stop();
#endif
	}
};

IMPLEMENT_MODULE(FUXWebviewCEFModule, UXWebviewCEF)
