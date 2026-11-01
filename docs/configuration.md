# Configuration

## Project settings

`UUXWebviewSettings` is registered under **Project Settings > Plugins > UXWebview**
and stored in `Config/DefaultGame.ini`.

| Field | Default | Meaning |
| --- | --- | --- |
| `PlatformDefaultProvider` | `""` | Provider used when a view does not name one; empty falls back to the built-in default (`CEF`) |
| `FallbackProviders` | `[]` | Ordered candidates tried when the requested provider is unavailable |
| `bAllowProviderFallback` | `true` | When false, an unavailable explicitly requested provider fails instead of falling back |
| `BridgeAllowedOrigins` | `[]` | Origins allowed to call game functions from the page; empty denies every origin |
| `bDirectDisplay` | `true` | CEF texture mode binds the shared frame directly; disable to copy into a pool texture. WebView2 always uploads CPU pixels and ignores this |
| `bEnableDevTools` | `false` | Enables `OpenDevTools`; CEF also starts a DevTools HTTP endpoint |
| `DevToolsPort` | `9222` | CEF DevTools port; `http://127.0.0.1:9222/json/version` answers when enabled |
| `IdleShutdownSeconds` | `30` | CEF host exits after this many idle seconds with no live views. In-process WebView2 has no host process to reap |
| `bAutoRecoverOnCrash` | `true` | Restarts the CEF host or reloads the WebView2 page after a renderer failure |

## Per-view parameters

`FUXWebviewCreateParams` always wins over project settings:

| Field | Meaning |
| --- | --- |
| `ProviderName` | Explicit provider; empty uses the project default |
| `InitialUrl` | First page; empty starts blank |
| `ViewportSize` | Physical pixel size of the page |
| `DpiScale` | CSS-to-pixel scale; `0` follows the widget layout scale |
| `TargetFrameRate` | Frame request rate; the host never exceeds it |
| `DisplayMode` | `Texture` (off-screen frame) or `Overlay` (native child window) |

## Providers

| Name | Availability | Notes |
| --- | --- | --- |
| `Null` | Always | Animated texture, no browser; useful for layout work and tests |
| `CEF` | Windows with a built host | Shared-texture frames or native overlay; primary backend |
| `WebView2` | Windows with the SDK at build time and the Edge WebView2 Runtime installed | In-process Edge; CPU-captured texture up to 30 fps or native overlay |

Provider resolution order is: an explicit `ProviderName`, then
`PlatformDefaultProvider`, then the built-in default (`CEF`), then
`FallbackProviders` when `bAllowProviderFallback` is enabled. A provider that
resolves but is unavailable fails with its `UnavailableReason`.

`UUXWebviewSubsystem::GetProviders()` returns every registered provider with its
capabilities and an availability reason, which is the same data that
`UXWebview.DumpState` logs.

## Display modes

- **Texture** renders the page off-screen and publishes a texture that the
  widget draws. CEF shares a GPU texture; WebView2 captures PNG frames and
  uploads CPU pixels, so its frame rate is capped at 30. Frames are paced by the
  game tick and the target frame rate.
- **Overlay** creates a native browser child window inside the game window. No
  frame copies occur and the page runs on its own compositor; input, IME and
  clipping follow the native window rules. Overlay also runs the full frame rate
  for WebView2.

Overlay and texture views can coexist in one process.
