# Getting started

## Requirements

- Unreal Engine 5.x (Windows target verified on 5.8)
- Windows 10/11; macOS and Linux are planned, not implemented
- Visual Studio 2022 with the C++ desktop workload
- CMake 3.20+ and PowerShell 7 for the native host
- Optional WebView2 backend: the Microsoft Edge WebView2 Runtime (installed by
  Windows 10/11 updates, Edge, or the Evergreen runtime installer)

## 1. Fetch the CEF runtime

```
powershell -ExecutionPolicy Bypass -File Scripts/FetchCef.ps1
```

The script downloads the CEF distribution and unpacks it under
`native/third_party/cef`. Re-running it is safe and skips existing files.

## 2. Optional: fetch the WebView2 SDK

```
powershell -ExecutionPolicy Bypass -File Scripts/FetchWebView2.ps1
```

This downloads the WebView2 NuGet package pinned in
`native/webview2.lock.json` and extracts its headers and loader into
`native/third_party/webview2`. `UXWebviewWebView2.Build.cs` detects that folder
and enables the backend; without it the module still compiles and the provider
reports itself as unavailable. The SDK is a build-time dependency only.

## 3. Build the native host

```
powershell -ExecutionPolicy Bypass -File Scripts/BuildHost.ps1
```

The host is written to `native/build/Release/UXWebviewHost.exe`. Unless you plan
to ship a packaged build, the plugin finds it there automatically.

## 4. Build the plugin

Open `Samples/UXWebviewSample/UXWebviewSample.uproject` in the engine version you
target and build the editor target, or run:

```
Build.bat UXWebviewSampleEditor Win64 Development -Project=Samples/UXWebviewSample/UXWebviewSample.uproject
```

The log must contain `UXWebview module loaded`. A second line,
`null provider registered`, confirms the fallback provider is available.
With the native host built, `cef provider registered` follows; with the
WebView2 SDK present, `webview2 provider registered` follows as well.

## 5. Run a scene

The sample project exposes console commands; any of them can be driven with
`-ExecCmds` for a headless smoke test.

| Command | Purpose |
| --- | --- |
| `UXWebviewSample.WidgetDemo` | Centered UMG panel; `[dpi] [seconds] [provider] [url] [fps] [width] [height] [overlay] [topLeft]`; width or height 0 fills the viewport; a positive `seconds` closes the demo |
| `UXWebviewSample.Hud` | HUD preset: top-left texture panel (CEF) |
| `UXWebviewSample.Fullscreen` | Fullscreen preset: viewport-sized overlay (CEF) |
| `UXWebviewSample.NullDemo` | Animated Null-provider panel; `[width] [height] [fps]` |
| `UXWebviewSample.NullStop` | Destroys the Null demo |
| `UXWebviewSample.WidgetStop` | Destroys the running widget scene |
| `UXWebviewSample.WidgetProbe` | Clicks the page; `[cssX] [cssY] [delay]` |
| `UXWebviewSample.WidgetIme` | Feeds IME composition/commit/cancel; `[text] [mode] [delay]` |
| `UXWebviewSample.WidgetKey` | Presses one key; `[vk] [ctrl] [delay]` |
| `UXWebviewSample.WidgetDrag` | Drags across the page; `[x1] [y1] [x2] [y2] [delay]` |
| `UXWebviewSample.WidgetNavigate` | `[delay] [url]` |
| `UXWebviewSample.WidgetReload` | `[delay]` |
| `UXWebviewSample.WidgetLoadHtml` | `[delay] [file]`; loads the file through `LoadHTML` |
| `UXWebviewSample.WidgetBridge` | Makes the page call `window.uxwv.call` and logs the reply |
| `UXWebviewSample.WidgetComposition` | Switches a WebView2 overlay between composition hosting and a windowed controller; `[on] [delay]` |
| `UXWebviewSample.WidgetFpsProbe` | Samples the page `requestAnimationFrame` rate |
| `UXWebviewSample.WidgetScreenshot` | Requests a `HighResShot` of the game viewport |
| `UXWebviewSample.WidgetEmit` | Sends a game event to the page; `[delay] [event] [payloadJson]` |
| `UXWebviewSample.WidgetSecond` | Adds a second view next to the demo; `[delay] [url] [fps]` |
| `UXWebview.DumpState` | Logs settings, providers and live views |

Example:

```
UnrealEditor.exe Samples/UXWebviewSample/UXWebviewSample.uproject -game -windowed -resx=1280 -resy=720 \
  -ExecCmds="UXWebviewSample.Hud file:///C:/path/page.html"

UnrealEditor.exe Samples/UXWebviewSample/UXWebviewSample.uproject -game -windowed -resx=1280 -resy=720 \
  -ExecCmds="UXWebviewSample.WidgetDemo 1 8 WebView2 about:blank 30 640 360 0 0"
```

`UXWebview.DumpState` prints one line per provider and one line per live view,
which is usually enough to tell a missing host from a failed navigation.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| `UXWebviewHost.exe was not found` | Host not built, or packaged build missing the staged host; set `UXWEBVIEW_HOST_EXE` to override |
| Panel shows but clicks do nothing | The game viewport captures the mouse; switch to `FInputModeGameAndUI` while the pointer is over the panel |
| Typing does nothing in texture mode | Widget never received keyboard focus, so no IME context was registered; click the panel once |
| Overlay window is not visible | The overlay is a native child window; it is clipped by the game window and hidden with it |
| `the Microsoft Edge WebView2 Runtime is not installed` | Install the WebView2 Evergreen Runtime, or use the CEF provider |
| WebView2 frames stop at ~30 fps | Texture mode captures through `CapturePreview`, which is a CPU readback; use overlay mode for full frame rate |
| WebView2 typing is slow in texture mode | Composition is forwarded through page JavaScript because the off-screen window has no native IME |
| `the game thread already uses a multithreaded COM apartment` | Another plugin initialized COM as MTA before the view was created; WebView2 requires an STA game thread |
