#include "UXWebviewDiagnostics.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Provider/UXWebviewProviderRegistry.h"
#include "UObject/Class.h"
#include "UUXWebview.h"
#include "UUXWebviewSubsystem.h"
#include "UXWebviewLog.h"
#include "UXWebviewSettings.h"

namespace
{
	FString StateName(EUXWebviewState State)
	{
		const UEnum* Enum = StaticEnum<EUXWebviewState>();
		return Enum ? Enum->GetNameStringByValue(static_cast<int64>(State)) : FString(TEXT("Unknown"));
	}

	/** Game instances that currently own a webview subsystem, PIE and game alike. */
	TArray<UUXWebviewSubsystem*> LiveSubsystems()
	{
		TArray<UUXWebviewSubsystem*> Result;
		TSet<UGameInstance*> Seen;
		if (!GEngine)
		{
			return Result;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World)
			{
				continue;
			}
			UGameInstance* GameInstance = World->GetGameInstance();
			if (!GameInstance || Seen.Contains(GameInstance))
			{
				continue;
			}
			Seen.Add(GameInstance);
			if (UUXWebviewSubsystem* Subsystem = GameInstance->GetSubsystem<UUXWebviewSubsystem>())
			{
				Result.Add(Subsystem);
			}
		}
		return Result;
	}

	void DumpState(const TArray<FString>& Args)
	{
		(void)Args;

		const UUXWebviewSettings& Settings = UUXWebviewSettings::Get();
		UE_LOG(LogUXWebview, Log,
			TEXT("state settings: defaultProvider='%s' fallbacks=[%s] allowFallback=%d directDisplay=%d compositionOverlay=%d devtools=%d:%d idleShutdown=%ds autoRecover=%d bridgeOrigins=%d"),
			*Settings.PlatformDefaultProvider,
			*FString::Join(Settings.FallbackProviders, TEXT(",")),
			Settings.bAllowProviderFallback ? 1 : 0,
			Settings.bDirectDisplay ? 1 : 0,
			Settings.bCompositionOverlay ? 1 : 0,
			Settings.bEnableDevTools ? 1 : 0,
			Settings.DevToolsPort,
			Settings.IdleShutdownSeconds,
			Settings.bAutoRecoverOnCrash ? 1 : 0,
			Settings.BridgeAllowedOrigins.Num());

		for (const FUXWebviewProviderInfo& Info : FUXWebviewProviderRegistry::Get().GetProviders())
		{
			const FString Reason = Info.bAvailable
				? FString()
				: FString::Printf(TEXT(" reason='%s'"), *Info.UnavailableReason);
			UE_LOG(LogUXWebview, Log,
				TEXT("state provider: %s (%s %s, %s) available=%d%s"),
				*Info.Name, *Info.DisplayName, *Info.Version, *Info.Platform,
				Info.bAvailable ? 1 : 0, *Reason);
		}

		int32 ViewCount = 0;
		for (UUXWebviewSubsystem* Subsystem : LiveSubsystems())
		{
			for (UUXWebview* View : Subsystem->GetWebviews())
			{
				if (!View)
				{
					continue;
				}
				const FIntPoint FrameSize = View->GetFrameSize();
				UE_LOG(LogUXWebview, Log,
					TEXT("state view %d: provider=%s state=%s overlay=%d frame=%dx%d frames=%d texture=%d"),
					ViewCount, *View->GetProviderName(), *StateName(View->GetState()),
					View->IsOverlayMode() ? 1 : 0, FrameSize.X, FrameSize.Y,
					View->GetFrameCount(), View->GetTexture() ? 1 : 0);
				++ViewCount;
			}
		}
		UE_LOG(LogUXWebview, Log, TEXT("state summary: %d live webview(s)"), ViewCount);
	}

	TArray<IConsoleObject*> Commands;
} // namespace

void UXWebviewDiagnostics::Register()
{
	Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("UXWebview.DumpState"),
		TEXT("Logs plugin settings, registered providers and every live webview."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&DumpState)));
}

void UXWebviewDiagnostics::Unregister()
{
	for (IConsoleObject* Command : Commands)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Command, false);
	}
	Commands.Reset();
}
