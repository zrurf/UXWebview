# Packaging

Packaged builds ship the CEF host inside the game binaries, so no native build
tree is needed on the target machine.

## What ships

`UXWebviewCEF.Build.cs` stages these files into `Binaries/Win64/UXWebviewHost/`
next to the game executable:

- `UXWebviewHost.exe`
- `libcef.dll`, `chrome_elf.dll`, `chrome_100_percent.pak`, `chrome_200_percent.pak`,
  `resources.pak`, `icudtl.dat`, `v8_context_snapshot.bin`
- `dxcompiler.dll`, `d3dcompiler_47.dll`, `vk_swiftshader.dll`,
  `vk_swiftshader_icd.json`, `vulkan-1.dll`
- `locales/en-US.pak`, `locales/zh-CN.pak`

The staging source is `Plugins/UXWebview/Binaries/ThirdParty/UXWebviewHost/Win64`,
which `Scripts/BuildHost.ps1` fills; if that folder is absent the module falls
back to `native/build/Release`.

The WebView2 backend stages only `WebView2Loader.dll` next to the game binary.
The browser itself is the machine-wide Microsoft Edge WebView2 Runtime and is
never copied into the archive; target machines need it installed (Windows 10/11
usually has it already). A build without the WebView2 SDK simply omits the
provider.

## Command

```
powershell -ExecutionPolicy Bypass -File Scripts/Package.ps1 -EngineRoot 'E:\UnrealEngine\UE_5.8'
```

Options:

| Flag | Effect |
| --- | --- |
| `-Config Shipping` | Client configuration (default `Shipping`) |
| `-ArchiveDir <dir>` | Archive output; defaults to `Samples/UXWebviewSample/Saved/Package/Windows` |
| `-Build` | Let UAT compile the game target instead of passing `-nobuild` |
| `-SkipHost` | Reuse the already staged host instead of rebuilding |

The script rebuilds the host, runs `BuildCookRun -cook -stage -pak -archive`,
then verifies that `UXWebviewHost.exe`, `libcef.dll`, `resources.pak` and
`locales/zh-CN.pak` are present in the archive.

## Host lookup order

At runtime the plugin looks for the host in this order:

1. `UXWEBVIEW_HOST_EXE` environment variable
2. `Plugins/UXWebview/Binaries/Win64/UXWebviewHost.exe` (installed plugin)
3. `native/build/Release/UXWebviewHost.exe` (source checkout)
4. `<Project>/Binaries/Win64/UXWebviewHost/UXWebviewHost.exe` (packaged build)
5. `<Project>/Binaries/ThirdParty/UXWebviewHost/Win64/UXWebviewHost.exe`

If none is found the view fails with `UXWebviewHost.exe was not found; build
native/host or set UXWEBVIEW_HOST_EXE`.

## Sample project note

`AdditionalPluginDirectories` must point at `Plugins`, not at the repository
root. Pointing at the root makes UAT remap the sample project itself into
`RemappedPlugins/Samples/...`, which breaks descriptor paths
(`Failed to open descriptor file`) in the packaged build.

## Verifying a packaged build

Shipping builds compile out console commands and `Log`-level `UE_LOG` output, so
the sample answers to two command-line keys instead:

```
UXWebviewSample.exe -windowed -resx=1280 -resy=720 -nosplash ^
  -uxwv-demo="0 16 CEF http://127.0.0.1:8765/page-links-http.html 30 640 360 0 0 2" ^
  -uxwv-clicks="160 180 8;480 180 12"
```

`-uxwv-demo` takes the `UXWebviewSample.WidgetDemo` argument list
(`dpi seconds provider url fps width height overlay topLeft newWindow`); the
demo only starts once the game has a viewport, and a non-zero `seconds` exits
the game afterwards. `-uxwv-clicks` queues `x y delay` probe clicks separated
by `;` so a smoke page can be driven without console commands.

What to check:

- `<Saved>/Logs/UXWebviewHost.log` reports `new window ... policy=broadcast`
  for the `newWindow=2` argument and `download ... completed` for a page that
  serves `Content-Disposition: attachment`.
- The file lands in `<Saved>/UXWebview/Downloads/`.
- The host logs `host shutting down` after the demo duration and leaves no
  `UXWebviewHost` process behind.

A packaged game resolves `<Saved>` to `%LOCALAPPDATA%/<Project>/Saved`.

In the editor and in non-shipping builds the same scenes are reachable through
`UXWebviewSample.WidgetDemo`, `UXWebviewSample.WidgetProbe` and
`UXWebview.DumpState`, which reports the provider list and a live view line
(`available=1` when the WebView2 runtime is present).
