# Getting started

## Requirements

- Unreal Engine 5.x (Windows target verified on 5.8)
- Windows 10/11; macOS and Linux are planned, not implemented
- Visual Studio 2022 with the C++ desktop workload
- CMake 3.20+ and PowerShell 7 for the native host

## 1. Fetch the CEF runtime

```
powershell -ExecutionPolicy Bypass -File Scripts/FetchCef.ps1
```

The script downloads the CEF distribution and unpacks it under
`native/third_party/cef`. Re-running it is safe and skips existing files.

## 2. Build the native host

```
powershell -ExecutionPolicy Bypass -File Scripts/BuildHost.ps1
```

The host is written to `native/build/Release/UXWebviewHost.exe`. Unless you plan
to ship a packaged build, the plugin finds it there automatically.

## 3. Build the plugin

Open `Samples/UXWebviewSample/UXWebviewSample.uproject` in the engine version you
target and build the editor target, or run:

```
Build.bat UXWebviewSampleEditor Win64 Development -Project=Samples/UXWebviewSample/UXWebviewSample.uproject
```

The log must contain `UXWebview module loaded`. A second line,
`null provider registered`, confirms the fallback provider is available.

## 4. Run a scene

The sample project exposes console commands; any of them can be driven with
`-ExecCmds` for a headless smoke test.

| Command | Purpose |
| --- | --- |
| `UXWebviewSample.WidgetDemo` | Centered UMG panel; `[dpi] [seconds] [provider] [url] [fps] [width] [height] [overlay] [topLeft]`; width or height 0 fills the viewport |
| `UXWebviewSample.Hud` | HUD preset: top-left texture panel |
| `UXWebviewSample.Fullscreen` | Fullscreen preset: viewport-sized overlay |
| `UXWebviewSample.WidgetStop` | Destroys the running scene |
| `UXWebviewSample.WidgetProbe` | Injects a click at page coordinates |
| `UXWebviewSample.WidgetNavigate`, `.WidgetReload`, `.WidgetLoadHtml` | Navigation smoke tests |
| `UXWebview.DumpState` | Logs settings, providers and live views |

Example:

```
UnrealEditor.exe Samples/UXWebviewSample/UXWebviewSample.uproject -game -windowed -resx=1280 -resy=720 \
  -ExecCmds="UXWebviewSample.Hud file:///C:/path/page.html"
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
