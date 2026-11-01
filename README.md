# UXWebview

Cross-platform WebView plugin for Unreal Engine 5. The plugin keeps browser
processes outside the engine process and transfers frames through GPU shared
textures, so pages can be shown in UMG, as textures, or as a native overlay.

## Status

Windows is feature-complete: CEF off-screen rendering with shared-texture frames,
native overlay windows, UMG widget with mouse/keyboard/IME/clipboard, script
bridge with an origin allowlist, crash recovery and packaged builds. macOS and
Linux accept the same provider interface but have no host yet; the WebView2
provider is still a stub.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/getting-started.md](docs/getting-started.md) | Build steps, sample commands, troubleshooting |
| [docs/configuration.md](docs/configuration.md) | Project settings, per-view parameters, display modes |
| [docs/script-bridge.md](docs/script-bridge.md) | `window.uxwv` API, allowlist, failure modes |
| [docs/packaging.md](docs/packaging.md) | Cook/stage/archive, host lookup, bundled CEF runtime |

## Requirements

- Unreal Engine 5.0 - 5.8, desktop platforms first (Windows, macOS, Linux)
- Windows builds use the CEF binaries fetched by `Scripts/FetchCef.*`
- Native host: CMake 3.20+, C++17

## Layout

| Path | Purpose |
| --- | --- |
| `Plugins/UXWebview` | Plugin root: runtime modules and editor tooling |
| `Plugins/UXWebview/Source` | Module sources (`UXWebview`, `UXWebviewWidget`, `UXWebviewCEF`, `UXWebviewWebView2`, `UXWebviewEditor`) |
| `native/host` | Standalone CEF host process |
| `Samples/UXWebviewSample` | Sample project used for development and verification |

## Building the sample

1. Open `Samples/UXWebviewSample/UXWebviewSample.uproject` in the engine version
   you target; the project resolves `Plugins/UXWebview` through
   `AdditionalPluginDirectories`.
2. Build the editor target, then check the log for `UXWebview module loaded`.

## License

MIT, see `LICENSE` and `NOTICE` for third-party components.
