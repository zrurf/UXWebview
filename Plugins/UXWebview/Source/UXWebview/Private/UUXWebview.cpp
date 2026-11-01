#include "UUXWebview.h"

#include "Engine/Texture2D.h"
#include "Provider/UXWebviewProviderRegistry.h"
#include "UXWebviewLog.h"

FUXWebviewResult UUXWebview::Initialize(const FUXWebviewCreateParams& Params)
{
	if (State != EUXWebviewState::Created)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidState,
			TEXT("webview has already been initialized"));
	}
	if (Params.ViewportSize.X <= 0 || Params.ViewportSize.Y <= 0)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("viewport size must be positive"));
	}

	FUXWebviewProviderRegistry& Registry = FUXWebviewProviderRegistry::Get();

	FString ResolvedName;
	const FUXWebviewResult ResolveResult = Registry.Resolve(Params, ResolvedName);
	if (!ResolveResult.bSuccess)
	{
		State = EUXWebviewState::Destroyed;
		UE_LOG(LogUXWebview, Warning, TEXT("provider resolution failed: %s"), *ResolveResult.Message);
		return ResolveResult;
	}

	const FUXWebviewResult CreateResult = Registry.CreateProvider(ResolvedName, Provider);
	if (!CreateResult.bSuccess)
	{
		State = EUXWebviewState::Destroyed;
		UE_LOG(LogUXWebview, Warning, TEXT("provider creation failed: %s"), *CreateResult.Message);
		return CreateResult;
	}

	ProviderName = ResolvedName;
	Capabilities = Provider->GetCapabilities();
	State = EUXWebviewState::Initializing;

	FUXWebviewInitParams InitParams;
	InitParams.ViewportSize = Params.ViewportSize;
	InitParams.DpiScale = Params.DpiScale;
	InitParams.TargetFrameRate = Params.TargetFrameRate;
	InitParams.bTransparent = Params.bTransparent;
	InitParams.bOverlay = Params.DisplayMode == EUXWebviewDisplayMode::Overlay;
	InitParams.InitialUrl = Params.InitialUrl;

	const FUXWebviewResult InitResult = Provider->Initialize(InitParams, this);
	if (!InitResult.bSuccess)
	{
		UE_LOG(LogUXWebview, Warning, TEXT("provider '%s' failed to initialize: %s"),
			*ResolvedName, *InitResult.Message);
		ShutdownProvider();
		State = EUXWebviewState::Destroyed;
		return InitResult;
	}

	// Providers may report readiness synchronously; keep Initializing otherwise.
	if (State == EUXWebviewState::Initializing)
	{
		State = EUXWebviewState::Ready;
	}
	UE_LOG(LogUXWebview, Log, TEXT("webview initialized with provider '%s'"), *ProviderName);
	return FUXWebviewResult::Ok();
}

void UUXWebview::Destroy()
{
	if (State == EUXWebviewState::Destroyed)
	{
		return;
	}
	ShutdownProvider();
	CachedTexture = nullptr;
	State = EUXWebviewState::Destroyed;
}

void UUXWebview::ShutdownProvider()
{
	FailPendingScriptReplies(TEXT("webview shut down"));
	if (Provider)
	{
		Provider->Shutdown();
		Provider.Reset();
	}
}

void UUXWebview::BeginDestroy()
{
	ShutdownProvider();
	Super::BeginDestroy();
}

FUXWebviewResult UUXWebview::GuardAlive() const
{
	if (State == EUXWebviewState::Destroyed || !Provider)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidState,
			TEXT("webview is destroyed"));
	}
	return FUXWebviewResult::Ok();
}

FUXWebviewResult UUXWebview::GuardReady() const
{
	const FUXWebviewResult Alive = GuardAlive();
	if (!Alive.bSuccess)
	{
		return Alive;
	}
	if (State != EUXWebviewState::Ready && State != EUXWebviewState::Navigating)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady,
			TEXT("webview is not ready"));
	}
	return FUXWebviewResult::Ok();
}

UTexture2D* UUXWebview::GetTexture() const
{
	if (State == EUXWebviewState::Destroyed || !Provider)
	{
		return nullptr;
	}
	return CachedTexture ? CachedTexture.Get() : Provider->GetTexture();
}

FUXWebviewResult UUXWebview::LoadURL(const FString& Url)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	State = EUXWebviewState::Navigating;
	return Provider->LoadURL(Url);
}

FUXWebviewResult UUXWebview::LoadHTML(const FString& Html, const FString& BaseUrl)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	State = EUXWebviewState::Navigating;
	return Provider->LoadHTML(Html, BaseUrl);
}

FUXWebviewResult UUXWebview::Reload()
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	State = EUXWebviewState::Navigating;
	return Provider->Reload();
}

FUXWebviewResult UUXWebview::StopLoading()
{
	const FUXWebviewResult Ready = GuardReady();
	return Ready.bSuccess ? Provider->StopLoading() : Ready;
}

FUXWebviewResult UUXWebview::SetViewport(const FIntPoint& Size, float DpiScale)
{
	const FUXWebviewResult Alive = GuardAlive();
	if (!Alive.bSuccess)
	{
		return Alive;
	}
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("viewport size must be positive"));
	}
	return Provider->SetViewport(Size, DpiScale);
}

FUXWebviewResult UUXWebview::SetOverlayBounds(const FIntPoint& Position, const FIntPoint& Size,
	float DpiScale)
{
	const FUXWebviewResult Alive = GuardAlive();
	if (!Alive.bSuccess)
	{
		return Alive;
	}
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("overlay size must be positive"));
	}
	return Provider->SetOverlayBounds(Position, Size, DpiScale);
}

bool UUXWebview::IsOverlayMode() const
{
	return Provider.IsValid() && Provider->IsOverlayMode();
}

FUXWebviewResult UUXWebview::SetCompositionOverlay(bool bComposition)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}

	const FUXWebviewResult Result = Provider->SetCompositionOverlay(bComposition);
	// The controller is rebuilt on the provider's next tick, so the capabilities
	// refresh happens when the switched view finishes its reload.
	return Result;
}

bool UUXWebview::IsCompositionOverlay() const
{
	return Provider.IsValid() && Provider->IsCompositionOverlay();
}

FUXWebviewResult UUXWebview::EnsureKeyboardInput()
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	// Regular controllers already own real keyboard input, so only a composition
	// overlay has to trade transparency back for typing.
	if (!IsCompositionOverlay())
	{
		return FUXWebviewResult::Ok();
	}
	return SetCompositionOverlay(false);
}

FUXWebviewResult UUXWebview::SetVisible(bool bVisible)
{
	const FUXWebviewResult Alive = GuardAlive();
	if (!Alive.bSuccess)
	{
		return Alive;
	}
	State = bVisible ? EUXWebviewState::Ready : EUXWebviewState::Suspended;
	return Provider->SetVisible(bVisible);
}

FUXWebviewResult UUXWebview::SetFrameRate(int32 FramesPerSecond)
{
	const FUXWebviewResult Alive = GuardAlive();
	return Alive.bSuccess ? Provider->SetFrameRate(FramesPerSecond) : Alive;
}

FUXWebviewResult UUXWebview::SetZoom(float ZoomFactor)
{
	const FUXWebviewResult Alive = GuardAlive();
	return Alive.bSuccess ? Provider->SetZoom(ZoomFactor) : Alive;
}

FUXWebviewResult UUXWebview::SetUserAgent(const FString& UserAgent)
{
	const FUXWebviewResult Alive = GuardAlive();
	return Alive.bSuccess ? Provider->SetUserAgent(UserAgent) : Alive;
}

FUXWebviewResult UUXWebview::ExecuteScript(const FString& Script, FUXWebviewScriptCallback OnResult)
{
	const FUXWebviewResult Ready = GuardReady();
	return Ready.bSuccess ? Provider->ExecuteScript(Script, MoveTemp(OnResult)) : Ready;
}

int32 UUXWebview::ExecuteScriptAsync(const FString& Script)
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		OnScriptResult.Broadcast(0, false, Ready.Message);
		return 0;
	}

	const int32 RequestId = NextScriptRequestId++;
	TWeakObjectPtr<UUXWebview> WeakThis(this);
	const FUXWebviewResult Result = Provider->ExecuteScript(Script,
		[WeakThis, RequestId](bool bSuccess, const FString& ResultJson)
		{
			if (UUXWebview* View = WeakThis.Get())
			{
				View->OnScriptResult.Broadcast(RequestId, bSuccess, ResultJson);
			}
		});
	if (!Result.bSuccess)
	{
		OnScriptResult.Broadcast(0, false, Result.Message);
		return 0;
	}
	return RequestId;
}

FUXWebviewResult UUXWebview::EmitToWeb(const FString& EventName, const FString& PayloadJson)
{
	const FUXWebviewResult Ready = GuardReady();
	return Ready.bSuccess ? Provider->EmitToWeb(EventName, PayloadJson) : Ready;
}

bool UUXWebview::RespondToScriptMessage(int32 RequestId, bool bSuccess, const FString& ResultJson)
{
	FUXWebviewBridgeReply* Found = PendingScriptReplies.Find(RequestId);
	if (!Found)
	{
		// Already answered, timed out, or never issued by this view.
		return false;
	}

	FUXWebviewBridgeReply Reply = MoveTemp(*Found);
	PendingScriptReplies.Remove(RequestId);
	if (Reply)
	{
		Reply(bSuccess, ResultJson);
	}
	return true;
}

FUXWebviewResult UUXWebview::SendInput(const FUXWebviewInputEvent& Event)
{
	const FUXWebviewResult Ready = GuardReady();
	return Ready.bSuccess ? Provider->SendInput(Event) : Ready;
}

FUXWebviewResult UUXWebview::SetFocus(bool bFocused)
{
	const FUXWebviewResult Ready = GuardReady();
	return Ready.bSuccess ? Provider->SetFocus(bFocused) : Ready;
}

FUXWebviewResult UUXWebview::RequestFrame()
{
	const FUXWebviewResult Ready = GuardReady();
	return Ready.bSuccess ? Provider->RequestFrame() : Ready;
}

FUXWebviewResult UUXWebview::OpenDevTools()
{
	const FUXWebviewResult Ready = GuardReady();
	if (!Ready.bSuccess)
	{
		return Ready;
	}
	if (!Capabilities.bDevTools)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotSupported,
			TEXT("provider does not support devtools"));
	}
	return Provider->OpenDevTools();
}

void UUXWebview::OnViewReady()
{
	if (State == EUXWebviewState::Destroyed)
	{
		return;
	}
	State = EUXWebviewState::Ready;
	OnReady.Broadcast();
}

void UUXWebview::OnViewLoadCompleted(const FString& Url)
{
	if (State == EUXWebviewState::Destroyed)
	{
		return;
	}
	State = EUXWebviewState::Ready;
	// Transparency depends on the live controller, and a composition switch only
	// reports its new mode once the reloaded page finishes navigation.
	if (Provider.IsValid())
	{
		Capabilities = Provider->GetCapabilities();
	}
	OnLoadCompleted.Broadcast(Url);
}

void UUXWebview::OnViewFrame(int32 Width, int32 Height)
{
	if (State == EUXWebviewState::Destroyed || !Provider)
	{
		return;
	}
	FrameWidth = Width;
	FrameHeight = Height;
	++FrameCount;
	CachedTexture = Provider->GetTexture();
}

void UUXWebview::OnViewCrash(const FString& Reason)
{
	if (State == EUXWebviewState::Destroyed)
	{
		return;
	}
	State = EUXWebviewState::Crashed;
	UE_LOG(LogUXWebview, Warning, TEXT("provider '%s' crashed: %s"), *ProviderName, *Reason);
	OnCrashed.Broadcast(Reason);
}

void UUXWebview::OnViewScriptMessage(const FString& Name, const FString& PayloadJson,
	int32 RequestId, FUXWebviewBridgeReply Reply)
{
	if (State == EUXWebviewState::Destroyed)
	{
		if (Reply)
		{
			Reply(false, TEXT("webview is destroyed"));
		}
		return;
	}

	// Reserved bridge name: the page needs typed input, which a composition
	// controller cannot deliver. The request is answered here so it never waits
	// on gameplay, and gameplay decides how to rehost the view.
	if (Name == TEXT("request-keyboard"))
	{
		OnKeyboardRequest.Broadcast(PayloadJson);
		if (Reply)
		{
			Reply(true, TEXT("{}"));
		}
		return;
	}

	PendingScriptReplies.Add(RequestId, MoveTemp(Reply));

	// Nobody listening means the page should fail fast instead of waiting for
	// the provider timeout.
	if (!OnScriptMessage.IsBound())
	{
		RespondToScriptMessage(RequestId, false, TEXT("no script message handler"));
		return;
	}

	OnScriptMessage.Broadcast(Name, PayloadJson, RequestId);
}

void UUXWebview::FailPendingScriptReplies(const FString& Reason)
{
	for (TPair<int32, FUXWebviewBridgeReply>& Pair : PendingScriptReplies)
	{
		if (Pair.Value)
		{
			Pair.Value(false, Reason);
		}
	}
	PendingScriptReplies.Reset();
}
