#include "UXWebviewSettings.h"

UUXWebviewSettings::UUXWebviewSettings()
{
	// Same fallback order everywhere; providers that are not registered for the
	// current platform are skipped by the resolver.
	FallbackProviders.Add(TEXT("CEF"));
	FallbackProviders.Add(TEXT("WebView2"));
}

const UUXWebviewSettings& UUXWebviewSettings::Get()
{
	return *GetDefault<UUXWebviewSettings>();
}

FName UUXWebviewSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}
