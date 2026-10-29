#include "UXWebviewSettings.h"

UUXWebviewSettings::UUXWebviewSettings()
{
	// Windows tries CEF first and falls back to WebView2; other platforms only CEF.
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
