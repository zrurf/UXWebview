// Sample-only demo: shows the Null provider animation on screen without CEF.
#include "CoreMinimal.h"

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Styling/SlateBrush.h"
#include "UUXWebview.h"
#include "UUXWebviewSubsystem.h"
#include "Widgets/Images/SImage.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewSampleDemo, Log, All);

namespace
{
TSharedPtr<FSlateBrush> GDemoBrush;
TSharedPtr<SImage> GDemoImage;
FTSTicker::FDelegateHandle GDemoTicker;
TWeakObjectPtr<UUXWebview> GDemoWebview;
TWeakObjectPtr<UGameInstance> GDemoGameInstance;
UTexture2D* GDemoTexture = nullptr;

void StopNullDemo()
{
	if (GDemoTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GDemoTicker);
		GDemoTicker.Reset();
	}

	if (GDemoImage.IsValid() && GDemoGameInstance.IsValid() && GDemoGameInstance->GetGameViewportClient())
	{
		GDemoGameInstance->GetGameViewportClient()->RemoveViewportWidgetContent(GDemoImage.ToSharedRef());
	}
	GDemoImage.Reset();

	if (GDemoBrush.IsValid())
	{
		GDemoBrush->SetResourceObject(nullptr);
		GDemoBrush.Reset();
	}
	GDemoTexture = nullptr;

	if (GDemoWebview.IsValid() && GDemoGameInstance.IsValid())
	{
		if (UUXWebviewSubsystem* Subsystem = GDemoGameInstance->GetSubsystem<UUXWebviewSubsystem>())
		{
			Subsystem->DestroyWebview(GDemoWebview.Get());
		}
	}
	GDemoWebview.Reset();
	GDemoGameInstance.Reset();
}

void StartNullDemo(const TArray<FString>& Args, UWorld* World)
{
	if (GDemoWebview.IsValid())
	{
		StopNullDemo();
	}

	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	UUXWebviewSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UUXWebviewSubsystem>() : nullptr;
	if (!ViewportClient || !Subsystem)
	{
		UE_LOG(LogUXWebviewSampleDemo, Warning, TEXT("null demo needs a running game instance"));
		return;
	}

	const int32 Width = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 512;
	const int32 Height = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 288;
	const int32 Fps = Args.Num() > 2 ? FCString::Atoi(*Args[2]) : 30;
	const float Seconds = Args.Num() > 3 ? FCString::Atof(*Args[3]) : 0.0f;

	FUXWebviewCreateParams Params;
	Params.ProviderName = TEXT("Null");
	Params.ViewportSize = FIntPoint(Width, Height);
	Params.TargetFrameRate = Fps;

	FUXWebviewResult Result;
	UUXWebview* Webview = Subsystem->CreateWebview(Params, Result);
	if (!Webview)
	{
		UE_LOG(LogUXWebviewSampleDemo, Warning, TEXT("null demo failed: %s"), *Result.Message);
		return;
	}

	GDemoWebview = Webview;
	GDemoGameInstance = GameInstance;

	UTexture2D* Texture = Webview->GetTexture();
	if (Texture)
	{
		GDemoTexture = Texture;
		GDemoBrush = MakeShared<FSlateBrush>();
		GDemoBrush->DrawAs = ESlateBrushDrawType::Image;
		GDemoBrush->ImageSize = FVector2D(Width, Height);
		GDemoBrush->SetResourceObject(Texture);

		SAssignNew(GDemoImage, SImage).Image(GDemoBrush.Get());
		ViewportClient->AddViewportWidgetContent(GDemoImage.ToSharedRef(), 0);
	}

	// Counts texture swaps so a headless run can prove the animation advances.
	TSharedPtr<uint64> Swaps = MakeShared<uint64>(0);
	TSharedPtr<float> Elapsed = MakeShared<float>(0.0f);
	TSharedPtr<float> NextLog = MakeShared<float>(1.0f);

	GDemoTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Swaps, Elapsed, NextLog, Seconds](float DeltaSeconds)
	{
		if (!GDemoWebview.IsValid())
		{
			return false;
		}

		*Elapsed += DeltaSeconds;
		UTexture2D* Current = GDemoWebview->GetTexture();
		if (Current && Current != GDemoTexture && GDemoBrush.IsValid())
		{
			GDemoTexture = Current;
			(*Swaps)++;
			GDemoBrush->SetResourceObject(Current);
			if (GDemoImage.IsValid())
			{
				GDemoImage->Invalidate(EInvalidateWidgetReason::Paint);
			}
		}

		if (*Elapsed >= *NextLog)
		{
			*NextLog += 1.0f;
			UE_LOG(LogUXWebviewSampleDemo, Log, TEXT("null demo: %.1fs swaps=%llu"),
				*Elapsed, *Swaps);
		}

		if (Seconds > 0.0f && *Elapsed >= Seconds)
		{
			StopNullDemo();
			if (!GIsEditor)
			{
				FPlatformMisc::RequestExit(false);
			}
			return false;
		}
		return true;
	}));

	UE_LOG(LogUXWebviewSampleDemo, Log, TEXT("null demo started: %dx%d @%dfps"), Width, Height, Fps);
}

void StopNullDemoCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)Args;
	(void)World;
	StopNullDemo();
	UE_LOG(LogUXWebviewSampleDemo, Log, TEXT("null demo stopped"));
}

FAutoConsoleCommandWithWorldAndArgs GNullDemoCommand(
	TEXT("UXWebviewSample.NullDemo"),
	TEXT("UXWebviewSample.NullDemo [width] [height] [fps] - shows the Null provider animation"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&StartNullDemo));

FAutoConsoleCommandWithWorldAndArgs GNullDemoStopCommand(
	TEXT("UXWebviewSample.NullStop"),
	TEXT("Stops the Null provider demo"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&StopNullDemoCommand));
} // namespace
