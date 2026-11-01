# API reference

This page is the contract between the plugin and game code. Everything listed
here is usable from C++; the Blueprint column marks what is also callable from
Blueprint graphs.

## Entry points

| Type | Role |
| --- | --- |
| `UUXWebviewSubsystem` | Game instance subsystem that owns every view and selects providers |
| `UUXWebview` | One web content instance: navigation, input, frames, script bridge |
| `UUXWebviewWidget` | UMG widget that draws `UUXWebview` and routes mouse/keyboard/IME |
| `FUXWebviewCreateParams` | Per-view parameters |
| `FUXWebviewResult` | Success flag, error code and message returned by every call |
| `FUXWebviewCapabilities` | Fixed provider abilities, so gameplay code can branch on degradation |

Include `UUXWebviewSubsystem.h`, `UUXWebview.h` and `UUXWebviewWidget.h`, and add
`UXWebview`, `UXWebviewWidget` to the module dependencies of your game module.

## Creating and destroying views

```cpp
UGameInstance* GameInstance = GetGameInstance();
UUXWebviewSubsystem* Subsystem = GameInstance->GetSubsystem<UUXWebviewSubsystem>();

FUXWebviewCreateParams Params;
Params.ProviderName = TEXT("");                 // empty: resolved from settings
Params.ViewportSize = FIntPoint(1280, 720);
Params.TargetFrameRate = 60;
Params.InitialUrl = TEXT("https://example.com/");

FUXWebviewResult Result;
UUXWebview* View = Subsystem->CreateWebview(Params, Result);
if (!View)
{
    UE_LOG(LogTemp, Warning, TEXT("webview failed: %s"), *Result.Message);
}
```

`CreateWebview` returns `nullptr` and fills `OutResult` when the provider cannot
be resolved, is unavailable, or rejects initialization. The subsystem owns the
view; call `DestroyWebview(View)` to release it early, or let the game instance
shut down. Destroying the owning game instance destroys every view.

Blueprint: call **Get Game Instance Subsystem**, cast the result to
`UXWebviewSubsystem`, then use **Create Webview**. The result struct is exposed
as a split pin; check `Success` before using the returned view.

### `UUXWebviewSubsystem`

| Function | Blueprint | Notes |
| --- | --- | --- |
| `CreateWebview(Params, OutResult)` | Callable | Returns null on failure |
| `DestroyWebview(Webview)` | Callable | Safe to call with a null or stale pointer |
| `GetWebviews()` | Pure | Live views owned by this game instance |
| `GetProviders()` | Pure | Registered providers with availability and reasons |

`GetProviders()` is the first stop when a view fails to create: each entry
carries `bAvailable` and `UnavailableReason`, for example a missing native host
or an absent WebView2 runtime.

## `UUXWebview`

All functions must be called on the game thread. They return a `FUXWebviewResult`
instead of throwing. A view is usable after `OnReady`; calls made before that
return `NotReady`.

| Function | Blueprint | Notes |
| --- | --- | --- |
| `GetState()` | Pure | `EUXWebviewState` |
| `GetProviderName()` | Pure | Canonical provider name, e.g. `CEF` |
| `GetCapabilities()` | Pure | See capability flags below |
| `GetTexture()` | Pure | Frame texture; null before the first frame |
| `GetFrameSize()` | Pure | Physical pixels of the last frame |
| `GetFrameCount()` | Pure | Frames received since the view became ready |
| `IsOverlayMode()` | Pure | True when the provider composites through a native window |
| `SetCompositionOverlay(bComposition)` | Callable | WebView2 overlay views only: moves the page to a composition controller (transparent, no typed input) or back. Captures scroll position, form values and the caret before the reload and reapplies them; the swap completes on the next tick |
| `IsCompositionOverlay()` | Pure | True while the overlay renders through a composition controller |
| `EnsureKeyboardInput()` | Callable | Composition overlays only: switches back through `SetCompositionOverlay(false)` so the page can type; a no-op on other views |
| `LoadURL(Url)` | Callable | Absolute URL |
| `LoadHTML(Html, BaseUrl)` | Callable | `BaseUrl` resolves relative links and bridge origin |
| `Reload()` | Callable | |
| `StopLoading()` | Callable | |
| `SetViewport(Size, DpiScale)` | Callable | Physical pixel size; zero DPI keeps the current scale |
| `SetOverlayBounds(Position, Size, DpiScale)` | Callable | Overlay views only, parent client pixels |
| `SetVisible(bVisible)` | Callable | Texture mode pauses frame requests when hidden |
| `SetFrameRate(FramesPerSecond)` | Callable | Clamped to the provider maximum |
| `SetZoom(ZoomFactor)` | Callable | Page zoom, not widget scale |
| `SetUserAgent(UserAgent)` | Callable | Applies to subsequent navigations |
| `ExecuteScriptAsync(Script)` | Callable | Returns a request id, or 0 on failure |
| `ExecuteScript(Script, Callback)` | C++ only | Callback receives the script result |
| `EmitToWeb(EventName, PayloadJson)` | Callable | Dispatches a `uxwv` CustomEvent in the page |
| `SendInput(Event)` | Callable | Mouse, key and IME events in view CSS pixels |
| `SetFocus(bFocused)` | Callable | Routes keyboard and IME to the page |
| `RespondToScriptMessage(RequestId, bSuccess, ResultJson)` | Callable | Answers `OnScriptMessage` |
| `OpenDevTools()` | Callable | Fails with `NotSupported` when unavailable |
| `RequestFrame()` | C++ only | Manual frame request; usually driven by the widget |

### Delegates

| Delegate | Blueprint | Parameters |
| --- | --- | --- |
| `OnReady` | Assignable | none |
| `OnLoadCompleted` | Assignable | `Url` |
| `OnCrashed` | Assignable | `Reason` |
| `OnScriptMessage` | Assignable | `Name`, `PayloadJson`, `RequestId` |
| `OnKeyboardRequest` | Assignable | `PayloadJson` |
| `OnScriptResult` | Assignable | `RequestId`, `bSuccess`, `ResultJson` |

`OnScriptResult` only fires for scripts started with `ExecuteScriptAsync`;
`RequestId` is 0 when the call could not be started at all. The C++ overload
with a callback is independent and does not broadcast. A script that returns a
promise is awaited: the result is the JSON text of the resolved value, and a
rejected promise reports `bSuccess = false` with the rejection message.

### Capability flags

`FUXWebviewCapabilities` is fixed for the lifetime of a view:

| Flag | Meaning |
| --- | --- |
| `bGpuSharedTexture` | Frames arrive as GPU shared textures without a CPU copy |
| `bCpuPixelBuffer` | Frames are uploaded from CPU pixels |
| `bOverlayMode` | Provider can composite through a native child window |
| `bTransparency` | Page background can be transparent; WebView2 reports it while a composition controller hosts the view, so the flag follows `SetCompositionOverlay` |
| `bExternalBeginFrame` | The plugin paces page frames explicitly |
| `bDevTools` | `OpenDevTools` is available |
| `bOfflineCache` | Provider keeps a persistent page cache |
| `MaxFrameRate` | Upper bound accepted by `SetFrameRate` |
| `MaxViewportSize` | Largest accepted viewport; zero means unconstrained |

Use these flags instead of provider names when gameplay behaviour depends on a
feature. `GetProviders()` covers availability; capabilities cover what a view
can do once created.

### Result codes

| Code | Meaning |
| --- | --- |
| `Ok` | Call accepted |
| `InvalidState` | View destroyed, already initialized or otherwise closed |
| `NotReady` | Provider still initializing or view is suspended |
| `NotFound` | Provider name is not registered |
| `NotSupported` | Provider or view mode does not implement the call |
| `HostCrashed` | Browser process is gone; recovery may be in progress |
| `Timeout` | The provider did not answer in time |
| `InvalidArgs` | Empty URL, non-positive size and similar input errors |
| `BridgeDenied` | Script bridge origin is not allowlisted |
| `Unknown` | Unclassified provider failure; read `Message` |

## `UUXWebviewWidget`

| Member | Blueprint | Notes |
| --- | --- | --- |
| `SetWebView(View)` | Callable | Binds the view; does not take ownership |
| `GetWebView()` | Pure | Currently bound view |
| `SetDpiScale(Scale)` | Callable | `0` follows the layout scale |
| `SetInteractable(bInteractable)` | Callable | Disables hit tests and focus when false |
| `WebView` | Editable | Default binding for widgets placed in a designer |
| `DpiScale` | Editable | `0` - `4`; `0` follows the layout scale |
| `bInteractable` | Editable | Initial interaction state |

Add the widget to a viewport or panel and give it a size. The widget reports its
layout size to the view, so `SetViewport` normally does not need to be called by
game code. Overlay views ignore the widget's painted texture and instead track
the widget rectangle every tick.

## Input

`FUXWebviewInputEvent` uses view CSS pixels (`Position`) and virtual key codes
(`KeyCode`, Windows `VK_*` on Windows). `Button` is 0 left, 1 right, 2 middle.

| `EUXWebviewInputType` | Payload |
| --- | --- |
| `MouseMove` | `Position` |
| `MouseDown` / `MouseUp` | `Position`, `Button` |
| `MouseWheel` | `Position`, `WheelDelta` |
| `KeyDown` / `KeyUp` | `KeyCode`, modifier flags |
| `Char` | `Text` |
| `ImeComposition` | `Text`; replaces the previous composition |
| `ImeCommit` | `Text`; inserts the final composition |
| `ImeCancel` | Discards the current composition |

Dragging needs intermediate `MouseMove` events between `MouseDown` and
`MouseUp`; a press and release without movement is treated as a click.

WebView2 composition overlay views accept mouse events only: `KeyDown`, `KeyUp`,
`Char` and IME events fail with `NotSupported`, and `bTransparency` is the flag
that tells game code whether the provider can show a transparent page background.
Call `SetCompositionOverlay(false)` when the page needs typed text; the view
reloads and gains the regular controller's keyboard and IME path. A page can ask
for that itself: the reserved bridge name `request-keyboard` broadcasts
`OnKeyboardRequest` and is already answered, so game code only has to decide
whether to call `EnsureKeyboardInput()`.

Every provider forwards `SendInput`, overlay views included. In a native overlay
the browser window consumes real input straight from the OS, and a `SendInput`
call travels a separate path (window messages for WebView2, IPC for CEF), so one
physical event never reaches the page twice. Overlay views of both providers
therefore accept synthetic mouse and key events, and CEF overlays also accept
IME events.

## Script bridge

Page side and failure modes are documented in [script-bridge.md](script-bridge.md).
From game code the contract is:

1. Bind `OnScriptMessage`.
2. Answer each call exactly once with `RespondToScriptMessage(RequestId, ...)`.
3. Pass `ResultJson` as valid JSON text; it becomes the resolved value of the
   page promise.
4. Unanswered calls fail after the provider timeout.

The single reserved name is `request-keyboard`: it never reaches
`OnScriptMessage`, it broadcasts `OnKeyboardRequest` and is answered with `{}`
right after the delegates run, so a page can call it without any handler.

If no handler is bound the plugin answers immediately with
`no script message handler`, so the page never waits for a reply that cannot
arrive.

## Provider extension

`IUXWebviewProvider`, `IUXWebviewProviderFactory` and
`FUXWebviewProviderRegistry` are public C++ interfaces for new browser backends.
See [architecture.md](architecture.md) for the contract and the registration
steps.
