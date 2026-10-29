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
	const FString Url = Args.Num() > 4 ? Args[4] : FString();

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
	TEXT("UXWebview.SpikeRun [seconds] [width] [height] [fps]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpikeRun));
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
