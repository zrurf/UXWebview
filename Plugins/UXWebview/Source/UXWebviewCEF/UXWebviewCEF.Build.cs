using UnrealBuildTool;

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
			"RHI"
		});
	}
}
