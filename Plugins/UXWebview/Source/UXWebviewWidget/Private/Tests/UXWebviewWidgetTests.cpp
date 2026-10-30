#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "UXWebviewCoordinateMapper.h"
#include "UXWebviewInputFilter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewWidgetCoordinatesTest, "UXWebview.Widget.Coordinates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewWidgetCoordinatesTest::RunTest(const FString& Parameters)
{
	const FVector2D WidgetSize(512.0f, 288.0f);

	// 100% DPI: physical viewport equals the widget size, so CSS equals local DIPs.
	const FIntPoint Viewport100 = FUXWebviewCoordinateMapper::ComputeViewportSize(WidgetSize, 1.0f);
	TestEqual(TEXT("viewport at 100% DPI"), Viewport100, FIntPoint(512, 288));
	const FVector2D Center = FUXWebviewCoordinateMapper::LocalToCss(
		FVector2D(256.0f, 144.0f), WidgetSize, Viewport100, 1.0f);
	TestTrue(TEXT("center maps to CSS center"), Center.Equals(FVector2D(256.0f, 144.0f), 0.01f));

	// 150% DPI: the viewport grows, so the same local click still lands on the same CSS point.
	const FIntPoint Viewport150 = FUXWebviewCoordinateMapper::ComputeViewportSize(WidgetSize, 1.5f);
	TestEqual(TEXT("viewport at 150% DPI"), Viewport150, FIntPoint(768, 432));
	const FVector2D Center150 = FUXWebviewCoordinateMapper::LocalToCss(
		FVector2D(256.0f, 144.0f), WidgetSize, Viewport150, 1.5f);
	TestTrue(TEXT("center maps to CSS center at 150%"), Center150.Equals(FVector2D(256.0f, 144.0f), 0.01f));

	const FVector2D Corner150 = FUXWebviewCoordinateMapper::LocalToCss(
		FVector2D(512.0f, 288.0f), WidgetSize, Viewport150, 1.5f);
	TestTrue(TEXT("corner maps to CSS corner at 150%"), Corner150.Equals(FVector2D(512.0f, 288.0f), 0.01f));

	// A fixed viewport stretches the page across the widget.
	const FVector2D Stretched = FUXWebviewCoordinateMapper::LocalToCss(
		FVector2D(256.0f, 144.0f), WidgetSize, FIntPoint(1024, 576), 1.0f);
	TestTrue(TEXT("fixed viewport scales the position"), Stretched.Equals(FVector2D(512.0f, 288.0f), 0.01f));

	// Degenerate input must not divide by zero.
	const FVector2D ZeroSize = FUXWebviewCoordinateMapper::LocalToCss(
		FVector2D(10.0f, 10.0f), FVector2D::ZeroVector, FIntPoint(512, 288), 1.0f);
	TestTrue(TEXT("zero widget size yields zero"), ZeroSize.IsZero());

	const FVector2D ZeroDpi = FUXWebviewCoordinateMapper::LocalToCss(
		FVector2D(256.0f, 144.0f), WidgetSize, Viewport100, 0.0f);
	TestTrue(TEXT("zero DPI falls back to 1"), ZeroDpi.Equals(FVector2D(256.0f, 144.0f), 0.01f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewWidgetInputFilterTest, "UXWebview.Widget.InputFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewWidgetInputFilterTest::RunTest(const FString& Parameters)
{
	FUXWebviewInputFilter Filter;

	TestTrue(TEXT("first focus is forwarded"), Filter.ShouldSendFocus(true));
	TestFalse(TEXT("repeated focus is dropped"), Filter.ShouldSendFocus(true));
	TestTrue(TEXT("focus lost is forwarded"), Filter.ShouldSendFocus(false));
	TestFalse(TEXT("repeated focus lost is dropped"), Filter.ShouldSendFocus(false));

	TestTrue(TEXT("first move is forwarded"), Filter.ShouldSendMouseMove(FVector2D(10.0f, 10.0f)));
	TestFalse(TEXT("same position is dropped"), Filter.ShouldSendMouseMove(FVector2D(10.0f, 10.0f)));
	TestTrue(TEXT("new position is forwarded"), Filter.ShouldSendMouseMove(FVector2D(11.0f, 10.0f)));
	Filter.InvalidateLastMove();
	TestTrue(TEXT("re-entry resends the same position"), Filter.ShouldSendMouseMove(FVector2D(11.0f, 10.0f)));

	TestTrue(TEXT("first press is forwarded"), Filter.ShouldSendMouseButton(true, 0));
	TestFalse(TEXT("duplicate press is dropped"), Filter.ShouldSendMouseButton(true, 0));
	TestTrue(TEXT("release is forwarded"), Filter.ShouldSendMouseButton(false, 0));
	TestFalse(TEXT("duplicate release is dropped"), Filter.ShouldSendMouseButton(false, 0));
	TestTrue(TEXT("right press is forwarded"), Filter.ShouldSendMouseButton(true, 1));

	// Reset clears focus, move and button state.
	Filter.Reset();
	TestTrue(TEXT("focus after reset is forwarded"), Filter.ShouldSendFocus(true));
	TestTrue(TEXT("move after reset is forwarded"), Filter.ShouldSendMouseMove(FVector2D(10.0f, 10.0f)));
	TestTrue(TEXT("press after reset is forwarded"), Filter.ShouldSendMouseButton(true, 1));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
