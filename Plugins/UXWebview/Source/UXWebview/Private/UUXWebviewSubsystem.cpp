#include "UUXWebviewSubsystem.h"

#include "Provider/UXWebviewProviderRegistry.h"
#include "UUXWebview.h"
#include "UXWebviewLog.h"

UUXWebview* UUXWebviewSubsystem::CreateWebview(
	const FUXWebviewCreateParams& Params, FUXWebviewResult& OutResult)
{
	UUXWebview* Webview = NewObject<UUXWebview>(this);
	OutResult = Webview->Initialize(Params);
	if (!OutResult.bSuccess)
	{
		UE_LOG(LogUXWebview, Warning, TEXT("CreateWebview failed: %s"), *OutResult.Message);
		return nullptr;
	}

	Webviews.Add(Webview);
	return Webview;
}

void UUXWebviewSubsystem::DestroyWebview(UUXWebview* Webview)
{
	if (!Webview)
	{
		return;
	}
	Webviews.Remove(Webview);
	Webview->Destroy();
}

TArray<FUXWebviewProviderInfo> UUXWebviewSubsystem::GetProviders() const
{
	return FUXWebviewProviderRegistry::Get().GetProviders();
}

void UUXWebviewSubsystem::Deinitialize()
{
	for (UUXWebview* Webview : Webviews)
	{
		if (Webview)
		{
			Webview->Destroy();
		}
	}
	Webviews.Reset();
	Super::Deinitialize();
}
