#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewCEF, Log, All);

class FUXWebviewCEFModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogUXWebviewCEF, Log, TEXT("UXWebviewCEF module loaded"));
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FUXWebviewCEFModule, UXWebviewCEF)
