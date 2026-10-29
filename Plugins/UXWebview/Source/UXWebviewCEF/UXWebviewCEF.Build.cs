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
		}
	}
}
