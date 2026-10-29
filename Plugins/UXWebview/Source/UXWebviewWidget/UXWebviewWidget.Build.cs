using UnrealBuildTool;

public class UXWebviewWidget : ModuleRules
{
	public UXWebviewWidget(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UXWebview",
			"UMG",
			"Slate",
			"SlateCore",
			"InputCore"
		});
	}
}
