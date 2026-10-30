#pragma once

#include "CoreMinimal.h"

// Frame rate policy: visibility first, then a short interaction boost, then the
// declared animation state. A rate of 0 means the host must not be asked for a
// frame at all, which keeps a hidden page completely idle.
struct FUXWebviewGovernorConfig
{
	float IdleFrameRate = 15.0f;
	float ActiveFrameRate = 60.0f;
	float MaxFrameRate = 120.0f;
	float InteractionHoldSeconds = 0.5f;
};

// Header-only so every module can own an instance without exports.
class FUXWebviewFrameGovernor
{
public:
	FUXWebviewFrameGovernor() = default;

	explicit FUXWebviewFrameGovernor(const FUXWebviewGovernorConfig& InConfig)
		: Config(InConfig)
	{
	}

	void SetConfig(const FUXWebviewGovernorConfig& InConfig) { Config = InConfig; }
	const FUXWebviewGovernorConfig& GetConfig() const { return Config; }

	void SetVisible(bool bInVisible)
	{
		bVisible = bInVisible;
		if (!bVisible)
		{
			InteractionRemaining = 0.0f;
		}
	}
	bool IsVisible() const { return bVisible; }

	// Pages are assumed to animate unless the caller declares otherwise.
	void SetAnimated(bool bInAnimated) { bAnimated = bInAnimated; }
	bool IsAnimated() const { return bAnimated; }

	// Keeps the frame rate at MaxFrameRate for InteractionHoldSeconds.
	void NotifyInteraction()
	{
		if (bVisible)
		{
			InteractionRemaining = Config.InteractionHoldSeconds;
		}
	}
	bool IsInteracting() const { return InteractionRemaining > 0.0f; }

	// Advances the interaction timer; returns the frame rate for this frame.
	float Update(float DeltaSeconds)
	{
		if (InteractionRemaining > 0.0f)
		{
			InteractionRemaining = FMath::Max(0.0f, InteractionRemaining - DeltaSeconds);
		}
		return GetTargetFrameRate();
	}

	float GetTargetFrameRate() const
	{
		if (!bVisible)
		{
			return 0.0f;
		}
		if (InteractionRemaining > 0.0f)
		{
			return Config.MaxFrameRate;
		}
		const float Base = bAnimated ? Config.ActiveFrameRate : Config.IdleFrameRate;
		return FMath::Min(Base, Config.MaxFrameRate);
	}

private:
	FUXWebviewGovernorConfig Config;
	bool bVisible = true;
	bool bAnimated = true;
	float InteractionRemaining = 0.0f;
};
