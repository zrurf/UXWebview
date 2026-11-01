#include "SUXWebviewWidget.h"

#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/ITextInputMethodSystem.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SWindow.h"
#include "UUXWebview.h"
#include "UXWebviewCoordinateMapper.h"
#include "UXWebviewTextInputContext.h"

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
	OverlayPosition = FIntPoint::ZeroValue;
	OverlaySize = FIntPoint::ZeroValue;
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
	if (TextInputContext.IsValid())
	{
		// The IME needs the panel transform for candidate placement.
		TextInputContext->UpdateCachedGeometry(AllottedGeometry);
	}

	UUXWebview* View = WebView.Get();
	if (View && View->IsOverlayMode())
	{
		// Overlay browsers are native child windows, so this widget neither
		// paints a texture nor owns their geometry; it only tracks the panel.
		UpdateOverlayBounds(AllottedGeometry, *View);
		return;
	}

	UpdateViewport(AllottedGeometry);
	UpdateBrush();
}

void SUXWebviewWidget::UpdateOverlayBounds(const FGeometry& AllottedGeometry, UUXWebview& View)
{
	// Absolute geometry is in screen pixels, so strip the window's client origin
	// before handing the panel rectangle to the host: it places its native
	// window relative to the client area. Without this step a windowed game
	// offsets the overlay by the window's desktop position.
	FVector2f ClientOrigin = FVector2f::ZeroVector;
	if (TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(SharedThis(this)))
	{
		const FSlateRect ClientRect = Window->GetClientRectInScreen();
		ClientOrigin = FVector2f(ClientRect.Left, ClientRect.Top);
	}

	const FVector2f AbsolutePosition = AllottedGeometry.GetAbsolutePosition();
	const FVector2f AbsoluteSize = AllottedGeometry.GetAbsoluteSize();
	const FIntPoint PixelPosition = FIntPoint(
		FMath::RoundToInt(AbsolutePosition.X - ClientOrigin.X),
		FMath::RoundToInt(AbsolutePosition.Y - ClientOrigin.Y));
	const FIntPoint PixelSize = FIntPoint(
		FMath::Max(1, FMath::RoundToInt(AbsoluteSize.X)),
		FMath::Max(1, FMath::RoundToInt(AbsoluteSize.Y)));
	if (PixelPosition == OverlayPosition && PixelSize == OverlaySize)
	{
		return;
	}

	OverlayPosition = PixelPosition;
	OverlaySize = PixelSize;
	const float LayoutScale = AllottedGeometry.Scale > 0.0f ? AllottedGeometry.Scale : 1.0f;
	EffectiveDpiScale = DpiScale > 0.0f ? DpiScale : LayoutScale;
	View.SetOverlayBounds(PixelPosition, PixelSize, EffectiveDpiScale);
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
	if (View->IsOverlayMode())
	{
		// Chromium draws the child window over this panel; painting a texture
		// here would only burn overdraw.
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
	RegisterTextInput();
	SyncFocus(true);
	return FReply::Handled();
}

void SUXWebviewWidget::OnFocusLost(const FFocusEvent& InFocusEvent)
{
	UnregisterTextInput();
	SyncFocus(false);
}

void SUXWebviewWidget::RegisterTextInput()
{
	if (TextInputContext.IsValid() || !bInteractable || !FSlateApplication::IsInitialized())
	{
		return;
	}

	UUXWebview* View = WebView.Get();
	if (!View)
	{
		return;
	}

	// Overlay browsers are real windows, so Chromium runs its own IME there.
	if (View->IsOverlayMode())
	{
		return;
	}

	ITextInputMethodSystem* System = FSlateApplication::Get().GetTextInputMethodSystem();
	if (!System)
	{
		return;
	}

	TextInputContext = MakeShared<FUXWebviewTextInputContext>(TWeakPtr<SUXWebviewWidget>(SharedThis(this)));
	TextInputNotifier = System->RegisterContext(TextInputContext.ToSharedRef());
	System->ActivateContext(TextInputContext.ToSharedRef());
}

void SUXWebviewWidget::UnregisterTextInput()
{
	if (!TextInputContext.IsValid())
	{
		return;
	}

	if (TextInputContext->IsComposing())
	{
		SendImeCancel();
	}

	if (FSlateApplication::IsInitialized())
	{
		if (ITextInputMethodSystem* System = FSlateApplication::Get().GetTextInputMethodSystem())
		{
			System->DeactivateContext(TextInputContext.ToSharedRef());
			System->UnregisterContext(TextInputContext.ToSharedRef());
		}
	}

	TextInputNotifier.Reset();
	TextInputContext.Reset();
}

void SUXWebviewWidget::SendImeComposition(const FString& Text)
{
	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::ImeComposition;
	Event.Text = Text;
	ForwardEvent(Event);
}

void SUXWebviewWidget::SendImeCommit(const FString& Text)
{
	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::ImeCommit;
	Event.Text = Text;
	ForwardEvent(Event);
}

void SUXWebviewWidget::SendImeCancel()
{
	FUXWebviewInputEvent Event;
	Event.Type = EUXWebviewInputType::ImeCancel;
	ForwardEvent(Event);
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
