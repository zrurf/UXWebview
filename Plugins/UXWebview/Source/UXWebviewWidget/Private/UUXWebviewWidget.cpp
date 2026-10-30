#include "UUXWebviewWidget.h"

#include "SUXWebviewWidget.h"
#include "UUXWebview.h"

TSharedRef<SWidget> UUXWebviewWidget::RebuildWidget()
{
	SAssignNew(SlateWebview, SUXWebviewWidget)
		.WebView(WebView)
		.DpiScale(DpiScale)
		.bInteractable(bInteractable);

	UpdateWebViewVisibility();
	return SlateWebview.ToSharedRef();
}

void UUXWebviewWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	SlateWebview.Reset();
	Super::ReleaseSlateResources(bReleaseChildren);
}

void UUXWebviewWidget::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (SlateWebview.IsValid())
	{
		SlateWebview->SetWebView(WebView);
		SlateWebview->SetDpiScale(DpiScale);
		SlateWebview->SetInteractable(bInteractable);
	}
	UpdateWebViewVisibility();
}

void UUXWebviewWidget::SetVisibility(ESlateVisibility InVisibility)
{
	Super::SetVisibility(InVisibility);
	UpdateWebViewVisibility();
}

void UUXWebviewWidget::SetWebView(UUXWebview* InWebView)
{
	WebView = InWebView;
	if (SlateWebview.IsValid())
	{
		SlateWebview->SetWebView(InWebView);
	}
	UpdateWebViewVisibility();
}

void UUXWebviewWidget::SetDpiScale(float InDpiScale)
{
	DpiScale = FMath::Clamp(InDpiScale, 0.5f, 4.0f);
	if (SlateWebview.IsValid())
	{
		SlateWebview->SetDpiScale(DpiScale);
	}
}

void UUXWebviewWidget::SetInteractable(bool bInInteractable)
{
	bInteractable = bInInteractable;
	if (SlateWebview.IsValid())
	{
		SlateWebview->SetInteractable(bInInteractable);
	}
}

void UUXWebviewWidget::UpdateWebViewVisibility()
{
	if (!WebView)
	{
		return;
	}

	const ESlateVisibility CurrentVisibility = GetVisibility();
	const bool bVisible = CurrentVisibility == ESlateVisibility::Visible
		|| CurrentVisibility == ESlateVisibility::HitTestInvisible
		|| CurrentVisibility == ESlateVisibility::SelfHitTestInvisible;
	WebView->SetVisible(bVisible);
}
