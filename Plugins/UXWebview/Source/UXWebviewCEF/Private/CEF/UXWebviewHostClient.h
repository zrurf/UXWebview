#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS

#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "Protocol/UXWebviewHostRecovery.h"
#include "Protocol/UXWebviewInstanceTable.h"
#include "UXWebviewTypes.h"

/**
 * Per-instance callbacks. Every method runs on the game thread; the client is
 * owned by the provider and outlives the host client registration.
 */
class IUXWebviewHostInstanceClient
{
public:
	virtual ~IUXWebviewHostInstanceClient() = default;

	virtual void OnHostReady() = 0;
	virtual void OnHostLoadStart() = 0;
	virtual void OnHostLoadEnd(const FString& Url, int32 Code) = 0;
	virtual void OnHostTitleChanged(const FString& Title) = 0;
	virtual void OnHostUrlChanged(const FString& Url) = 0;
	virtual void OnHostCrash(const FString& Reason) = 0;
	virtual void OnHostRecovered() = 0;
	virtual void OnHostScriptResult(uint32 RequestId, bool bSuccess, const FString& Result) = 0;
	/** A page query that needs an answer through ReplyBridge. */
	virtual void OnHostBridgeMessage(const FString& Name, const FString& PayloadJson,
		uint32 RequestId) = 0;
	virtual void OnHostDevTools(const FString& Url) = 0;
};

/** Everything the host needs to create one browser. */
struct FUXWebviewHostInstanceSpec
{
	FIntPoint Size = FIntPoint(1280, 720);
	/** Device pixels per CSS pixel; the host reports it to CEF as the screen scale. */
	float DpiScale = 1.0f;
	FString Url;
	int32 FrameRate = 60;
	bool bVisible = true;
};

/**
 * Owns the single shared UXWebviewHost.exe process for this game instance.
 * Instances are created over one IPC pipe; frames arrive through per instance
 * shared memory. Recovery, heartbeat and process lifetime are handled here.
 */
class FUXWebviewHostClient
{
public:
	static FUXWebviewHostClient& Get();

	/** True when a host executable can be located; never launches anything. */
	bool IsHostAvailable(FString* OutReason = nullptr) const;

	/** Creates the host on first use and registers a new browser instance. */
	FUXWebviewResult CreateInstance(const FUXWebviewHostInstanceSpec& Spec,
		IUXWebviewHostInstanceClient* Client, uint32& OutInstanceId);
	void DestroyInstance(uint32 InstanceId);

	FUXWebviewResult Navigate(uint32 InstanceId, const FString& Url);
	FUXWebviewResult LoadHtml(uint32 InstanceId, const FString& Html, const FString& BaseUrl);
	FUXWebviewResult Reload(uint32 InstanceId);
	FUXWebviewResult StopLoading(uint32 InstanceId);
	FUXWebviewResult Resize(uint32 InstanceId, const FIntPoint& Size, float DpiScale);
	FUXWebviewResult SetVisible(uint32 InstanceId, bool bVisible);
	FUXWebviewResult SetFrameRate(uint32 InstanceId, int32 FramesPerSecond);
	FUXWebviewResult SetFocus(uint32 InstanceId, bool bFocused);
	FUXWebviewResult BeginFrame(uint32 InstanceId);
	FUXWebviewResult SendInput(uint32 InstanceId, const FUXWebviewInputEvent& Event);
	FUXWebviewResult ExecuteScript(uint32 InstanceId, uint32 RequestId, const FString& Script);
	FUXWebviewResult EmitToWeb(uint32 InstanceId, const FString& Event, const FString& PayloadJson);
	/** Resolves a pending page query; the JS promise settles with this value. */
	FUXWebviewResult ReplyBridge(uint32 InstanceId, uint32 RequestId, bool bSuccess,
		const FString& Result);
	FUXWebviewResult RequestDevTools(uint32 InstanceId);

	/** Shared memory name of the current host generation; empty when unknown. */
	FString GetSharedMemoryName(uint32 InstanceId) const;
	bool IsInstanceReady(uint32 InstanceId) const;

	/** Sends SHUTDOWN, waits briefly and closes the process. */
	void Shutdown();

private:
	struct FInstance
	{
		uint32 Id = 0;
		IUXWebviewHostInstanceClient* Client = nullptr;
		FUXWebviewHostInstanceSpec Spec;
		FString SharedMemoryName;
		bool bCreateSent = false;
		bool bReady = false;
		bool bRecoverPending = false;
	};

	FUXWebviewHostClient() = default;
	~FUXWebviewHostClient();

	void EnsureTicker();
	bool Tick(float DeltaSeconds);
	bool LaunchHost(double NowSeconds);
	void TerminateHost();
	void TryConnect(double NowSeconds);
	bool PollPipe();
	void HandleProcessExit(double NowSeconds, const char* Reason);
	void HandleDisconnect();
	void Dispatch(const struct FUXWebviewIpcEnvelope& Envelope);
	void FlushPendingCreates();

	bool SendRaw(uint32 Type, uint32 InstanceId, uint32 RequestId, uint32 A, uint32 B,
		uint32 Flags, const TArray<uint8>& Payload);
	bool SendJson(uint32 Type, uint32 InstanceId, uint32 RequestId, uint32 A, uint32 B,
		const FString& Json);

	void NotifyAllCrashed(const FString& Reason);
	void ResetInstanceGeneration();
	static bool FindHostExecutable(FString& OutPath);
	static uint32 CurrentProcessId();

	FProcHandle HostProcess;
	FString HostPath;
	FString HostArgs;
	FString PipeName;
	uint32 SessionCounter = 0;
	bool bPipeOpen = false;
	bool bHelloSent = false;
	bool bHelloAck = false;
	bool bExitHandled = false;
	bool bGiveUp = false;
	bool bTickerRegistered = false;

	void* PipeHandle = nullptr;
	TArray<uint8> ReadBuffer;

	double NextConnectSeconds = 0.0;
	double NextPingSeconds = 0.0;

	FUXWebviewHostRecovery Recovery;
	TUXWebviewInstanceTable<FInstance> Instances;
	FTSTicker::FDelegateHandle TickerHandle;
};

#endif // PLATFORM_WINDOWS
