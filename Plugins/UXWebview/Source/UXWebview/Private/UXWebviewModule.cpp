#include "Modules/ModuleManager.h"

#include "Null/UXWebviewNullProvider.h"
#include "UXWebviewLog.h"

DEFINE_LOG_CATEGORY(LogUXWebview);

class FUXWebviewModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RegisterNullProvider();
		UE_LOG(LogUXWebview, Log, TEXT("UXWebview module loaded"));
	}

	virtual void ShutdownModule() override
	{
		UnregisterNullProvider();
	}
};

IMPLEMENT_MODULE(FUXWebviewModule, UXWebview)
