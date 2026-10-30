#pragma once

#include "CoreMinimal.h"

// Lifecycle of a single frame texture inside the pool.
enum class EUXWebviewFrameSlotState : uint8
{
	Free,
	Copying,
	Ready,
	InUse
};

// Slot bookkeeping for the frame pool. Owns no GPU resources; the caller maps
// slot indices to textures and drives the transitions from the game thread.
// Header-only so both the runtime and other modules can use it without exports.
class FUXWebviewFrameSlotRing
{
public:
	explicit FUXWebviewFrameSlotRing(int32 InSlotCount = 3)
	{
		Reset(InSlotCount);
	}

	// Drops every slot back to Free and forgets the displayed one.
	void Reset(int32 InSlotCount)
	{
		States.Init(EUXWebviewFrameSlotState::Free, FMath::Max(0, InSlotCount));
		ReadyOrder.Reset();
		DisplaySlot = INDEX_NONE;
	}

	int32 NumSlots() const { return States.Num(); }

	EUXWebviewFrameSlotState GetState(int32 SlotIndex) const
	{
		check(States.IsValidIndex(SlotIndex));
		return States[SlotIndex];
	}

	int32 GetDisplaySlot() const { return DisplaySlot; }

	int32 CountInState(EUXWebviewFrameSlotState State) const
	{
		int32 Count = 0;
		for (const EUXWebviewFrameSlotState SlotState : States)
		{
			if (SlotState == State)
			{
				++Count;
			}
		}
		return Count;
	}

	// Claims a Free slot for an upcoming copy; INDEX_NONE when all slots are busy.
	int32 AcquireCopySlot()
	{
		for (int32 Index = 0; Index < States.Num(); ++Index)
		{
			if (States[Index] == EUXWebviewFrameSlotState::Free)
			{
				States[Index] = EUXWebviewFrameSlotState::Copying;
				return Index;
			}
		}
		return INDEX_NONE;
	}

	// Marks a finished copy as displayable. Fails on unknown or non-copying slots.
	bool CompleteCopy(int32 SlotIndex)
	{
		if (!States.IsValidIndex(SlotIndex)
			|| States[SlotIndex] != EUXWebviewFrameSlotState::Copying)
		{
			return false;
		}

		States[SlotIndex] = EUXWebviewFrameSlotState::Ready;
		ReadyOrder.Add(SlotIndex);
		return true;
	}

	// Returns a claimed slot to the free list after a failed copy.
	bool CancelCopy(int32 SlotIndex)
	{
		if (!States.IsValidIndex(SlotIndex)
			|| States[SlotIndex] != EUXWebviewFrameSlotState::Copying)
		{
			return false;
		}

		States[SlotIndex] = EUXWebviewFrameSlotState::Free;
		return true;
	}

	// Hands the display to the newest finished slot and releases stale ones.
	// Returns the slot to display, or INDEX_NONE when nothing new is ready.
	int32 TakeNewestReadySlot()
	{
		if (ReadyOrder.IsEmpty())
		{
			return INDEX_NONE;
		}

		const int32 Newest = ReadyOrder.Last();
		// Older finished frames are never shown once a newer one exists.
		for (int32 Index = 0; Index + 1 < ReadyOrder.Num(); ++Index)
		{
			States[ReadyOrder[Index]] = EUXWebviewFrameSlotState::Free;
		}
		ReadyOrder.Reset();

		if (DisplaySlot != INDEX_NONE && DisplaySlot != Newest)
		{
			States[DisplaySlot] = EUXWebviewFrameSlotState::Free;
		}

		States[Newest] = EUXWebviewFrameSlotState::InUse;
		DisplaySlot = Newest;
		return Newest;
	}

private:
	TArray<EUXWebviewFrameSlotState> States;
	// Completion order, oldest first.
	TArray<int32> ReadyOrder;
	int32 DisplaySlot = INDEX_NONE;
};
