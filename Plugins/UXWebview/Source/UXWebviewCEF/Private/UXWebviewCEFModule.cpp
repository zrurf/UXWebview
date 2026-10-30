#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "CEF/UXWebviewCEFProvider.h"
#include "CEF/UXWebviewHostClient.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewCEF, Log, All);

class FUXWebviewCEFModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if PLATFORM_WINDOWS
		RegisterCEFProvider();
		UE_LOG(LogUXWebviewCEF, Log, TEXT("UXWebviewCEF module loaded"));
#endif
	}

	virtual void ShutdownModule() override
	{
#if PLATFORM_WINDOWS
		// Views are normally gone by now; this is the last chance to stop the host.
		UnregisterCEFProvider();
		FUXWebviewHostClient::Get().Shutdown();
		UE_LOG(LogUXWebviewCEF, Log, TEXT("UXWebviewCEF module unloaded"));
#endif
	}
};

IMPLEMENT_MODULE(FUXWebviewCEFModule, UXWebviewCEF)
