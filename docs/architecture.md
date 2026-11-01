# Architecture

This page explains how the plugin is put together, what each module owns, and
how a browser backend plugs into it. It is written for contributors who need to
change the plugin without reading an implementation log.

## Process model

The plugin supports two kinds of backend:

- **Out-of-process (CEF).** A separate `UXWebviewHost.exe` owns the browser and
  exchanges frames and commands with the game over IPC. A renderer crash cannot
  take the game down.
- **In-process (WebView2).** The Edge WebView2 runtime runs inside the game
  process on the game thread's STA apartment. Frames are captured as PNG images
  and uploaded to a texture; overlay views host a native child window.

Both backends sit behind the same provider interface, so game code does not
branch on which one is active. Capability flags describe what each backend can
do, and `GetProviders()` reports availability before a view is created.

## Module map

| Module | Type | Responsibility |
| --- | --- | --- |
| `UXWebview` | Runtime | Provider interface and registry, settings, view object (`UUXWebview`), game instance subsystem, Null provider, diagnostics and core tests |
| `UXWebviewWidget` | Runtime | `UUXWebviewWidget` (UMG) and `SUXWebviewWidget` (Slate), coordinate mapping, input routing, IME context |
| `UXWebviewCEF` | Runtime | CEF provider, host client, IPC protocol, shared-texture frame pool, CEF tests |
| `UXWebviewWebView2` | Runtime | WebView2 provider and module registration; compiled only on Win64 |
| `UXWebviewEditor` | Editor | Project settings customization |
| `native/host` | Standalone C++ | `UXWebviewHost.exe`: CEF browser process, IPC server, shared frames, overlay windows |
| `native/include` | Shared C++ | `uxwebview_ipc.h`, `uxwebview_frame.h`; used by the host and by `UXWebviewCEF` |
| `Samples/UXWebviewSample` | Sample project | Console commands that exercise every backend and display mode |

Dependency direction: `UXWebviewWidget` depends on `UXWebview`; backend modules
depend on `UXWebview` and register themselves; `UXWebviewEditor` depends on
`UXWebview`. Backend modules never depend on each other.

## View lifecycle

1. `UUXWebviewSubsystem::CreateWebview` builds `FUXWebviewCreateParams`.
2. `FUXWebviewProviderRegistry::Resolve` chooses a provider: explicit name,
   `PlatformDefaultProvider`, built-in default (`CEF`), then `FallbackProviders`
   when `bAllowProviderFallback` is true.
3. The registry creates the provider through its factory and `UUXWebview` calls
   `Initialize(params, client)`.
4. The provider reports readiness with `IUXWebviewViewClient::OnViewReady`, then
   navigation results with `OnViewLoadCompleted` and frames with `OnViewFrame`.
5. `DestroyWebview` (or game instance shutdown) calls `Shutdown`. Providers
   must tolerate calls made after `Shutdown` with `InvalidState`.

Provider calls run on the game thread. Backends move blocking work to their own
threads (CEF host process) or to asynchronous COM callbacks (WebView2) and
marshal results back to the game thread.

## Provider contract

`IUXWebviewProvider` (see `Plugins/UXWebview/Source/UXWebview/Public/Provider/IUXWebviewProvider.h`)
is the single interface a backend implements:

| Group | Methods |
| --- | --- |
| Identity | `GetInfo`, `GetCapabilities` |
| Lifecycle | `Initialize`, `Shutdown` |
| Navigation | `LoadURL`, `LoadHTML`, `Reload`, `StopLoading` |
| Layout | `SetViewport`, `SetOverlayBounds`, `IsOverlayMode`, `SetVisible`, `SetFrameRate`, `SetZoom`, `SetUserAgent` |
| Scripting | `ExecuteScript`, `EmitToWeb`, plus `OnViewScriptMessage` on the client |
| Input | `SendInput`, `SetFocus`, `RequestFrame` |
| Output | `GetTexture`, `OpenDevTools` |

Results use `FUXWebviewResult`; `Message` must explain failures in a form a user
can act on (missing runtime, missing host, unsupported mode).

### Callbacks and threading

`IUXWebviewViewClient` receives `OnViewReady`, `OnViewLoadCompleted`,
`OnViewFrame`, `OnViewCrash` and `OnViewScriptMessage`. All callbacks are on the
game thread and the client outlives the provider. `OnViewScriptMessage` carries
a `FUXWebviewBridgeReply`; the provider must call it exactly once, or let its
timeout reject the page promise.

## Capability matrix

Flags are fixed for the lifetime of a view. Game code should branch on these
flags, not on provider names.

| Flag | Null | CEF | WebView2 |
| --- | --- | --- | --- |
| `bGpuSharedTexture` | false | true | false |
| `bCpuPixelBuffer` | true | false | true |
| `bOverlayMode` | false | true | true |
| `bTransparency` | false | false | follows `bCompositionOverlay` |
| `bExternalBeginFrame` | false | true | false |
| `bDevTools` | false | follows `bEnableDevTools` | follows `bEnableDevTools` |
| `bOfflineCache` | false | true | true |
| `MaxFrameRate` | 60 | 240 | 30 |
| `MaxViewportSize` | 512x512 | 2048x2048 | 4096x4096 |
| Process model | in-process | separate host | in-process |

## Frame pipeline

**CEF.** The host renders off-screen into a pool of shared D3D textures. The
provider either copies a frame into a pool texture or, with `bDirectDisplay`,
points the display texture at the newest shared buffer. Frames reach Slate
without a game-thread copy.

**WebView2 texture mode.** Every presented frame is requested with
`ICoreWebView2::CapturePreview` (PNG), decoded with `IImageWrapperModule`, and
uploaded with `UTexture2D::UpdateTextureRegions`. This is a CPU readback, so the
provider caps `MaxFrameRate` at 30 and reports `bCpuPixelBuffer`. The browser
lives in a host window positioned off-screen (see "Input and IME" for why it
stays shown). Overlay mode bypasses this path entirely: the page is a native
child window and Chromium composites it.

**WebView2 composition overlay.** When `bCompositionOverlay` is enabled, overlay
views are created through `ICoreWebView2Environment3::CreateCoreWebView2CompositionController`.
The provider owns a D3D11 device, creates a DirectComposition target for the host
window, and passes the root visual to `put_RootVisualTarget`; Chromium then mixes
the page into the window. No capture or upload happens, so the view keeps the full
frame rate, `bTransparency` is reported and `bTransparent` requests a transparent
page background. Runtimes without composition support log a warning and fall back
to the regular windowed controller on the same host window.

## Input and IME

`SUXWebviewWidget` converts Slate-local positions to page CSS pixels with
`FUXWebviewCoordinateMapper`, then forwards `FUXWebviewInputEvent` values to the
provider. CEF sends them to the host over IPC.

WebView2 texture mode delivers input as Win32 window messages. Chromium nests
two windows named `Chrome_WidgetWin_*` under the host window: the outer one is a
wrapper that ignores input, and the inner one (whose parent is the outer one)
converts mouse, wheel and keyboard messages into web events. The provider
therefore targets the inner window and delivers messages synchronously
(`SendMessageW`) from the game thread, which also owns those windows. Keyboard
messages only reach the page while the browser window owns the OS keyboard
focus, so the provider moves OS focus to that window before key events. Because
the window never receives a native IME, composition is forwarded to page JavaScript
(`window.__uxwvIme`) and committed into the focused editable element. Overlay
mode input is handled by the native browser child window.

The off-screen host window stays shown (`SW_SHOWNOACTIVATE` at negative
coordinates) while the view is visible: Chromium drops wheel input sent to a
hidden window but not to a visible off-screen one.

**WebView2 composition overlay.** No browser child window exists to consume
input, so the provider subclasses the host window procedure: mouse move, button,
wheel and leave messages become `SendMouseInput` calls, and the widget's
`FUXWebviewInputEvent` values take the same route. Wheel deltas are passed as the
raw signed value because `SendMouseInput` ignores the `MAKELPARAM` encoding. Page
cursors are read with `add_CursorChanged` and applied on `WM_SETCURSOR`. A
composition controller has no key or IME injection API, so `SendInput` reports
`NotSupported` for `KeyDown`, `KeyUp`, `Char` and every IME event: composition
overlay views show and click, but cannot receive typed text. Enable this mode only
for views that do not need a keyboard.

The widget follows the view rectangle for overlay views, so moving or resizing
the widget moves the native window in client pixels.

## Script bridge

Both backends implement `window.uxwv` from `docs/script-bridge.md`:

- CEF: the page calls `uxwvQuery`; the host forwards the message to the plugin.
- WebView2: the page calls `window.chrome.webview.postMessage`; the provider
  parses `{uxwv:1, kind, id, name, payloadJson}`.
- `window.uxwv` is installed with `AddScriptToExecuteOnDocumentCreated`, whose
  registration completes asynchronously. WebView2 navigates only after that
  callback fires, so the first document always has the bridge.
- The plugin assigns a game-side `RequestId`, invokes `OnViewScriptMessage`, and
  sends the reply back to the page.
- Origin policy is identical: local schemes pass, remote origins must appear in
  `BridgeAllowedOrigins`. Requests time out after 30 seconds.

Scripts started from game code also behave alike: both backends run
`ExecuteScript` inside a wrapper that awaits a returned promise and reports
`{ok, value}` back to the plugin, so `ExecuteScriptAsync` resolves with the
awaited value and reports rejections as failures.

## Adding a backend

1. Create a module that depends on `UXWebview` and put its public headers under
   `Public`, private sources under `Private`.
2. Implement `IUXWebviewProvider` and an `IUXWebviewProviderFactory` that
   reports availability through `FUXWebviewProviderInfo::bAvailable` and
   `UnavailableReason`.
3. Register the factory in `StartupModule` and unregister it in
   `ShutdownModule`; keep the registration idempotent so hot reload is safe.
4. Add the module to `UXWebview.uplugin` with a `PlatformAllowList` when it is
   platform-specific.
5. Implement `SetOverlayBounds` only when `bOverlayMode` is true; otherwise
   return `NotSupported`.
6. Report honest capabilities. A backend that decodes CPU pixels must set
   `bCpuPixelBuffer` and cap `MaxFrameRate`.
7. Add automation tests under `Private/Tests` and verify with the sample
   commands listed in `docs/getting-started.md`.

## Repository conventions

- Source comments, log messages and documentation are written in English.
- Public API types live in `Public` headers and must document ownership,
   threading and failure behaviour.
- Every Blueprint-callable function is a `UFUNCTION`; every broadcast event is a
  `BlueprintAssignable` dynamic delegate.
- Comments describe the code as it is now. Do not reference planning notes,
  milestone numbers or documents outside this repository.
- Tests are UE automation tests named `UXWebview.<Area>.<Case>`; run them with
  `-ExecCmds="Automation RunTests UXWebview"`.

## Verification

```
Build.bat UXWebviewSampleEditor Win64 Development -Project=Samples/UXWebviewSample/UXWebviewSample.uproject
```

Then exercise both backends and both display modes:

```
UnrealEditor.exe Samples/UXWebviewSample/UXWebviewSample.uproject -game -windowed -resx=1280 -resy=720 \
  -ExecCmds="UXWebviewSample.WidgetDemo 1 8 WebView2 about:blank 30 640 360 0 0,UXWebview.DumpState"
```

A healthy texture run logs `webview2 provider registered`,
`webview2 view created: texture ...`, `webview2 first frame WxH`, and
`state provider: WebView2 ... available=1`. Overlay runs log
`webview2 view created: overlay ...` and no frame lines.
