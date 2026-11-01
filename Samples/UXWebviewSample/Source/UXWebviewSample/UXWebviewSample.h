#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UXWebviewTypes.h"

#include "UXWebviewSample.generated.h"

class UUXWebview;

/** Demo-only page bridge handler: replies with the payload it received. */
UCLASS()
class UUXWebviewSampleBridgeHandler : public UObject
{
	GENERATED_BODY()

public:
	void SetView(UUXWebview* InView);
	UUXWebview* GetView() const;

	UFUNCTION()
	void HandleScriptMessage(const FString& Name, const FString& PayloadJson, int32 RequestId);

	/** A page asked for typed input, so the demo drops composition hosting. */
	UFUNCTION()
	void HandleKeyboardRequest(const FString& PayloadJson);

	/** Broadcast new-window policy: the demo only logs what the page asked for. */
	UFUNCTION()
	void HandleNewWindow(const FString& Url, const FString& FrameName);

	UFUNCTION()
	void HandleDownload(const FUXWebviewDownloadInfo& Info);

private:
	TWeakObjectPtr<UUXWebview> View;
};
