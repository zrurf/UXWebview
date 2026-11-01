#include "UXWebviewSample.h"

#include "Modules/ModuleManager.h"
#include "UUXWebview.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, UXWebviewSample, "UXWebviewSample");

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
