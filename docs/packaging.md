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

## Command

```
powershell -ExecutionPolicy Bypass -File Scripts/Package.ps1 -EngineRoot 'E:\UnrealEngine\UE_5.8'
```

Options:

| Flag | Effect |
| --- | --- |
| `-Config Shipping` | Client configuration (default `Shipping`) |
| `-ArchiveDir <dir>` | Archive output; defaults to `Samples/UXWebviewSample/Saved/Package/Windows` |
| `-BuildEditor` | Let UAT compile the editor target instead of `-nobuild` |
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

## Verifying

Run the packaged executable with `-ExecCmds="UXWebview.DumpState"` and check the
log for the provider list and a live view line; then open a page and confirm
frames arrive.
