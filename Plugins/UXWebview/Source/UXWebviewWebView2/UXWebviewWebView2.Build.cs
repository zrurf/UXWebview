using UnrealBuildTool;

public class UXWebviewWebView2 : ModuleRules
{
	public UXWebviewWebView2(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UXWebview"
		});
	}
}
