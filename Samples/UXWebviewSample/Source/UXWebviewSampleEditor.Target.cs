using UnrealBuildTool;

public class UXWebviewSampleEditorTarget : TargetRules
{
	public UXWebviewSampleEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UXWebviewSample");
	}
}
