#pragma once

#include "CoreMinimal.h"

#include "GenericPlatform/ITextInputMethodSystem.h"
#include "Layout/Geometry.h"

class SUXWebviewWidget;
class SWindow;

/**
 * Slate text input context for a webview panel.
 *
 * Slate's IME plumbing only calls into widgets that implement
 * ITextInputMethodContext; mirroring the engine's SWebBrowser handler lets the
 * platform IME compose text for a page that lives in another process.
 */
class FUXWebviewTextInputContext : public ITextInputMethodContext, public TSharedFromThis<FUXWebviewTextInputContext>
{
public:
	explicit FUXWebviewTextInputContext(const TWeakPtr<SUXWebviewWidget>& InWidget);

	void UpdateCachedGeometry(const FGeometry& InGeometry);

	// ITextInputMethodContext
	virtual bool IsComposing() override;
	virtual bool IsReadOnly() override;
	virtual uint32 GetTextLength() override;
	virtual void GetSelectionRange(uint32& OutBeginIndex, uint32& OutLength, ECaretPosition& OutCaretPosition) override;
	virtual void SetSelectionRange(const uint32 InBeginIndex, const uint32 InLength, const ECaretPosition InCaretPosition) override;
	virtual void GetTextInRange(const uint32 InBeginIndex, const uint32 InLength, FString& OutString) override;
	virtual void SetTextInRange(const uint32 InBeginIndex, const uint32 InLength, const FString& InString) override;
	virtual int32 GetCharacterIndexFromPoint(const FVector2D& InPoint) override;
	virtual bool GetTextBounds(const uint32 InBeginIndex, const uint32 InLength, FVector2D& OutPosition, FVector2D& OutSize) override;
	virtual void GetScreenBounds(FVector2D& OutPosition, FVector2D& OutSize) override;
	virtual TSharedPtr<FGenericWindow> GetWindow() override;
	virtual void BeginComposition() override;
	virtual void UpdateCompositionRange(const int32 InBeginIndex, const uint32 InLength) override;
	virtual void EndComposition() override;

private:
	void ResetComposition();
	TSharedPtr<SWindow> FindSlateWindow() const;

	TWeakPtr<SUXWebviewWidget> Widget;
	/** Geometry the IME uses to place candidates and to map screen points. */
	FGeometry CachedGeometry;

	bool bIsComposing = false;
	/** All characters composed since composition began, needed for range queries. */
	FString CompositionSequence;
	/** The last chunk reported by the IME, which is what the page should show. */
	FString CompositionString;
	uint32 SelectionBegin = 0;
	uint32 SelectionLength = 0;
	ECaretPosition SelectionCaret = ECaretPosition::Ending;
};
