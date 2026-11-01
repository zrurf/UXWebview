#pragma once

#include "CoreMinimal.h"

/**
 * Converts Slate local (DIP) positions into web CSS pixels.
 * Formula: Web = Local / WidgetSize * ViewportSize / DpiScale.
 * ViewportSize is physical pixels, so dividing by DpiScale leaves CSS pixels.
 * Keeping it in one place avoids per-event copies of the math.
 */
struct FUXWebviewCoordinateMapper
{
	static FVector2D LocalToCss(
		const FVector2D& LocalPosition,
		const FVector2D& WidgetSize,
		const FIntPoint& ViewportSize,
		float DpiScale)
	{
		if (WidgetSize.X <= KINDA_SMALL_NUMBER || WidgetSize.Y <= KINDA_SMALL_NUMBER)
		{
			return FVector2D::ZeroVector;
		}

		const float SafeDpi = DpiScale > KINDA_SMALL_NUMBER ? DpiScale : 1.0f;
		return FVector2D(
			LocalPosition.X / WidgetSize.X * static_cast<float>(ViewportSize.X) / SafeDpi,
			LocalPosition.Y / WidgetSize.Y * static_cast<float>(ViewportSize.Y) / SafeDpi);
	}

	/** Physical viewport size that keeps CSS pixels equal to Slate DIPs. */
	static FIntPoint ComputeViewportSize(const FVector2D& WidgetSize, float DpiScale)
	{
		const float SafeDpi = DpiScale > KINDA_SMALL_NUMBER ? DpiScale : 1.0f;
		return FIntPoint(
			FMath::Max(1, FMath::RoundToInt(WidgetSize.X * SafeDpi)),
			FMath::Max(1, FMath::RoundToInt(WidgetSize.Y * SafeDpi)));
	}
};
