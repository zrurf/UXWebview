#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewWidget, Log, All);

class FUXWebviewWidgetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogUXWebviewWidget, Log, TEXT("UXWebviewWidget module loaded"));
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FUXWebviewWidgetModule, UXWebviewWidget)
