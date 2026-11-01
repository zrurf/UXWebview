#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "UXWebviewSettings.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewEditor, Log, All);

#define LOCTEXT_NAMESPACE "UXWebviewEditor"

namespace
{
	const TCHAR* SettingsContainer = TEXT("Project");
	const TCHAR* SettingsCategory = TEXT("Plugins");
	const TCHAR* SettingsSection = TEXT("UXWebview");
}

class FUXWebviewEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>("Settings"))
		{
			SettingsModule->RegisterSettings(SettingsContainer, SettingsCategory, SettingsSection,
				LOCTEXT("UXWebviewSettingsName", "UXWebview"),
				LOCTEXT("UXWebviewSettingsDescription",
					"Provider selection, script bridge allowlist and CEF host defaults."),
				GetMutableDefault<UUXWebviewSettings>());
		}

		UE_LOG(LogUXWebviewEditor, Log, TEXT("UXWebviewEditor module loaded"));
	}

	virtual void ShutdownModule() override
	{
		if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
		{
			SettingsModule->UnregisterSettings(SettingsContainer, SettingsCategory, SettingsSection);
		}
	}
};

IMPLEMENT_MODULE(FUXWebviewEditorModule, UXWebviewEditor)

#undef LOCTEXT_NAMESPACE
