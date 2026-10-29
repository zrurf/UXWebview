#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewEditor, Log, All);

class FUXWebviewEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogUXWebviewEditor, Log, TEXT("UXWebviewEditor module loaded"));
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FUXWebviewEditorModule, UXWebviewEditor)
