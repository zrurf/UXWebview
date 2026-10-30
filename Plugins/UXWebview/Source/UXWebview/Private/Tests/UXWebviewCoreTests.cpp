#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Frame/UXWebviewFrameGovernor.h"
#include "Frame/UXWebviewFrameSlotRing.h"
#include "Provider/UXWebviewProviderRegistry.h"
#include "UUXWebview.h"
#include "UUXWebviewSubsystem.h"

namespace
{
class FUXWebviewUnavailableFactory final : public IUXWebviewProviderFactory
{
public:
	virtual FUXWebviewProviderInfo GetInfo() const override
	{
		FUXWebviewProviderInfo Info;
		Info.Name = TEXT("Unavailable");
		Info.DisplayName = TEXT("Unavailable Provider");
		Info.bAvailable = false;
		Info.UnavailableReason = TEXT("runtime not installed");
		return Info;
	}

	virtual TUniquePtr<IUXWebviewProvider> Create() override
	{
		return nullptr;
	}
};

FUXWebviewCreateParams MakeNullParams()
{
	FUXWebviewCreateParams Params;
	Params.ProviderName = TEXT("Null");
	Params.ViewportSize = FIntPoint(128, 128);
	Params.TargetFrameRate = 30;
	return Params;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewRegistryResolveTest, "UXWebview.Core.Registry.Resolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewRegistryResolveTest::RunTest(const FString& Parameters)
{
	FUXWebviewProviderRegistry& Registry = FUXWebviewProviderRegistry::Get();
	FString ResolvedName;

	// Explicit names are matched case-insensitively.
	FUXWebviewCreateParams Explicit = MakeNullParams();
	Explicit.ProviderName = TEXT("null");
	const FUXWebviewResult ExplicitResult = Registry.Resolve(Explicit, ResolvedName);
	TestTrue(TEXT("null provider resolves"), ExplicitResult.bSuccess);
	TestEqual(TEXT("resolved name is canonical"), ResolvedName, FString(TEXT("Null")));

	// Unregistered explicit providers fail when fallback is disabled.
	FUXWebviewCreateParams Missing = MakeNullParams();
	Missing.ProviderName = TEXT("DoesNotExist");
	Missing.bAllowProviderFallback = false;
	const FUXWebviewResult MissingResult = Registry.Resolve(Missing, ResolvedName);
	TestFalse(TEXT("missing provider fails"), MissingResult.bSuccess);
	TestEqual(TEXT("missing provider reports NotFound"),
		static_cast<uint8>(MissingResult.Code), static_cast<uint8>(EUXWebviewResultCode::NotFound));

	// Unavailable providers fail with their reason when fallback is disabled.
	Registry.RegisterFactory(MakeShared<FUXWebviewUnavailableFactory>());
	FUXWebviewCreateParams Unavailable = MakeNullParams();
	Unavailable.ProviderName = TEXT("Unavailable");
	Unavailable.bAllowProviderFallback = false;
	const FUXWebviewResult UnavailableResult = Registry.Resolve(Unavailable, ResolvedName);
	TestFalse(TEXT("unavailable provider fails"), UnavailableResult.bSuccess);
	TestTrue(TEXT("reason is propagated"), UnavailableResult.Message.Contains(TEXT("runtime not installed")));
	Registry.UnregisterFactory(TEXT("Unavailable"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewNullLifecycleTest, "UXWebview.Core.Null.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewNullLifecycleTest::RunTest(const FString& Parameters)
{
	UUXWebview* Webview = NewObject<UUXWebview>(GetTransientPackage());
	FUXWebviewResult Result = Webview->Initialize(MakeNullParams());

	TestTrue(TEXT("null webview initializes"), Result.bSuccess);
	TestEqual(TEXT("state is Ready"),
		static_cast<uint8>(Webview->GetState()), static_cast<uint8>(EUXWebviewState::Ready));
	TestEqual(TEXT("provider name"), Webview->GetProviderName(), FString(TEXT("Null")));
	TestTrue(TEXT("texture is available"), Webview->GetTexture() != nullptr);

	FUXWebviewResult LoadResult = Webview->LoadURL(TEXT("about:blank"));
	TestTrue(TEXT("load succeeds"), LoadResult.bSuccess);

	Webview->Destroy();
	TestEqual(TEXT("state is Destroyed"),
		static_cast<uint8>(Webview->GetState()), static_cast<uint8>(EUXWebviewState::Destroyed));

	// Calls after destroy must fail with InvalidState, and destroy is idempotent.
	TestEqual(TEXT("load after destroy"),
		static_cast<uint8>(Webview->LoadURL(TEXT("https://example.com")).Code),
		static_cast<uint8>(EUXWebviewResultCode::InvalidState));
	TestEqual(TEXT("frame after destroy"),
		static_cast<uint8>(Webview->RequestFrame().Code),
		static_cast<uint8>(EUXWebviewResultCode::InvalidState));
	Webview->Destroy();
	TestTrue(TEXT("texture is cleared"), Webview->GetTexture() == nullptr);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewInitGuardsTest, "UXWebview.Core.Lifecycle.InitGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewInitGuardsTest::RunTest(const FString& Parameters)
{
	UUXWebview* Webview = NewObject<UUXWebview>(GetTransientPackage());
	FUXWebviewResult Result = Webview->Initialize(MakeNullParams());
	TestTrue(TEXT("first initialize succeeds"), Result.bSuccess);

	// A second initialize on the same instance is refused.
	FUXWebviewResult Second = Webview->Initialize(MakeNullParams());
	TestFalse(TEXT("second initialize fails"), Second.bSuccess);
	TestEqual(TEXT("second initialize reports InvalidState"),
		static_cast<uint8>(Second.Code), static_cast<uint8>(EUXWebviewResultCode::InvalidState));

	// Invalid viewport is rejected before touching the provider.
	FUXWebviewCreateParams Bad = MakeNullParams();
	Bad.ViewportSize = FIntPoint(0, 0);
	UUXWebview* InvalidViewport = NewObject<UUXWebview>(GetTransientPackage());
	FUXWebviewResult BadResult = InvalidViewport->Initialize(Bad);
	TestFalse(TEXT("zero viewport fails"), BadResult.bSuccess);
	TestEqual(TEXT("zero viewport reports InvalidArgs"),
		static_cast<uint8>(BadResult.Code), static_cast<uint8>(EUXWebviewResultCode::InvalidArgs));

	Webview->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewFrameSlotRingTest, "UXWebview.Frame.SlotRing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewFrameSlotRingTest::RunTest(const FString& Parameters)
{
	FUXWebviewFrameSlotRing Ring(3);
	TestEqual(TEXT("slots created"), Ring.NumSlots(), 3);
	TestEqual(TEXT("all slots start free"),
		Ring.CountInState(EUXWebviewFrameSlotState::Free), 3);
	TestEqual(TEXT("nothing displayed"), Ring.GetDisplaySlot(), INDEX_NONE);

	// Every free slot can be claimed once; the fourth claim fails.
	const int32 First = Ring.AcquireCopySlot();
	const int32 Second = Ring.AcquireCopySlot();
	const int32 Third = Ring.AcquireCopySlot();
	TestEqual(TEXT("first claim"), First, 0);
	TestEqual(TEXT("second claim"), Second, 1);
	TestEqual(TEXT("third claim"), Third, 2);
	TestEqual(TEXT("pool exhausted"), Ring.AcquireCopySlot(), INDEX_NONE);
	TestEqual(TEXT("no free slots left"),
		Ring.CountInState(EUXWebviewFrameSlotState::Free), 0);

	// Only a slot that is currently copying can complete or be cancelled.
	TestTrue(TEXT("complete first copy"), Ring.CompleteCopy(First));
	TestFalse(TEXT("double completion is rejected"), Ring.CompleteCopy(First));
	TestFalse(TEXT("unknown slot is rejected"), Ring.CompleteCopy(7));
	TestTrue(TEXT("cancel releases the copy slot"), Ring.CancelCopy(Third));
	TestFalse(TEXT("cancel on a free slot is rejected"), Ring.CancelCopy(Third));
	TestEqual(TEXT("cancelled slot can be claimed again"), Ring.AcquireCopySlot(), Third);
	TestEqual(TEXT("completion makes the slot ready"),
		static_cast<uint8>(Ring.GetState(First)),
		static_cast<uint8>(EUXWebviewFrameSlotState::Ready));

	// The newest finished frame becomes the display target.
	TestTrue(TEXT("complete second copy"), Ring.CompleteCopy(Second));
	TestEqual(TEXT("newest ready slot wins"), Ring.TakeNewestReadySlot(), Second);
	TestEqual(TEXT("display slot"), Ring.GetDisplaySlot(), Second);
	TestEqual(TEXT("displayed slot is in use"),
		static_cast<uint8>(Ring.GetState(Second)),
		static_cast<uint8>(EUXWebviewFrameSlotState::InUse));
	TestEqual(TEXT("older ready slot is recycled"),
		static_cast<uint8>(Ring.GetState(First)),
		static_cast<uint8>(EUXWebviewFrameSlotState::Free));

	// A later completion moves the display and frees the previous one.
	TestTrue(TEXT("complete third copy"), Ring.CompleteCopy(Third));
	TestEqual(TEXT("third copy takes the display"), Ring.TakeNewestReadySlot(), Third);
	TestEqual(TEXT("previous display is freed"),
		static_cast<uint8>(Ring.GetState(Second)),
		static_cast<uint8>(EUXWebviewFrameSlotState::Free));
	TestEqual(TEXT("in-use count"), Ring.CountInState(EUXWebviewFrameSlotState::InUse), 1);
	TestEqual(TEXT("no ready slots remain"),
		Ring.CountInState(EUXWebviewFrameSlotState::Ready), 0);

	// Nothing is ready until a copy finishes.
	TestEqual(TEXT("no ready copy yields no display"),
		Ring.TakeNewestReadySlot(), INDEX_NONE);

	// Reset drops everything, including the displayed slot.
	Ring.Reset(2);
	TestEqual(TEXT("resized slot count"), Ring.NumSlots(), 2);
	TestEqual(TEXT("resize frees every slot"),
		Ring.CountInState(EUXWebviewFrameSlotState::Free), 2);
	TestEqual(TEXT("resize clears the display"), Ring.GetDisplaySlot(), INDEX_NONE);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewFrameGovernorTest, "UXWebview.Frame.Governor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewFrameGovernorTest::RunTest(const FString& Parameters)
{
	FUXWebviewFrameGovernor Governor;
	TestEqual(TEXT("animated page runs at the active rate"),
		Governor.GetTargetFrameRate(), 60.0f);

	// A hidden page must not ask the host for frames.
	Governor.SetVisible(false);
	TestEqual(TEXT("hidden page targets zero"), Governor.GetTargetFrameRate(), 0.0f);
	Governor.SetVisible(true);

	// A page that declares no animation drops to the idle rate.
	Governor.SetAnimated(false);
	TestEqual(TEXT("static page targets the idle rate"),
		Governor.GetTargetFrameRate(), 15.0f);
	Governor.SetAnimated(true);

	// Input temporarily raises the rate to the maximum and then decays.
	Governor.NotifyInteraction();
	TestEqual(TEXT("interaction boosts to the maximum"),
		Governor.GetTargetFrameRate(), 120.0f);
	TestTrue(TEXT("interaction flag is set"), Governor.IsInteracting());
	for (int32 Step = 0; Step < 6; ++Step)
	{
		Governor.Update(0.1f);
	}
	TestFalse(TEXT("boost expires"), Governor.IsInteracting());
	TestEqual(TEXT("rate returns to active after the boost"),
		Governor.GetTargetFrameRate(), 60.0f);

	// Interaction must not override an invisible page.
	Governor.SetVisible(false);
	Governor.NotifyInteraction();
	TestEqual(TEXT("hidden page stays at zero during input"),
		Governor.GetTargetFrameRate(), 0.0f);
	TestFalse(TEXT("hide cancels the boost"), Governor.IsInteracting());

	// The configured maximum clamps every tier.
	FUXWebviewGovernorConfig Config;
	Config.MaxFrameRate = 60.0f;
	Config.ActiveFrameRate = 90.0f;
	Governor.SetConfig(Config);
	Governor.SetVisible(true);
	TestEqual(TEXT("active rate is clamped"), Governor.GetTargetFrameRate(), 60.0f);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
