#pragma once

#include "CoreMinimal.h"

#include "UXWebviewInputFilter.h"
#include "UXWebviewTypes.h"
#include "Widgets/SLeafWidget.h"

class UUXWebview;
class UTexture2D;
struct FSlateBrush;
struct FKey;
struct FInputEvent;

/**
 * Leaf widget that paints the webview texture and forwards mouse, wheel, key
 * and focus events to UUXWebview. Input coordinates are converted to CSS pixels
 * using the widget DPI so the same click lands on the same page point at any DPI.
 */
class SUXWebviewWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SUXWebviewWidget)
		: _WebView(nullptr)
		, _DpiScale(0.0f)
		, _bInteractable(true)
		, _DesiredSize(512.0f, 288.0f)
	{}
		SLATE_ARGUMENT(UUXWebview*, WebView)
		SLATE_ARGUMENT(float, DpiScale)
		SLATE_ARGUMENT(bool, bInteractable)
		SLATE_ARGUMENT(FVector2D, DesiredSize)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetWebView(UUXWebview* InWebView);
	UUXWebview* GetWebView() const { return WebView.Get(); }
	void SetDpiScale(float InDpiScale);
	void SetInteractable(bool bInInteractable);

	// SWidget
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual bool SupportsKeyboardFocus() const override;
	virtual bool IsInteractable() const override;
	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;
	virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
	virtual void OnFocusLost(const FFocusEvent& InFocusEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;

private:
	void UpdateViewport(const FGeometry& AllottedGeometry);
	void UpdateBrush();
	void SyncFocus(bool bFocused);
	FVector2D LocalToCss(const FGeometry& Geometry, const FVector2D& LocalPosition) const;
	void FillModifiers(FUXWebviewInputEvent& Event, const FInputEvent& InputEvent) const;
	int32 MapButton(const FKey& ButtonKey) const;
	void ForwardEvent(const FUXWebviewInputEvent& Event) const;

	TWeakObjectPtr<UUXWebview> WebView;
	TSharedPtr<FSlateBrush> Brush;
	UTexture2D* LastTexture = nullptr;
	FUXWebviewInputFilter Filter;
	FIntPoint ViewportSize = FIntPoint::ZeroValue;
	FVector2D DesiredSize = FVector2D(512.0f, 288.0f);
	/** Zero follows the layout scale so text renders at native pixel density. */
	float DpiScale = 0.0f;
	float EffectiveDpiScale = 1.0f;
	bool bInteractable = true;
};
