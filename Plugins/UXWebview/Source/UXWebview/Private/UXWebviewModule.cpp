#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebview, Log, All);

class FUXWebviewModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogUXWebview, Log, TEXT("UXWebview module loaded"));
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FUXWebviewModule, UXWebview)
