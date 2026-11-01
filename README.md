# UXWebview

Cross-platform WebView plugin for Unreal Engine 5. Pages render in UMG, as
textures, or in a native overlay window. The CEF backend keeps the browser in a
separate process and shares GPU textures; the optional WebView2 backend runs
in-process and uploads CPU frames.

## Status

Windows is feature-complete: CEF off-screen rendering with shared-texture frames,
native overlay windows, UMG widget with mouse/keyboard/IME/clipboard, script
bridge with an origin allowlist, crash recovery and packaged builds.

Windows also has an in-process WebView2 (Microsoft Edge) backend. It captures
the page to a CPU texture at up to 30 fps or hosts it in a native child window;
it needs the WebView2 Runtime, which ships with Windows 10/11 but is not bundled
by this plugin. macOS and Linux accept the same provider interface but have no
host yet.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/getting-started.md](docs/getting-started.md) | Build steps, sample commands, troubleshooting |
| [docs/architecture.md](docs/architecture.md) | Module layout, provider contract, capability matrix, adding a backend |
| [docs/api-reference.md](docs/api-reference.md) | C++ and Blueprint API for game code |
| [docs/configuration.md](docs/configuration.md) | Project settings, per-view parameters, display modes |
| [docs/script-bridge.md](docs/script-bridge.md) | `window.uxwv` API, allowlist, failure modes |
| [docs/packaging.md](docs/packaging.md) | Cook/stage/archive, host lookup, bundled CEF runtime |

## Requirements

- Unreal Engine 5.0 - 5.8, desktop platforms first (Windows, macOS, Linux)
- Windows builds use the CEF binaries fetched by `Scripts/FetchCef.*`
- The optional WebView2 backend uses the SDK fetched by `Scripts/FetchWebView2.ps1`
  and the Microsoft Edge WebView2 Runtime installed on the target machine
- Native host: CMake 3.20+, C++17

## Layout

| Path | Purpose |
| --- | --- |
| `Plugins/UXWebview` | Plugin root: runtime modules and editor tooling |
| `Plugins/UXWebview/Source` | Module sources (`UXWebview`, `UXWebviewWidget`, `UXWebviewCEF`, `UXWebviewWebView2`, `UXWebviewEditor`) |
| `native/host` | Standalone CEF host process |
| `native/third_party` | Fetched third-party inputs (CEF runtime, WebView2 SDK); not committed |
| `Samples/UXWebviewSample` | Sample project used for development and verification |

## Building the sample

1. Open `Samples/UXWebviewSample/UXWebviewSample.uproject` in the engine version
   you target; the project resolves `Plugins/UXWebview` through
   `AdditionalPluginDirectories`.
2. Build the editor target, then check the log for `UXWebview module loaded`.

## License

MIT, see `LICENSE` and `NOTICE` for third-party components.
