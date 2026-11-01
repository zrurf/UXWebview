// Sample-only demo: hosts the UMG widget in the viewport without CEF.
#include "CoreMinimal.h"

#include "Blueprint/UserWidget.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "UUXWebview.h"
#include "UUXWebviewSubsystem.h"
#include "UUXWebviewWidget.h"
#include "UXWebviewSample.h"
#include "Widgets/Layout/SBox.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewWidgetDemo, Log, All);

namespace
{
TSharedPtr<SWidget> GWidgetDemoRoot;
TWeakObjectPtr<UUXWebviewWidget> GWidgetDemoWidget;
TWeakObjectPtr<UUXWebview> GWidgetDemoWebview;
TWeakObjectPtr<UGameInstance> GWidgetDemoGameInstance;
FTSTicker::FDelegateHandle GWidgetDemoTicker;

/** Optional second instance used to smoke-test multiple webviews on one host. */
TSharedPtr<SWidget> GWidgetSecondRoot;
TWeakObjectPtr<UUXWebview> GWidgetSecondWebview;

/** Answers page bridge queries while the demo runs. */
UUXWebviewSampleBridgeHandler* GWidgetBridgeHandler = nullptr;

void BindBridgeHandler(UUXWebview* View)
{
	if (GWidgetBridgeHandler)
	{
		GWidgetBridgeHandler->RemoveFromRoot();
		GWidgetBridgeHandler = nullptr;
	}

	GWidgetBridgeHandler = NewObject<UUXWebviewSampleBridgeHandler>();
	GWidgetBridgeHandler->SetView(View);
	GWidgetBridgeHandler->AddToRoot();
	View->OnScriptMessage.AddDynamic(GWidgetBridgeHandler,
		&UUXWebviewSampleBridgeHandler::HandleScriptMessage);
}

void StopWidgetDemo()
{
	if (GWidgetDemoTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GWidgetDemoTicker);
		GWidgetDemoTicker.Reset();
	}

	if (GWidgetDemoGameInstance.IsValid())
	{
		if (UGameViewportClient* ViewportClient = GWidgetDemoGameInstance->GetGameViewportClient())
		{
			if (GWidgetSecondRoot.IsValid())
			{
				ViewportClient->RemoveViewportWidgetContent(GWidgetSecondRoot.ToSharedRef());
			}
			if (GWidgetDemoRoot.IsValid())
			{
				ViewportClient->RemoveViewportWidgetContent(GWidgetDemoRoot.ToSharedRef());
			}
		}
	}
	GWidgetSecondRoot.Reset();
	GWidgetDemoRoot.Reset();
	GWidgetDemoWidget.Reset();

	if (GWidgetBridgeHandler)
	{
		GWidgetBridgeHandler->RemoveFromRoot();
		GWidgetBridgeHandler = nullptr;
	}

	if (GWidgetDemoGameInstance.IsValid())
	{
		if (UUXWebviewSubsystem* Subsystem = GWidgetDemoGameInstance->GetSubsystem<UUXWebviewSubsystem>())
		{
			if (GWidgetSecondWebview.IsValid())
			{
				Subsystem->DestroyWebview(GWidgetSecondWebview.Get());
			}
			if (GWidgetDemoWebview.IsValid())
			{
				Subsystem->DestroyWebview(GWidgetDemoWebview.Get());
			}
		}
	}
	GWidgetSecondWebview.Reset();
	GWidgetDemoWebview.Reset();
	GWidgetDemoGameInstance.Reset();
}

void StartWidgetDemo(const TArray<FString>& Args, UWorld* World)
{
	if (GWidgetDemoRoot.IsValid())
	{
		StopWidgetDemo();
	}

	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	UUXWebviewSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UUXWebviewSubsystem>() : nullptr;
	if (!ViewportClient || !Subsystem)
	{
		UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("widget demo needs a running game instance"));
		return;
	}

	// Zero means the widget follows the layout scale, which is what a shipped UI wants.
	const float Dpi = Args.Num() > 0 ? FMath::Clamp(FCString::Atof(*Args[0]), 0.0f, 4.0f) : 0.0f;
	const float Seconds = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f;
	const FString Provider = Args.Num() > 2 && !Args[2].IsEmpty() ? Args[2] : FString(TEXT("Null"));
	const FString Url = Args.Num() > 3 ? Args[3] : FString();
	const int32 FrameRate = Args.Num() > 4 ? FCString::Atoi(*Args[4]) : 30;
	// Optional size args let a perf pass use the same command for full-screen
	// viewports; the defaults stay at the small demo panel. Zero selects fill
	// mode, where the panel inherits the viewport size and Slate drives the DPI.
	const int32 Width = Args.Num() > 5 ? FMath::Max(FCString::Atoi(*Args[5]), 0) : 512;
	const int32 Height = Args.Num() > 6 ? FMath::Max(FCString::Atoi(*Args[6]), 0) : 288;
	const bool bFill = Width <= 0 || Height <= 0;
	// Non-zero switches the view to a native overlay child window (no texture copy).
	const bool bOverlay = Args.Num() > 7 && FCString::Atoi(*Args[7]) != 0;
	// Non-zero anchors the panel to the top-left corner, which is what a HUD wants.
	const bool bTopLeft = Args.Num() > 8 && FCString::Atoi(*Args[8]) != 0;
	const float BoxScale = Dpi > 0.0f ? Dpi : 1.0f;

	FUXWebviewCreateParams Params;
	Params.ProviderName = Provider;
	// Fill mode has no layout size yet, so seed the browser from the physical
	// viewport; the widget corrects the viewport once it has been laid out.
	FIntPoint SeedSize(1920, 1080);
	if (bFill && ViewportClient->Viewport)
	{
		SeedSize = ViewportClient->Viewport->GetSizeXY();
	}
	Params.ViewportSize = bFill
		? SeedSize
		: FIntPoint(FMath::RoundToInt(Width * BoxScale), FMath::RoundToInt(Height * BoxScale));
	Params.DpiScale = Dpi;
	Params.TargetFrameRate = FrameRate;
	Params.InitialUrl = Url;
	Params.DisplayMode = bOverlay ? EUXWebviewDisplayMode::Overlay : EUXWebviewDisplayMode::Texture;

	FUXWebviewResult Result;
	UUXWebview* Webview = Subsystem->CreateWebview(Params, Result);
	if (!Webview)
	{
		UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("widget demo failed: %s"), *Result.Message);
		return;
	}

	UUXWebviewWidget* Widget = CreateWidget<UUXWebviewWidget>(GameInstance);
	if (!Widget)
	{
		Subsystem->DestroyWebview(Webview);
		UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("widget demo could not create the UMG widget"));
		return;
	}

	Widget->SetWebView(Webview);
	Widget->SetDpiScale(Dpi);
	Widget->SetInteractable(true);

	GWidgetDemoWebview = Webview;
	GWidgetDemoGameInstance = GameInstance;
	GWidgetDemoWidget = Widget;

	// The viewport overlay gives its content the full viewport size, so an outer
	// box positions a size-constrained inner box instead of the leaf itself.
	// Fill mode drops the inner constraint and lets the panel cover the viewport.
	TSharedPtr<SWidget> Leaf = Widget->TakeWidget();
	if (!bFill)
	{
		Leaf = SNew(SBox)
			.WidthOverride(static_cast<float>(Width))
			.HeightOverride(static_cast<float>(Height))
			[Leaf.ToSharedRef()];
	}
	GWidgetDemoRoot = SNew(SBox)
		.HAlign(bFill ? HAlign_Fill : (bTopLeft ? HAlign_Left : HAlign_Center))
		.VAlign(bFill ? VAlign_Fill : (bTopLeft ? VAlign_Top : VAlign_Center))
		.Padding(FMargin(bTopLeft && !bFill ? 24.0f : 0.0f))
		[Leaf.ToSharedRef()];
	ViewportClient->AddViewportWidgetContent(GWidgetDemoRoot.ToSharedRef(), 10);

	const FString UrlDisplay = Url.IsEmpty() ? FString(TEXT("(default)")) : Url;
	const FString SizeDisplay = bFill ? FString(TEXT("fill")) : FString::Printf(TEXT("%dx%d"), Width, Height);
	UE_LOG(LogUXWebviewWidgetDemo, Log,
		TEXT("widget demo started: provider=%s url=%s size=%s dpi=%.2f (0=auto) viewport=%dx%d fps=%d mode=%s; click inside to send input"),
		*Provider, *UrlDisplay, *SizeDisplay, Dpi,
		Params.ViewportSize.X, Params.ViewportSize.Y, FrameRate,
		bOverlay ? TEXT("overlay") : TEXT("texture"));

	if (Seconds > 0.0f)
	{
		TSharedPtr<float> Elapsed = MakeShared<float>(0.0f);
		GWidgetDemoTicker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Elapsed, Seconds](float DeltaSeconds)
		{
			*Elapsed += DeltaSeconds;
			if (*Elapsed < Seconds)
			{
				return true;
			}

			StopWidgetDemo();
			if (!GIsEditor)
			{
				FPlatformMisc::RequestExit(false);
			}
			return false;
		}));
	}
}

/** HUD preset: a small texture panel anchored to the top-left corner. */
void HudSceneCommand(const TArray<FString>& Args, UWorld* World)
{
	const FString Url = Args.Num() > 0 ? Args[0] : FString();
	const int32 Width = Args.Num() > 1 ? FMath::Max(FCString::Atoi(*Args[1]), 64) : 480;
	const int32 Height = Args.Num() > 2 ? FMath::Max(FCString::Atoi(*Args[2]), 64) : 270;

	TArray<FString> DemoArgs;
	DemoArgs.Add(TEXT("0"));
	DemoArgs.Add(TEXT("0"));
	DemoArgs.Add(TEXT("CEF"));
	DemoArgs.Add(Url);
	DemoArgs.Add(TEXT("30"));
	DemoArgs.Add(FString::FromInt(Width));
	DemoArgs.Add(FString::FromInt(Height));
	DemoArgs.Add(TEXT("0"));
	DemoArgs.Add(TEXT("1"));
	StartWidgetDemo(DemoArgs, World);
}

/** Fullscreen preset: an overlay view covering the whole game viewport. */
void FullscreenSceneCommand(const TArray<FString>& Args, UWorld* World)
{
	const FString Url = Args.Num() > 0 ? Args[0] : FString();
	const int32 FrameRate = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 60;

	// Fill mode: the panel takes the viewport size, so Slate applies its own
	// layout scale and the overlay ends up covering the whole game window.
	TArray<FString> DemoArgs;
	DemoArgs.Add(TEXT("0"));
	DemoArgs.Add(TEXT("0"));
	DemoArgs.Add(TEXT("CEF"));
	DemoArgs.Add(Url);
	DemoArgs.Add(FString::FromInt(FrameRate));
	DemoArgs.Add(TEXT("0"));
	DemoArgs.Add(TEXT("0"));
	DemoArgs.Add(TEXT("1"));
	DemoArgs.Add(TEXT("0"));
	StartWidgetDemo(DemoArgs, World);
}

void StopWidgetDemoCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)Args;
	(void)World;
	StopWidgetDemo();
	UE_LOG(LogUXWebviewWidgetDemo, Log, TEXT("widget demo stopped"));
}

/** Injects a click at CSS coordinates after a delay so a smoke page has time to load. */
void ProbeClickCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const float X = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 256.0f;
	const float Y = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 192.0f;
	const float Delay = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 2.0f;

	TSharedPtr<float> Elapsed = MakeShared<float>(0.0f);
	TWeakObjectPtr<UUXWebview> WeakView = GWidgetDemoWebview;
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Elapsed, WeakView, X, Y, Delay](float DeltaSeconds) -> bool
		{
			*Elapsed += DeltaSeconds;
			if (*Elapsed < Delay)
			{
				return true;
			}

			UUXWebview* View = WeakView.Get();
			if (!View)
			{
				UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("widget probe lost the webview"));
				return false;
			}

			FUXWebviewInputEvent Event;
			Event.Position = FVector2D(X, Y);

			Event.Type = EUXWebviewInputType::MouseMove;
			View->SendInput(Event);
			Event.Type = EUXWebviewInputType::MouseDown;
			Event.Button = 0;
			View->SendInput(Event);
			Event.Type = EUXWebviewInputType::MouseUp;
			View->SendInput(Event);

			UE_LOG(LogUXWebviewWidgetDemo, Log, TEXT("widget probe clicked (%.0f, %.0f)"), X, Y);
			return false;
		}));
}

/** Runs a demo action after a delay so chained console commands can be sequenced. */
void ScheduleViewAction(float Delay, const FString& Label, TFunction<void(UUXWebview*)> Action);

/** Feeds IME composition, commit or cancel into the running webview. */
void ImeProbeCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const FString Text = Args.Num() > 0 ? Args[0] : FString(TEXT("ni"));
	const int32 Mode = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 0;
	const float Delay = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 2.0f;

	ScheduleViewAction(Delay, FString::Printf(TEXT("ime mode %d '%s'"), Mode, *Text),
		[Text, Mode](UUXWebview* View)
		{
			FUXWebviewInputEvent Event;
			Event.Text = Text;
			Event.Type = Mode == 2 ? EUXWebviewInputType::ImeCancel
				: Mode == 1 ? EUXWebviewInputType::ImeCommit
				: EUXWebviewInputType::ImeComposition;
			View->SendInput(Event);
		});
}

/** Drags across the page so text selection can be verified without a real mouse. */
void DragProbeCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const float X1 = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 30.0f;
	const float Y1 = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 40.0f;
	const float X2 = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 400.0f;
	const float Y2 = Args.Num() > 3 ? FCString::Atof(*Args[3]) : 40.0f;
	const float Delay = Args.Num() > 4 ? FCString::Atof(*Args[4]) : 2.0f;

	ScheduleViewAction(Delay, FString::Printf(TEXT("drag (%.0f,%.0f)->(%.0f,%.0f)"), X1, Y1, X2, Y2),
		[X1, Y1, X2, Y2](UUXWebview* View)
		{
			FUXWebviewInputEvent Event;
			Event.Position = FVector2D(X1, Y1);
			Event.Type = EUXWebviewInputType::MouseMove;
			View->SendInput(Event);
			Event.Type = EUXWebviewInputType::MouseDown;
			Event.Button = 0;
			View->SendInput(Event);

			// Intermediate moves carry the held button, which is what Chromium
			// needs to start a selection instead of a hover.
			for (int32 Step = 1; Step <= 8; ++Step)
			{
				const float Alpha = static_cast<float>(Step) / 8.0f;
				Event.Type = EUXWebviewInputType::MouseMove;
				Event.Position = FVector2D(X1 + (X2 - X1) * Alpha, Y1 + (Y2 - Y1) * Alpha);
				View->SendInput(Event);
			}

			Event.Type = EUXWebviewInputType::MouseUp;
			Event.Button = 0;
			View->SendInput(Event);
		});
}

/** Sends one key press to the running webview without going through OS focus. */
void KeyProbeCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const int32 Vk = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 'A';
	const bool bCtrl = Args.Num() > 1 && FCString::Atoi(*Args[1]) != 0;
	const float Delay = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 2.0f;

	ScheduleViewAction(Delay, FString::Printf(TEXT("key vk=%d ctrl=%d"), Vk, bCtrl ? 1 : 0),
		[Vk, bCtrl](UUXWebview* View)
		{
			FUXWebviewInputEvent Event;
			Event.KeyCode = Vk;
			Event.bControl = bCtrl;
			Event.Type = EUXWebviewInputType::KeyDown;
			View->SendInput(Event);
			Event.Type = EUXWebviewInputType::KeyUp;
			View->SendInput(Event);
		});
}

/** Runs a demo action after a delay so chained console commands can be sequenced. */
void ScheduleViewAction(float Delay, const FString& Label, TFunction<void(UUXWebview*)> Action)
{
	TSharedPtr<float> Elapsed = MakeShared<float>(0.0f);
	TWeakObjectPtr<UUXWebview> WeakView = GWidgetDemoWebview;
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Elapsed, WeakView, Delay, Label, Action](float DeltaSeconds) -> bool
		{
			*Elapsed += DeltaSeconds;
			if (*Elapsed < Delay)
			{
				return true;
			}

			UUXWebview* View = WeakView.Get();
			if (!View)
			{
				UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("widget demo lost the webview"));
				return false;
			}

			Action(View);
			UE_LOG(LogUXWebviewWidgetDemo, Log, TEXT("widget demo %s"), *Label);
			return false;
		}));
}

void NavigateCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const float Delay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 2.0f;
	const FString Url = Args.Num() > 1 ? Args[1] : FString();
	if (Url.IsEmpty())
	{
		UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("widget navigate needs a url"));
		return;
	}

	ScheduleViewAction(Delay, FString::Printf(TEXT("navigate to %s"), *Url),
		[Url](UUXWebview* View) { View->LoadURL(Url); });
}

void ReloadCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const float Delay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 2.0f;
	ScheduleViewAction(Delay, TEXT("reload"), [](UUXWebview* View) { View->Reload(); });
}

void LoadHtmlCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const float Delay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 2.0f;
	const FString FilePath = Args.Num() > 1
		? FPaths::ConvertRelativePathToFull(Args[1])
		: FString();
	if (FilePath.IsEmpty())
	{
		UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("widget load html needs a file path"));
		return;
	}

	FString Html;
	if (!FFileHelper::LoadFileToString(Html, *FilePath))
	{
		UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("widget load html could not read %s"), *FilePath);
		return;
	}

	ScheduleViewAction(Delay, FString::Printf(TEXT("load html from %s"), *FilePath),
		[Html, FilePath](UUXWebview* View) { View->LoadHTML(Html, FilePath); });
}

/** Runs a page bridge query and logs the reply the page received. */
void BridgeCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const float Delay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 2.0f;
	ScheduleViewAction(Delay, TEXT("bridge call"), [](UUXWebview* View)
	{
		BindBridgeHandler(View);

		const FString Script = TEXT(
			"window.uxwv.call('ping', {value: 42})"
			".then(function(v){return 'bridge:' + JSON.stringify(v);})"
			".catch(function(e){return 'bridge-error:' + e;});");
		View->ExecuteScript(Script, [](bool bSuccess, const FString& Result)
		{
			UE_LOG(LogUXWebviewWidgetDemo, Log, TEXT("bridge result ok=%d '%s'"),
				bSuccess ? 1 : 0, *Result);
		});

		// A throwing script must be reported, never crash the webview.
		View->ExecuteScript(TEXT("throw new Error('bridge-probe-error');"),
			[](bool bSuccess, const FString& Result)
			{
				UE_LOG(LogUXWebviewWidgetDemo, Log, TEXT("bridge error probe ok=%d '%s'"),
					bSuccess ? 1 : 0, *Result);
			});
	});
}

/**
 * Samples the page's requestAnimationFrame rate. Overlay browsers composite
 * inside their own window, so this is the only headless way to see how many
 * frames the page actually gets.
 */
void FpsProbeCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const float Delay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 3.0f;
	const float Seconds = FMath::Max(Args.Num() > 1 ? FCString::Atof(*Args[1]) : 3.0f, 0.5f);

	ScheduleViewAction(Delay, FString::Printf(TEXT("fps probe (%0.1fs window)"), Seconds),
		[Seconds](UUXWebview* View)
		{
			const FString Script = FString::Printf(TEXT(
				"new Promise(function(resolve){"
				"var frames=0,start=0;"
				"function step(now){"
				"if(start===0){start=now;}"
				"frames++;"
				"if(now-start>=%.0f){resolve('raf-fps:'+(frames*1000/(now-start)).toFixed(1)"
				"+' frames='+frames);return;}"
				"requestAnimationFrame(step);}"
				"requestAnimationFrame(step);})"), Seconds * 1000.0f);

			View->ExecuteScript(Script, [](bool bSuccess, const FString& Result)
			{
				UE_LOG(LogUXWebviewWidgetDemo, Log, TEXT("fps probe ok=%d '%s'"),
					bSuccess ? 1 : 0, *Result);
			});
		});
}

/**
 * Rehosts the running overlay on the other WebView2 controller type. The page
 * reloads, so the widget keeps its bounds and URL but loses page state.
 */
void CompositionCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const bool bComposition = Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : true;
	const float Delay = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 2.0f;

	ScheduleViewAction(Delay, FString::Printf(TEXT("composition %d"), bComposition ? 1 : 0),
		[bComposition](UUXWebview* View)
		{
			const FUXWebviewResult Result = View->SetCompositionOverlay(bComposition);
			const FString Message = FString::Printf(TEXT("composition overlay %s: ok=%d '%s'"),
				bComposition ? TEXT("on") : TEXT("off"), Result.bSuccess ? 1 : 0,
				*Result.Message);
			if (Result.bSuccess)
			{
				UE_LOG(LogUXWebviewWidgetDemo, Log, TEXT("%s"), *Message);
			}
			else
			{
				UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("%s"), *Message);
			}
		});
}

/**
 * Captures the game viewport after a delay. Texture mode draws the web frame
 * into the viewport, so a HighResShot is the headless way to inspect the
 * pixels that are actually presented.
 */
void ScreenshotCommand(const TArray<FString>& Args, UWorld* World)
{
	const float Delay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 3.0f;
	ScheduleViewAction(Delay, FString::Printf(TEXT("screenshot after %0.1fs"), Delay),
		[World](UUXWebview* View)
		{
			UWorld* TargetWorld = World ? World : (View ? View->GetWorld() : nullptr);
			if (TargetWorld)
			{
				// An explicit path keeps the file name stable for comparison runs.
				FScreenshotRequest::RequestScreenshot(
					FPaths::ProjectSavedDir() / TEXT("Screenshots") / TEXT("uxwebview-shot"), true, false);
				UE_LOG(LogUXWebviewWidgetDemo, Log, TEXT("screenshot requested"));
			}
		});
}

/** Sends a game event to the page as a 'uxwv' CustomEvent. */
void EmitCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const float Delay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 2.0f;
	const FString EventName = Args.Num() > 1 ? Args[1] : FString(TEXT("demo"));
	const FString Payload = Args.Num() > 2 ? Args[2] : FString(TEXT("{\"value\":7}"));

	ScheduleViewAction(Delay, FString::Printf(TEXT("emit %s"), *EventName),
		[EventName, Payload](UUXWebview* View)
		{
			const FUXWebviewResult Result = View->EmitToWeb(EventName, Payload);
			if (!Result.bSuccess)
			{
				UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("emit failed: %s"), *Result.Message);
			}
		});
}

/** Adds a second webview on the same host so multi-instance routing can be smoke-tested. */
void SecondViewCommand(const TArray<FString>& Args, UWorld* World)
{
	(void)World;
	const float Delay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 2.0f;
	const FString Url = Args.Num() > 1 ? Args[1] : FString(TEXT("http://example.com/"));
	const int32 FrameRate = Args.Num() > 2 ? FCString::Atoi(*Args[2]) : 30;
	if (GWidgetSecondWebview.IsValid())
	{
		UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("second view already exists"));
		return;
	}

	TSharedPtr<float> Elapsed = MakeShared<float>(0.0f);
	TWeakObjectPtr<UGameInstance> WeakGameInstance = GWidgetDemoGameInstance;
	TWeakObjectPtr<UUXWebview> WeakPrimary = GWidgetDemoWebview;
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Elapsed, WeakGameInstance, WeakPrimary, Delay, Url, FrameRate](float DeltaSeconds) -> bool
		{
			*Elapsed += DeltaSeconds;
			if (*Elapsed < Delay)
			{
				return true;
			}

			UGameInstance* GameInstance = WeakGameInstance.Get();
			UUXWebviewSubsystem* Subsystem = GameInstance
				? GameInstance->GetSubsystem<UUXWebviewSubsystem>()
				: nullptr;
			UGameViewportClient* ViewportClient = GameInstance
				? GameInstance->GetGameViewportClient()
				: nullptr;
			if (!Subsystem || !ViewportClient)
			{
				UE_LOG(LogUXWebviewWidgetDemo, Warning, TEXT("second view needs the running demo"));
				return false;
			}

			UUXWebview* Primary = WeakPrimary.Get();
			FUXWebviewCreateParams Params;
			Params.ProviderName = Primary ? Primary->GetProviderName() : FString(TEXT("CEF"));
			Params.ViewportSize = FIntPoint(256, 144);
			Params.DpiScale = 1.0f;
			Params.TargetFrameRate = FrameRate;
			Params.InitialUrl = Url;

			FUXWebviewResult Result;
			UUXWebview* Second = Subsystem->CreateWebview(Params, Result);
			if (!Second)
			{
				UE_LOG(LogUXWebviewWidgetDemo, Warning,
					TEXT("second view failed: %s"), *Result.Message);
				return false;
			}

			UUXWebviewWidget* SecondWidget = CreateWidget<UUXWebviewWidget>(GameInstance);
			if (SecondWidget)
			{
				SecondWidget->SetWebView(Second);
				SecondWidget->SetInteractable(true);
				GWidgetSecondRoot = SNew(SBox)
					.WidthOverride(256.0f)
					.HeightOverride(144.0f)
					.Padding(FMargin(16.0f))
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Bottom)
					[
						SecondWidget->TakeWidget()
					];
				ViewportClient->AddViewportWidgetContent(GWidgetSecondRoot.ToSharedRef(), 11);
			}

			GWidgetSecondWebview = Second;
			UE_LOG(LogUXWebviewWidgetDemo, Log,
				TEXT("second view created: provider=%s url=%s 256x144"),
				*Params.ProviderName, *Url);
			return false;
		}));
}

FAutoConsoleCommandWithWorldAndArgs GWidgetDemoCommand(
	TEXT("UXWebviewSample.WidgetDemo"),
	TEXT("UXWebviewSample.WidgetDemo [dpi] [seconds] [provider] [url] [fps] [width] [height] [overlay] [topLeft] - hosts the UMG webview widget in the viewport; width/height 0 fills the viewport"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&StartWidgetDemo));

FAutoConsoleCommandWithWorldAndArgs GWidgetHudCommand(
	TEXT("UXWebviewSample.Hud"),
	TEXT("UXWebviewSample.Hud [url] [width] [height] - HUD scene preset: top-left texture panel"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HudSceneCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetFullscreenCommand(
	TEXT("UXWebviewSample.Fullscreen"),
	TEXT("UXWebviewSample.Fullscreen [url] [fps] - fullscreen scene preset: viewport-sized overlay"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FullscreenSceneCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetProbeCommand(
	TEXT("UXWebviewSample.WidgetProbe"),
	TEXT("UXWebviewSample.WidgetProbe [cssX] [cssY] [delay] - clicks the running demo after a delay"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ProbeClickCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetImeCommand(
	TEXT("UXWebviewSample.WidgetIme"),
	TEXT("UXWebviewSample.WidgetIme [text] [mode] [delay] - feeds IME composition(0)/commit(1)/cancel(2)"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ImeProbeCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetKeyCommand(
	TEXT("UXWebviewSample.WidgetKey"),
	TEXT("UXWebviewSample.WidgetKey [vk] [ctrl] [delay] - presses one key in the running demo"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KeyProbeCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetDragCommand(
	TEXT("UXWebviewSample.WidgetDrag"),
	TEXT("UXWebviewSample.WidgetDrag [x1] [y1] [x2] [y2] [delay] - drags across the page"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DragProbeCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetNavigateCommand(
	TEXT("UXWebviewSample.WidgetNavigate"),
	TEXT("UXWebviewSample.WidgetNavigate [delay] [url] - navigates the running demo"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&NavigateCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetReloadCommand(
	TEXT("UXWebviewSample.WidgetReload"),
	TEXT("UXWebviewSample.WidgetReload [delay] - reloads the running demo"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ReloadCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetLoadHtmlCommand(
	TEXT("UXWebviewSample.WidgetLoadHtml"),
	TEXT("UXWebviewSample.WidgetLoadHtml [delay] [file] - loads the file content through LoadHTML"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LoadHtmlCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetBridgeCommand(
	TEXT("UXWebviewSample.WidgetBridge"),
	TEXT("UXWebviewSample.WidgetBridge [delay] - makes the page call window.uxwv.call and logs the reply"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&BridgeCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetFpsProbeCommand(
	TEXT("UXWebviewSample.WidgetFpsProbe"),
	TEXT("UXWebviewSample.WidgetFpsProbe [delay] [seconds] - samples the page requestAnimationFrame rate and logs it"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FpsProbeCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetCompositionCommand(
	TEXT("UXWebviewSample.WidgetComposition"),
	TEXT("UXWebviewSample.WidgetComposition [on] [delay] - rehosts the running overlay as a composition(1) or windowed(0) controller"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&CompositionCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetScreenshotCommand(
	TEXT("UXWebviewSample.WidgetScreenshot"),
	TEXT("UXWebviewSample.WidgetScreenshot [delay] - requests a HighResShot of the game viewport"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ScreenshotCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetEmitCommand(
	TEXT("UXWebviewSample.WidgetEmit"),
	TEXT("UXWebviewSample.WidgetEmit [delay] [event] [payloadJson] - sends a game event to the page"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&EmitCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetSecondViewCommand(
	TEXT("UXWebviewSample.WidgetSecond"),
	TEXT("UXWebviewSample.WidgetSecond [delay] [url] [fps] - adds a second webview next to the demo"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SecondViewCommand));

FAutoConsoleCommandWithWorldAndArgs GWidgetDemoStopCommand(
	TEXT("UXWebviewSample.WidgetStop"),
	TEXT("Stops the UMG widget demo"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&StopWidgetDemoCommand));
} // namespace
