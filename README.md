# UXWebview

Cross-platform WebView plugin for Unreal Engine 5. The plugin keeps browser
processes outside the engine process and transfers frames through GPU shared
textures, so pages can be shown in UMG, as textures, or as a native overlay.

## Status

Repository scaffold (M0). No functional API yet: modules load, but providers
and the native host are not implemented.

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
