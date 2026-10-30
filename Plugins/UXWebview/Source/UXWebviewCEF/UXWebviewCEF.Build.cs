using UnrealBuildTool;
using System.IO;

public class UXWebviewCEF : ModuleRules
{
	public UXWebviewCEF(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UXWebview",
			"Projects"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"RenderCore",
			"RHI",
			"Slate",
			"SlateCore"
		});

		// Shared frame/IPC headers live outside the plugin so the host can use them too.
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "..", "..", "..", "native", "include"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"D3D12RHI",
				"D3D11RHI"
			});

			// The host process and the CEF runtime ship next to the game binary
			// so packaged builds never need the native build tree. They go into
			// a subdirectory because CEF expects its own DLLs and locales to sit
			// beside UXWebviewHost.exe, and the names must not collide with the
			// game's own runtime DLLs.
			string PluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
			string HostRoot = Path.Combine(PluginRoot, "Binaries", "ThirdParty", "UXWebviewHost", "Win64");
			if (!Directory.Exists(HostRoot))
			{
				HostRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", "..",
					"native", "build", "Release"));
			}

			if (Directory.Exists(HostRoot))
			{
				string DestRoot = "$(BinaryOutputDir)/UXWebviewHost";
				string[] RuntimeFiles = new string[]
				{
					"UXWebviewHost.exe",
					"libcef.dll",
					"chrome_elf.dll",
					"chrome_100_percent.pak",
					"chrome_200_percent.pak",
					"resources.pak",
					"icudtl.dat",
					"v8_context_snapshot.bin",
					"dxcompiler.dll",
					"d3dcompiler_47.dll",
					"vk_swiftshader.dll",
					"vk_swiftshader_icd.json",
					"vulkan-1.dll"
				};
				foreach (string FileName in RuntimeFiles)
				{
					RuntimeDependencies.Add(DestRoot + "/" + FileName,
						Path.Combine(HostRoot, FileName), StagedFileType.NonUFS);
				}
				foreach (string Locale in new string[] { "en-US.pak", "zh-CN.pak" })
				{
					RuntimeDependencies.Add(DestRoot + "/locales/" + Locale,
						Path.Combine(HostRoot, "locales", Locale), StagedFileType.NonUFS);
				}
			}
		}
	}
}
