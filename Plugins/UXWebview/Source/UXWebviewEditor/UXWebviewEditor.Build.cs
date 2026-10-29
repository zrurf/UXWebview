using UnrealBuildTool;

public class UXWebviewEditor : ModuleRules
{
	public UXWebviewEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UXWebview",
			"UnrealEd"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate"
		});
	}
}
