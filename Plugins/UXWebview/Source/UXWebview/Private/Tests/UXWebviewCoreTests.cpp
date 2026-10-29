#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

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

#endif // WITH_AUTOMATION_TESTS
