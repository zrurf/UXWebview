using UnrealBuildTool;
using System.IO;

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

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"ImageWrapper",
			"ImageCore",
			"Slate",
			"SlateCore"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// The WebView2 SDK is fetched by Scripts/FetchWebView2.ps1. Only the
			// headers and loader are shipped; the runtime comes with Windows/Edge.
			string ProjectRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", ".."));
			string SdkRoot = Path.Combine(ProjectRoot, "native", "third_party", "webview2");
			string IncludeRoot = Path.Combine(SdkRoot, "include");
			string LoaderDll = Path.Combine(SdkRoot, "x64", "WebView2Loader.dll");
			string LoaderLib = Path.Combine(SdkRoot, "x64", "WebView2Loader.lib");

			if (File.Exists(Path.Combine(IncludeRoot, "WebView2.h")) && File.Exists(LoaderLib))
			{
				PublicIncludePaths.Add(IncludeRoot);
				PublicDefinitions.Add("UXWEBVIEW_WITH_WEBVIEW2=1");
				PublicAdditionalLibraries.Add(LoaderLib);
				PublicDelayLoadDLLs.Add("WebView2Loader.dll");
				RuntimeDependencies.Add("$(BinaryOutputDir)/WebView2Loader.dll", LoaderDll,
					StagedFileType.NonUFS);

				// Composition hosting draws the page into a DirectComposition visual,
				// which needs a D3D11 device as the composition device.
				PublicSystemLibraries.AddRange(new string[]
				{
					"d3d11.lib",
					"dcomp.lib"
				});
			}
			else
			{
				// Keep the module loadable on machines without the SDK; it reports
				// the provider as unavailable instead of failing the build.
				PublicDefinitions.Add("UXWEBVIEW_WITH_WEBVIEW2=0");
			}
		}
		else
		{
			PublicDefinitions.Add("UXWEBVIEW_WITH_WEBVIEW2=0");
		}
	}
}
