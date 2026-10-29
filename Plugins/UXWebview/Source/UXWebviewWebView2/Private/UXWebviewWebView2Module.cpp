#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewWebView2, Log, All);

class FUXWebviewWebView2Module : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogUXWebviewWebView2, Log, TEXT("UXWebviewWebView2 module loaded"));
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FUXWebviewWebView2Module, UXWebviewWebView2)
