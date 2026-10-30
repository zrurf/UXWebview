#include "UXWebviewTextInputContext.h"

#include "Framework/Application/SlateApplication.h"
#include "SUXWebviewWidget.h"
#include "Widgets/SWindow.h"

FUXWebviewTextInputContext::FUXWebviewTextInputContext(const TWeakPtr<SUXWebviewWidget>& InWidget)
	: Widget(InWidget)
{
}

void FUXWebviewTextInputContext::UpdateCachedGeometry(const FGeometry& InGeometry)
{
	CachedGeometry = InGeometry;
}

bool FUXWebviewTextInputContext::IsComposing()
{
	return bIsComposing;
}

bool FUXWebviewTextInputContext::IsReadOnly()
{
	// The page owns its edit model, so the IME must always be allowed to write.
	return false;
}

uint32 FUXWebviewTextInputContext::GetTextLength()
{
	return static_cast<uint32>(CompositionSequence.Len());
}

void FUXWebviewTextInputContext::GetSelectionRange(uint32& OutBeginIndex, uint32& OutLength, ECaretPosition& OutCaretPosition)
{
	OutBeginIndex = SelectionBegin;
	OutLength = SelectionLength;
	OutCaretPosition = SelectionCaret;
}

void FUXWebviewTextInputContext::SetSelectionRange(const uint32 InBeginIndex, const uint32 InLength, const ECaretPosition InCaretPosition)
{
	SelectionBegin = InBeginIndex;
	SelectionLength = InLength;
	SelectionCaret = InCaretPosition;
}

void FUXWebviewTextInputContext::GetTextInRange(const uint32 InBeginIndex, const uint32 InLength, FString& OutString)
{
	OutString = CompositionSequence.Mid(static_cast<int32>(InBeginIndex), static_cast<int32>(InLength));
}

void FUXWebviewTextInputContext::SetTextInRange(const uint32 InBeginIndex, const uint32 InLength, const FString& InString)
{
	const int32 Begin = static_cast<int32>(FMath::Min<uint32>(InBeginIndex, static_cast<uint32>(CompositionSequence.Len())));
	const int32 End = static_cast<int32>(FMath::Min<uint32>(InBeginIndex + InLength, static_cast<uint32>(CompositionSequence.Len())));

	FString NewSequence = CompositionSequence.Left(Begin);
	NewSequence += InString;
	NewSequence += CompositionSequence.Mid(End);

	CompositionSequence = NewSequence;
	CompositionString = InString;

	if (const TSharedPtr<SUXWebviewWidget> PinnedWidget = Widget.Pin())
	{
		PinnedWidget->SendImeComposition(CompositionString);
	}
}

int32 FUXWebviewTextInputContext::GetCharacterIndexFromPoint(const FVector2D& InPoint)
{
	// Chromium owns the text layout, so there is no local character map to hit test.
	return INDEX_NONE;
}

bool FUXWebviewTextInputContext::GetTextBounds(const uint32 InBeginIndex, const uint32 InLength, FVector2D& OutPosition, FVector2D& OutSize)
{
	// Reporting the whole panel keeps the candidate window next to the panel.
	GetScreenBounds(OutPosition, OutSize);
	OutSize = FVector2D::ZeroVector;
	return false;
}

void FUXWebviewTextInputContext::GetScreenBounds(FVector2D& OutPosition, FVector2D& OutSize)
{
	const FVector2D Translation = CachedGeometry.GetAccumulatedRenderTransform().GetTranslation();
	OutPosition = Translation;
	OutSize = FVector2D(CachedGeometry.GetLocalSize());
}

TSharedPtr<SWindow> FUXWebviewTextInputContext::FindSlateWindow() const
{
	if (const TSharedPtr<SUXWebviewWidget> PinnedWidget = Widget.Pin())
	{
		if (FSlateApplication::IsInitialized())
		{
			return FSlateApplication::Get().FindWidgetWindow(PinnedWidget.ToSharedRef());
		}
	}
	return nullptr;
}

TSharedPtr<FGenericWindow> FUXWebviewTextInputContext::GetWindow()
{
	const TSharedPtr<SWindow> SlateWindow = FindSlateWindow();
	return SlateWindow.IsValid() ? SlateWindow->GetNativeWindow() : nullptr;
}

void FUXWebviewTextInputContext::BeginComposition()
{
	bIsComposing = true;
}

void FUXWebviewTextInputContext::UpdateCompositionRange(const int32 InBeginIndex, const uint32 InLength)
{
	SelectionBegin = FMath::Max(InBeginIndex, 0);
	SelectionLength = InLength;
}

void FUXWebviewTextInputContext::EndComposition()
{
	if (!bIsComposing)
	{
		return;
	}
	bIsComposing = false;

	if (const TSharedPtr<SUXWebviewWidget> PinnedWidget = Widget.Pin())
	{
		if (CompositionString.Len() > 0)
		{
			PinnedWidget->SendImeCommit(CompositionString);
		}
		else
		{
			PinnedWidget->SendImeCancel();
		}
	}

	ResetComposition();
}

void FUXWebviewTextInputContext::ResetComposition()
{
	CompositionSequence.Empty();
	CompositionString.Empty();
	SelectionBegin = 0;
	SelectionLength = 0;
	SelectionCaret = ECaretPosition::Ending;
}
