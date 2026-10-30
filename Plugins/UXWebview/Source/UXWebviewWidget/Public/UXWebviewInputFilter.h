#pragma once

#include "CoreMinimal.h"

/**
 * Drops duplicate input before it reaches the provider: repeated focus changes
 * and mouse moves that did not actually move, plus duplicate button presses.
 */
struct FUXWebviewInputFilter
{
	void Reset()
	{
		bFocusKnown = false;
		bFocused = false;
		bHasLastMove = false;
		LastMove = FVector2D::ZeroVector;
		for (bool& Down : ButtonsDown)
		{
			Down = false;
		}
	}

	/** True when the focus state differs from the last forwarded one. */
	bool ShouldSendFocus(bool bInFocused)
	{
		if (bFocusKnown && bFocused == bInFocused)
		{
			return false;
		}
		bFocusKnown = true;
		bFocused = bInFocused;
		return true;
	}

	/** True when the pointer moved since the last forwarded position. */
	bool ShouldSendMouseMove(const FVector2D& Position)
	{
		if (bHasLastMove && LastMove.Equals(Position, 0.01))
		{
			return false;
		}
		bHasLastMove = true;
		LastMove = Position;
		return true;
	}

	/** True for the first press or release of a button; a repeat is dropped. */
	bool ShouldSendMouseButton(bool bPressed, int32 Button)
	{
		if (Button < 0 || Button >= UE_ARRAY_COUNT(ButtonsDown))
		{
			return true;
		}
		if (ButtonsDown[Button] == bPressed)
		{
			return false;
		}
		ButtonsDown[Button] = bPressed;
		return true;
	}

	/** Forget the last move so re-entering the widget sends a fresh one. */
	void InvalidateLastMove()
	{
		bHasLastMove = false;
	}

private:
	bool bFocusKnown = false;
	bool bFocused = false;
	bool bHasLastMove = false;
	FVector2D LastMove = FVector2D::ZeroVector;

	/** Left, right and middle. */
	bool ButtonsDown[3] = {false, false, false};
};
