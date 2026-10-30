#include "SUXWebviewWidget.h"

#include "Engine/Texture2D.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Styling/SlateBrush.h"
#include "UUXWebview.h"
#include "UXWebviewCoordinateMapper.h"

void SUXWebviewWidget::Construct(const FArguments& InArgs)
{
	WebView = InArgs._WebView;
	DpiScale = InArgs._DpiScale > 0.0f ? InArgs._DpiScale : 0.0f;
	bInteractable = InArgs._bInteractable;
	DesiredSize = InArgs._DesiredSize;

	Filter.Reset();
	SetCanTick(true);
}

void SUXWebviewWidget::SetWebView(UUXWebview* InWebView)
{
	WebView = InWebView;
	ViewportSize = FIntPoint::ZeroValue;
	LastTexture = nullptr;
	if (Brush.IsValid())
	{
		Brush->SetResourceObject(nullptr);
	}
	Filter.Reset();
	Invalidate(EInvalidateWidgetReason::Paint);
}

void SUXWebviewWidget::SetDpiScale(float InDpiScale)
{
	// Zero means "follow the layout scale" so web text stays sharp on scaled displays.
	DpiScale = InDpiScale > 0.0f ? InDpiScale : 0.0f;
	ViewportSize = FIntPoint::ZeroValue;
}

void SUXWebviewWidget::SetInteractable(bool bInInteractable)
{
	bInteractable = bInInteractable;
}

FVector2D SUXWebviewWidget::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return DesiredSize;
}

bool SUXWebviewWidget::SupportsKeyboardFocus() const
{
	return bInteractable;
}

bool SUXWebviewWidget::IsInteractable() const
{
	return bInteractable;
}

void SUXWebviewWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	UpdateViewport(AllottedGeometry);
	UpdateBrush();
}

void SUXWebviewWidget::UpdateViewport(const FGeometry& AllottedGeometry)
{
	UUXWebview* View = WebView.Get();
	if (!View)
	{
		return;
	}

	const FVector2D WidgetSize = AllottedGeometry.GetLocalSize();
	// The layout scale is the physical pixel ratio of this panel; rendering at
	// that ratio keeps web text as sharp as native UMG text.
	const float LayoutScale = AllottedGeometry.Scale > 0.0f ? AllottedGeometry.Scale : 1.0f;
	const float EffectiveDpi = DpiScale > 0.0f ? DpiScale : LayoutScale;
	const FIntPoint Desired = FUXWebviewCoordinateMapper::ComputeViewportSize(WidgetSize, EffectiveDpi);
	if (Desired == ViewportSize && FMath::IsNearlyEqual(EffectiveDpi, EffectiveDpiScale))
	{
		return;
	}

	EffectiveDpiScale = EffectiveDpi;
	ViewportSize = Desired;
	View->SetViewport(ViewportSize, EffectiveDpiScale);
}

void SUXWebviewWidget::UpdateBrush()
{
	UUXWebview* View = WebView.Get();
	UTexture2D* Texture = View ? View->GetTexture() : nullptr;
	if (Texture == LastTexture)
	{
		return;
	}

	LastTexture = Texture;
	if (!Brush.IsValid())
	{
		Brush = MakeShared<FSlateBrush>();
		Brush->DrawAs = ESlateBrushDrawType::Image;
		Brush->ImageType = ESlateBrushImageType::FullColor;
	}
	Brush->SetResourceObject(Texture);
	if (Texture)
	{
		Brush->ImageSize = FVector2D(Texture->GetSizeX(), Texture->GetSizeY());
	}

	Invalidate(EInvalidateWidgetReason::Paint);
}

int32 SUXWebviewWidget::OnPaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	UUXWebview* View = WebView.Get();
	if (!Brush.IsValid() || LastTexture == nullptr || !View || View->GetState() == EUXWebviewState::Destroyed)
	{
		return LayerId;
	}

	FSlateDrawElement::MakeBox(
		OutDrawElements,
		LayerId,
		AllottedGeometry.ToPaintGeometry(),
		Brush.Get(),
		ESlateDrawEffect::None,
		FLinearColor::White);

	return LayerId + 1;
}

FCursorReply SUXWebviewWidget::OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const
{
	// CEF sends cursor updates in M5; until then the arrow is the safe default.
	return bInteractable ? FCursorReply::Cursor(EMouseCursor::Default) : FCursorReply::Unhandled();
}

FReply SUXWebviewWidget::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	SyncFocus(true);
	return FReply::Handled();
}

void SUXWebviewWidget::OnFocusLost(const FFocusEvent& InFocusEvent)
{
	SyncFocus(false);
}

void SUXWebviewWidget::SyncFocus(bool bFocused)
{
	if (!bInteractable || !Filter.ShouldSendFocus(bFocused))
	{
		return;
	}

	if (UUXWebview* View = WebView.Get())
	{
		View->SetFocus(bFocused);
	}
}

FVector2D SUXWebviewWidget::LocalToCss(const FGeometry& Geometry, const FVector2D& LocalPosition) const
{
	return FUXWebviewCoordinateMapper::LocalToCss(
		LocalPosition, Geometry.GetLocalSize(), ViewportSize, EffectiveDpiScale);
}

int32 SUXWebviewWidget::MapButton(const FKey& ButtonKey) const
{
	if (ButtonKey == EKeys::RightMouseButton)
	{
		return 1;
	}
	if (ButtonKey == EKeys::MiddleMouseButton)
	{
		return 2;
	}
	return 0;
}

void SUXWebviewWidget::FillModifiers(FUXWebviewInputEvent& Event, const FInputEvent& InputEvent) const
{
	Event.bShift = InputEvent.IsShiftDown();
	Event.bControl = InputEvent.IsControlDown();
	Event.bAlt = InputEvent.IsAltDown();
}

void SUXWebviewWidget::ForwardEvent(const FUXWebviewInputEvent& Event) const
{
	if (UUXWebview* View = WebView.Get())
	{
		View->SendInput(Event);
	}
}

FReply SUXWebviewWidget::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bInteractable)
	{
		return FReply::Unhandled();
	}

	const FVector2D LocalPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::MouseMove;
	Event.Position = LocalToCss(MyGeometry, LocalPosition);
	FillModifiers(Event, MouseEvent);

	if (Filter.ShouldSendMouseMove(Event.Position))
	{
		ForwardEvent(Event);
	}
	return FReply::Handled();
}

void SUXWebviewWidget::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	Filter.InvalidateLastMove();
}

FReply SUXWebviewWidget::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bInteractable)
	{
		return FReply::Unhandled();
	}

	const FVector2D LocalPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::MouseDown;
	Event.Position = LocalToCss(MyGeometry, LocalPosition);
	Event.Button = MapButton(MouseEvent.GetEffectingButton());
	FillModifiers(Event, MouseEvent);

	if (Filter.ShouldSendMouseButton(true, Event.Button))
	{
		ForwardEvent(Event);
	}

	// The first click both focuses the webview and reaches the page.
	return FReply::Handled()
		.SetUserFocus(SharedThis(this), EFocusCause::Mouse)
		.CaptureMouse(SharedThis(this));
}

FReply SUXWebviewWidget::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bInteractable)
	{
		return FReply::Unhandled();
	}

	const FVector2D LocalPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::MouseUp;
	Event.Position = LocalToCss(MyGeometry, LocalPosition);
	Event.Button = MapButton(MouseEvent.GetEffectingButton());
	FillModifiers(Event, MouseEvent);

	if (Filter.ShouldSendMouseButton(false, Event.Button))
	{
		ForwardEvent(Event);
	}

	return FReply::Handled().ReleaseMouseCapture();
}

FReply SUXWebviewWidget::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bInteractable)
	{
		return FReply::Unhandled();
	}

	const FVector2D LocalPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::MouseWheel;
	Event.Position = LocalToCss(MyGeometry, LocalPosition);
	Event.WheelDelta = MouseEvent.GetWheelDelta();
	FillModifiers(Event, MouseEvent);
	ForwardEvent(Event);
	return FReply::Handled();
}

FReply SUXWebviewWidget::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (!bInteractable || !HasKeyboardFocus())
	{
		return FReply::Unhandled();
	}

	const uint32* KeyCode = nullptr;
	const uint32* CharCode = nullptr;
	FInputKeyManager::Get().GetCodesFromKey(InKeyEvent.GetKey(), KeyCode, CharCode);

	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::KeyDown;
	Event.KeyCode = KeyCode ? static_cast<int32>(*KeyCode) : 0;
	FillModifiers(Event, InKeyEvent);
	ForwardEvent(Event);
	return FReply::Handled();
}

FReply SUXWebviewWidget::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (!bInteractable || !HasKeyboardFocus())
	{
		return FReply::Unhandled();
	}

	const uint32* KeyCode = nullptr;
	const uint32* CharCode = nullptr;
	FInputKeyManager::Get().GetCodesFromKey(InKeyEvent.GetKey(), KeyCode, CharCode);

	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::KeyUp;
	Event.KeyCode = KeyCode ? static_cast<int32>(*KeyCode) : 0;
	FillModifiers(Event, InKeyEvent);
	ForwardEvent(Event);
	return FReply::Handled();
}

FReply SUXWebviewWidget::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	if (!bInteractable || !HasKeyboardFocus())
	{
		return FReply::Unhandled();
	}

	const TCHAR Character = InCharacterEvent.GetCharacter();
	if (Character == 0)
	{
		return FReply::Handled();
	}

	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::Char;
	Event.Text.AppendChar(Character);
	FillModifiers(Event, InCharacterEvent);
	ForwardEvent(Event);
	return FReply::Handled();
}
