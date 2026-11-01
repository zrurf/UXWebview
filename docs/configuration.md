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
| `bDirectDisplay` | `true` | Texture mode shows the shared frame texture directly; disable to copy into a pool texture |
| `bEnableDevTools` | `false` | Starts the host with a DevTools HTTP endpoint |
| `DevToolsPort` | `9222` | DevTools port; `http://127.0.0.1:9222/json/version` answers when enabled |
| `IdleShutdownSeconds` | `30` | Host exits after this many idle seconds with no live views |
| `bAutoRecoverOnCrash` | `true` | Restarts the host and recreates views after a host crash |

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
| `CEF` | Windows with a built host | Shared-texture frames or native overlay |
| `WebView2` | Not implemented | Module is a stub |

`UUXWebviewSubsystem::GetProviders()` returns every registered provider with its
capabilities and an availability reason, which is the same data that
`UXWebview.DumpState` logs.

## Display modes

- **Texture** renders the page off-screen and publishes a shared texture; the
  widget draws it. Frames are paced by the game tick and the target frame rate.
- **Overlay** creates a native Chromium child window inside the game window. No
  frame copies occur and the page runs on its own compositor; input, IME and
  clipping follow the native window rules.

Overlay and texture views can coexist in one host session.
