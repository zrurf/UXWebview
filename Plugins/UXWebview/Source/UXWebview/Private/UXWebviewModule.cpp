#include "Modules/ModuleManager.h"

#include "Null/UXWebviewNullProvider.h"
#include "UXWebviewDiagnostics.h"
#include "UXWebviewLog.h"

DEFINE_LOG_CATEGORY(LogUXWebview);

class FUXWebviewModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RegisterNullProvider();
		UXWebviewDiagnostics::Register();
		UE_LOG(LogUXWebview, Log, TEXT("UXWebview module loaded"));
	}

	virtual void ShutdownModule() override
	{
		UXWebviewDiagnostics::Unregister();
		UnregisterNullProvider();
	}
};

IMPLEMENT_MODULE(FUXWebviewModule, UXWebview)
