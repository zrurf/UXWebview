#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "UXWebviewTypes.h"

#include "UUXWebviewSubsystem.generated.h"

class UUXWebview;

/** Owns every webview of a game instance and bridges to the provider registry. */
UCLASS()
class UXWEBVIEW_API UUXWebviewSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Creates and initializes a webview; returns null on failure (reason in OutResult). */
	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	UUXWebview* CreateWebview(const FUXWebviewCreateParams& Params, FUXWebviewResult& OutResult);

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	void DestroyWebview(UUXWebview* Webview);

	UFUNCTION(BlueprintPure, Category = "UXWebview")
	const TArray<UUXWebview*>& GetWebviews() const { return Webviews; }

	/** Provider list from the registry, including availability reasons. */
	UFUNCTION(BlueprintPure, Category = "UXWebview")
	TArray<FUXWebviewProviderInfo> GetProviders() const;

	virtual void Deinitialize() override;

private:
	UPROPERTY(Transient)
	TArray<UUXWebview*> Webviews;
};
