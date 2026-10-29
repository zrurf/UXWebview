#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "UXWebviewSettings.generated.h"

/** Project wide defaults. Per-instance FUXWebviewCreateParams always win. */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "UXWebview"))
class UXWEBVIEW_API UUXWebviewSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UUXWebviewSettings();

	/** Provider name override for this platform; empty uses the built-in default. */
	UPROPERTY(config, EditAnywhere, Category = "Providers")
	FString PlatformDefaultProvider;

	/** Tried in order when the resolved provider is unavailable. */
	UPROPERTY(config, EditAnywhere, Category = "Providers")
	TArray<FString> FallbackProviders;

	/** When false, an explicitly requested but unavailable provider fails instead of falling back. */
	UPROPERTY(config, EditAnywhere, Category = "Providers")
	bool bAllowProviderFallback = true;

	/** Origins allowed to call game functions from the script bridge. */
	UPROPERTY(config, EditAnywhere, Category = "Bridge")
	TArray<FString> BridgeAllowedOrigins;

	UPROPERTY(config, EditAnywhere, Category = "Debug")
	bool bEnableDevTools = false;

	UPROPERTY(config, EditAnywhere, Category = "Debug")
	int32 DevToolsPort = 9222;

	/** Host process exits after this many idle seconds with no views. */
	UPROPERTY(config, EditAnywhere, Category = "Host")
	int32 IdleShutdownSeconds = 30;

	UPROPERTY(config, EditAnywhere, Category = "Host")
	bool bAutoRecoverOnCrash = true;

	static const UUXWebviewSettings& Get();

	virtual FName GetCategoryName() const override;
};
