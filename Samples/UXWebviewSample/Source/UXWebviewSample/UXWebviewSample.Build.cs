using UnrealBuildTool;

public class UXWebviewSample : ModuleRules
{
	public UXWebviewSample(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"UXWebview",
			"UXWebviewWidget",
			"Slate",
			"SlateCore"
		});
	}
}
