#include "UXWebviewSample.h"

#include "Modules/ModuleManager.h"
#include "UUXWebview.h"

/** Starts the demo from the command line so a packaged Shipping build can be smoke-tested. */
class FUXWebviewSampleModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
		// Console commands are stripped from Shipping, so the demo entry point
		// has to come from the command line in a packaged run.
		UXWebviewSampleLaunchDemoFromCommandLine();
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FUXWebviewSampleModule, UXWebviewSample, "UXWebviewSample");

void UUXWebviewSampleBridgeHandler::SetView(UUXWebview* InView)
{
	View = InView;
}

UUXWebview* UUXWebviewSampleBridgeHandler::GetView() const
{
	return View.Get();
}

void UUXWebviewSampleBridgeHandler::HandleScriptMessage(const FString& Name,
	const FString& PayloadJson, int32 RequestId)
{
	UE_LOG(LogTemp, Log, TEXT("sample bridge '%s' payload=%s"), *Name, *PayloadJson);

	UUXWebview* Webview = View.Get();
	if (!Webview)
	{
		return;
	}

	// Echo the payload so the page can assert the round trip.
	Webview->RespondToScriptMessage(RequestId, true,
		PayloadJson.IsEmpty() ? FString(TEXT("null")) : PayloadJson);
}

void UUXWebviewSampleBridgeHandler::HandleKeyboardRequest(const FString& PayloadJson)
{
	UE_LOG(LogTemp, Log, TEXT("sample keyboard request payload=%s"), *PayloadJson);

	UUXWebview* Webview = View.Get();
	if (!Webview)
	{
		return;
	}

	const FUXWebviewResult Result = Webview->EnsureKeyboardInput();
	UE_LOG(LogTemp, Log, TEXT("sample keyboard request ok=%d '%s'"),
		Result.bSuccess ? 1 : 0, *Result.Message);
}

void UUXWebviewSampleBridgeHandler::HandleNewWindow(const FString& Url,
	const FString& FrameName)
{
	UE_LOG(LogTemp, Log, TEXT("sample new window url='%s' frame='%s'"), *Url, *FrameName);
}

void UUXWebviewSampleBridgeHandler::HandleDownload(const FUXWebviewDownloadInfo& Info)
{
	UE_LOG(LogTemp, Log, TEXT("sample download state=%d file='%s' (%d/%d)"),
		static_cast<int32>(Info.State), *Info.FilePath, Info.ReceivedBytes, Info.TotalBytes);
}
