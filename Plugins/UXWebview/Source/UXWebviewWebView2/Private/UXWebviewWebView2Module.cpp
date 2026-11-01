#include "Modules/ModuleManager.h"

#include "UXWebviewWebView2Provider.h"

DEFINE_LOG_CATEGORY(LogUXWebviewWebView2);

class FUXWebviewWebView2Module : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if PLATFORM_WINDOWS && defined(UXWEBVIEW_WITH_WEBVIEW2) && UXWEBVIEW_WITH_WEBVIEW2
		RegisterWebView2Provider();
#else
		UE_LOG(LogUXWebviewWebView2, Log,
			TEXT("WebView2 backend is not built for this platform"));
#endif
	}

	virtual void ShutdownModule() override
	{
#if PLATFORM_WINDOWS && defined(UXWEBVIEW_WITH_WEBVIEW2) && UXWEBVIEW_WITH_WEBVIEW2
		UnregisterWebView2Provider();
#endif
	}
};

IMPLEMENT_MODULE(FUXWebviewWebView2Module, UXWebviewWebView2)
