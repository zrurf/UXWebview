using UnrealBuildTool;

public class UXWebviewSampleTarget : TargetRules
{
	public UXWebviewSampleTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UXWebviewSample");
	}
}
