#pragma once

#include "CoreMinimal.h"

#include "Blueprint/UserWidget.h"

#include "UUXWebviewWidget.generated.h"

class UUXWebview;

/**
 * UMG wrapper for SUXWebviewWidget. Assign a webview, add the widget to a
 * viewport or panel, and the webview texture is drawn with full input routing.
 */
UCLASS(BlueprintType, meta = (DisplayName = "UX Webview"))
class UXWEBVIEWWIDGET_API UUXWebviewWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// UWidget
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	virtual void SetVisibility(ESlateVisibility InVisibility) override;
	virtual void SynchronizeProperties() override;

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	void SetWebView(UUXWebview* InWebView);

	UFUNCTION(BlueprintPure, Category = "UXWebview")
	UUXWebview* GetWebView() const { return WebView; }

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	void SetDpiScale(float InDpiScale);

	UFUNCTION(BlueprintCallable, Category = "UXWebview")
	void SetInteractable(bool bInInteractable);

	/** Webview drawn by this widget. Not owned by the widget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	TObjectPtr<UUXWebview> WebView;

	/** UI scale used to convert local input to CSS pixels and size the viewport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview", meta = (ClampMin = "0.5", ClampMax = "4.0"))
	float DpiScale = 1.0f;

	/** Disables hit tests and keyboard focus when false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	bool bInteractable = true;

private:
	void UpdateWebViewVisibility();

	TSharedPtr<class SUXWebviewWidget> SlateWebview;
};
