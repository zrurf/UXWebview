#pragma once

#include "CoreMinimal.h"

#include "UXWebviewTypes.h"
#include "Provider/IUXWebviewProvider.h"

/**
 * Provider registry. Registration happens on module startup/shutdown; resolution
 * reads settings and follows: explicit > platform default > built-in default > fallback.
 */
class UXWEBVIEW_API FUXWebviewProviderRegistry
{
public:
	static FUXWebviewProviderRegistry& Get();

	void RegisterFactory(TSharedPtr<IUXWebviewProviderFactory> Factory);
	void UnregisterFactory(const FString& Name);

	TArray<FUXWebviewProviderInfo> GetProviders() const;
	bool FindProvider(const FString& Name, FUXWebviewProviderInfo& OutInfo) const;

	/** Writes the chosen provider name into OutName; failures aggregate every reason. */
	FUXWebviewResult Resolve(const FUXWebviewCreateParams& Params, FString& OutName) const;

	/** Instantiates a registered provider; the caller owns the result. */
	FUXWebviewResult CreateProvider(const FString& Name, TUniquePtr<IUXWebviewProvider>& OutProvider) const;

private:
	FUXWebviewProviderRegistry() = default;

	/** Case-insensitive lookup; returns null when missing. */
	TSharedPtr<IUXWebviewProviderFactory> FindFactory(const FString& Name) const;
	static FString BuiltInDefaultProvider();

	TMap<FString, TSharedPtr<IUXWebviewProviderFactory>> Factories;
	mutable FCriticalSection Lock;
};
