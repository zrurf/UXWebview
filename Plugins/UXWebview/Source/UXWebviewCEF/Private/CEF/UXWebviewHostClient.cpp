#include "CEF/UXWebviewHostClient.h"

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "CEF/UXWebviewSharedTexture.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Protocol/UXWebviewIpcCodec.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UXWebviewSettings.h"
#include "uxwebview_ipc.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewHostClient, Log, All);

namespace
{
constexpr double kConnectRetrySeconds = 0.05;
constexpr double kPingIntervalSeconds = 2.0;
constexpr double kShutdownWaitSeconds = 1.0;

FString QuoteIfNeeded(const FString& Value)
{
	return Value.Contains(TEXT(" ")) ? FString::Printf(TEXT("\"%s\""), *Value) : Value;
}

FString Utf8ToString(const TArray<uint8>& Bytes)
{
	if (Bytes.Num() == 0)
	{
		return FString();
	}

	TArray<ANSICHAR> Buffer;
	Buffer.SetNumUninitialized(Bytes.Num() + 1);
	FMemory::Memcpy(Buffer.GetData(), Bytes.GetData(), Bytes.Num());
	Buffer[Bytes.Num()] = 0;
	return FString(UTF8_TO_TCHAR(Buffer.GetData()));
}

TArray<uint8> StringToPayload(const FString& Json)
{
	TArray<uint8> Payload;
	if (Json.IsEmpty())
	{
		return Payload;
	}

	const FTCHARToUTF8 Utf8(*Json);
	Payload.SetNumUninitialized(Utf8.Length());
	FMemory::Memcpy(Payload.GetData(), Utf8.Get(), Utf8.Length());
	return Payload;
}

FString SerializeJson(const TSharedRef<FJsonObject>& Object)
{
	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Object, Writer);
	return Out;
}

TSharedPtr<FJsonObject> ParseJsonObject(const TArray<uint8>& Payload)
{
	const FString Json = Utf8ToString(Payload);
	if (Json.IsEmpty())
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return nullptr;
	}
	return Object;
}

FString JsonString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	if (!Object.IsValid())
	{
		return FString();
	}
	FString Value;
	Object->TryGetStringField(Field, Value);
	return Value;
}

int32 JsonInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 Fallback)
{
	if (!Object.IsValid())
	{
		return Fallback;
	}
	int32 Value = Fallback;
	Object->TryGetNumberField(Field, Value);
	return Value;
}

bool JsonBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool bFallback)
{
	if (!Object.IsValid())
	{
		return bFallback;
	}
	bool bValue = bFallback;
	Object->TryGetBoolField(Field, bValue);
	return bValue;
}
} // namespace

FUXWebviewHostClient& FUXWebviewHostClient::Get()
{
	static FUXWebviewHostClient Instance;
	return Instance;
}

FUXWebviewHostClient::~FUXWebviewHostClient()
{
	Shutdown();
}

uint32 FUXWebviewHostClient::CurrentProcessId()
{
	return FPlatformProcess::GetCurrentProcessId();
}

bool FUXWebviewHostClient::FindHostExecutable(FString& OutPath)
{
	TArray<FString> Candidates;

	const FString EnvHost = FPlatformMisc::GetEnvironmentVariable(TEXT("UXWEBVIEW_HOST_EXE"));
	if (!EnvHost.IsEmpty())
	{
		Candidates.Add(EnvHost);
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UXWebview"));
	if (Plugin.IsValid())
	{
		const FString PluginBase = Plugin->GetBaseDir();
		Candidates.Add(FPaths::Combine(PluginBase, TEXT("Binaries/Win64/UXWebviewHost.exe")));
		Candidates.Add(FPaths::Combine(PluginBase, TEXT("../../native/build/Release/UXWebviewHost.exe")));
	}
	// Packaged builds stage the host and the CEF runtime under this folder.
	Candidates.Add(FPaths::Combine(FPaths::ProjectDir(),
		TEXT("Binaries/Win64/UXWebviewHost/UXWebviewHost.exe")));
	Candidates.Add(FPaths::Combine(FPaths::ProjectDir(),
		TEXT("Binaries/ThirdParty/UXWebviewHost/Win64/UXWebviewHost.exe")));

	for (const FString& Candidate : Candidates)
	{
		const FString FullPath = FPaths::ConvertRelativePathToFull(Candidate);
		if (FPaths::FileExists(FullPath))
		{
			OutPath = FullPath;
			return true;
		}
	}

	OutPath.Reset();
	return false;
}

bool FUXWebviewHostClient::IsHostAvailable(FString* OutReason) const
{
	FString Path;
	if (FindHostExecutable(Path))
	{
		return true;
	}

	if (OutReason)
	{
		*OutReason = TEXT("UXWebviewHost.exe was not found; build native/host or set UXWEBVIEW_HOST_EXE");
	}
	return false;
}

FUXWebviewResult FUXWebviewHostClient::CreateInstance(const FUXWebviewHostInstanceSpec& Spec,
	IUXWebviewHostInstanceClient* Client, uint32& OutInstanceId)
{
	OutInstanceId = 0;
	if (!Client)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("host instance needs a client"));
	}
	if (Spec.Size.X <= 0 || Spec.Size.Y <= 0)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs,
			TEXT("host instance needs a positive viewport"));
	}
	if (bGiveUp)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed,
			TEXT("the host process was given up after repeated failures"));
	}

	const bool bHostRunning = HostProcess.IsValid() && FPlatformProcess::IsProcRunning(HostProcess);
	if (!bHostRunning && Instances.Num() == 0)
	{
		FString Path;
		if (!FindHostExecutable(Path))
		{
			return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
				TEXT("UXWebviewHost.exe was not found; build native/host or set UXWEBVIEW_HOST_EXE"));
		}

		if (Recovery.IsFailed())
		{
			Recovery.Reset();
		}

		const double Now = FPlatformTime::Seconds();
		if (!LaunchHost(Now))
		{
			return FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed,
				TEXT("the CEF host process could not be launched"));
		}
	}

	FInstance* Instance = new FInstance();
	Instance->Spec = Spec;
	Instance->Client = Client;

	const uint32 Id = Instances.Add(Instance);
	if (Id == 0)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::Unknown,
			TEXT("could not allocate a host instance"));
	}

	Instance->Id = Id;
	Instance->SharedMemoryName = FString::Printf(TEXT("UXWebviewFrame-%u-%u-%u"),
		CurrentProcessId(), SessionCounter, Id);

	UE_LOG(LogUXWebviewHostClient, Log,
		TEXT("instance %u registered: %dx%d fps=%d visible=%d url='%s'"),
		Id, Spec.Size.X, Spec.Size.Y, Spec.FrameRate, Spec.bVisible ? 1 : 0, *Spec.Url);

	if (bHelloAck)
	{
		FlushPendingCreates();
	}

	EnsureTicker();
	OutInstanceId = Id;
	return FUXWebviewResult::Ok();
}

void FUXWebviewHostClient::EnsureTicker()
{
	if (bTickerRegistered)
	{
		return;
	}

	bTickerRegistered = true;
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FUXWebviewHostClient::Tick));
}

void FUXWebviewHostClient::DestroyInstance(uint32 InstanceId)
{
	FInstance* Instance = Instances.Find(InstanceId);
	if (!Instance)
	{
		return;
	}

	if (bHelloAck)
	{
		SendRaw(UXWV_IPC_DESTROY, InstanceId, 0, 0, 0, 0, TArray<uint8>());
	}

	UE_LOG(LogUXWebviewHostClient, Log, TEXT("instance %u released"), InstanceId);
	Instances.Remove(InstanceId);
}

bool FUXWebviewHostClient::LaunchHost(double NowSeconds)
{
	FString Path;
	if (!FindHostExecutable(Path))
	{
		UE_LOG(LogUXWebviewHostClient, Error,
			TEXT("UXWebviewHost.exe not found; set UXWEBVIEW_HOST_EXE or build native/host"));
		return false;
	}

	// The host runs in its own process, so hand it absolute paths instead of
	// paths that depend on which directory the game was started from.
	const FString CacheDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UXWebviewHostCache")));
	const FString HostLogPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs/UXWebviewHost.log")));
	IFileManager::Get().MakeDirectory(*CacheDir, true);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(HostLogPath), true);

	++SessionCounter;
	const uint32 Pid = CurrentProcessId();
	HostPath = Path;
	PipeName = FString::Printf(TEXT("UXWebviewIPC-%u-%u"), Pid, SessionCounter);

	// Every instance needs a fresh shared memory section after a relaunch.
	ResetInstanceGeneration();

	const UUXWebviewSettings& Settings = UUXWebviewSettings::Get();
	const uint64 AdapterLuid = UXWebviewSharedTexture::GetAdapterLuid();

	FString Args = FString::Printf(
		TEXT("--ipc %s --cache %s --log %s --log-level 2 --owner-pid %u --idle-seconds %d --no-sandbox --luid 0x%llX"),
		*PipeName, *QuoteIfNeeded(CacheDir), *QuoteIfNeeded(HostLogPath), Pid,
		Settings.IdleShutdownSeconds, AdapterLuid);
	if (Settings.bEnableDevTools && Settings.DevToolsPort > 0)
	{
		Args += FString::Printf(TEXT(" --devtools-port %d"), Settings.DevToolsPort);
	}

	uint32 ProcessId = 0;
	HostProcess = FPlatformProcess::CreateProc(*HostPath, *Args, true, false, false,
		&ProcessId, 0, *FPaths::GetPath(HostPath), nullptr, nullptr);
	if (!HostProcess.IsValid())
	{
		UE_LOG(LogUXWebviewHostClient, Error, TEXT("failed to launch '%s'"), *HostPath);
		HostProcess.Reset();
		return false;
	}

	bPipeOpen = false;
	bHelloSent = false;
	bHelloAck = false;
	bExitHandled = false;
	ReadBuffer.Reset();
	NextConnectSeconds = NowSeconds;
	NextPingSeconds = NowSeconds;
	Recovery.OnLaunched(NowSeconds);

	UE_LOG(LogUXWebviewHostClient, Log,
		TEXT("host launched pid=%u luid=0x%llX args='%s'"), ProcessId, AdapterLuid, *Args);
	return true;
}

void FUXWebviewHostClient::TerminateHost()
{
	if (HostProcess.IsValid())
	{
		if (FPlatformProcess::IsProcRunning(HostProcess))
		{
			UE_LOG(LogUXWebviewHostClient, Warning, TEXT("host did not exit, terminating it"));
			FPlatformProcess::TerminateProc(HostProcess, true);
		}
		FPlatformProcess::CloseProc(HostProcess);
		HostProcess.Reset();
	}
}

void FUXWebviewHostClient::TryConnect(double NowSeconds)
{
	const FString FullPipe = FString::Printf(TEXT("\\\\.\\pipe\\%s"), *PipeName);
	HANDLE Pipe = ::CreateFileW(*FullPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		OPEN_EXISTING, 0, nullptr);
	if (Pipe == INVALID_HANDLE_VALUE)
	{
		return;
	}

	PipeHandle = Pipe;
	bPipeOpen = true;
	bHelloSent = true;
	ReadBuffer.Reset();
	NextPingSeconds = NowSeconds;

	SendRaw(UXWV_IPC_HELLO, 0, 1, 0, 0, 0, TArray<uint8>());
	UE_LOG(LogUXWebviewHostClient, Log, TEXT("IPC connected to '%s'"), *PipeName);
}

bool FUXWebviewHostClient::Tick(float DeltaSeconds)
{
	(void)DeltaSeconds;

	if (bGiveUp)
	{
		return true;
	}

	const double Now = FPlatformTime::Seconds();

	// Process exit is the primary crash signal; the pipe close follows it.
	if (HostProcess.IsValid() && !FPlatformProcess::IsProcRunning(HostProcess))
	{
		HandleProcessExit(Now, "host process exited");
	}

	const EUXWebviewHostRecoveryAction Action = Recovery.Tick(Now);
	if (Action == EUXWebviewHostRecoveryAction::Relaunch)
	{
		TerminateHost();
		if (LaunchHost(Now))
		{
			UE_LOG(LogUXWebviewHostClient, Log, TEXT("host relaunch %d"), Recovery.GetRestartCount());
		}
		else
		{
			Recovery.OnExited(Now);
		}
	}
	else if (Action == EUXWebviewHostRecoveryAction::GiveUp)
	{
		bGiveUp = true;
		UE_LOG(LogUXWebviewHostClient, Error,
			TEXT("host gave up after %d restarts"), Recovery.GetRestartCount());
		NotifyAllCrashed(TEXT("the CEF host could not be restarted"));
		return true;
	}

	// A heartbeat timeout moved us into recovery while the process lingers.
	if (Recovery.IsRecovering() && HostProcess.IsValid() && FPlatformProcess::IsProcRunning(HostProcess))
	{
		HandleProcessExit(Now, "host heartbeat timed out");
	}

	if (Recovery.GetState() == EUXWebviewHostRecoveryState::Starting && !bHelloAck)
	{
		if (Now >= NextConnectSeconds)
		{
			NextConnectSeconds = Now + kConnectRetrySeconds;
			TryConnect(Now);
		}
	}

	if (bPipeOpen)
	{
		PollPipe();

		if (bHelloAck && Now >= NextPingSeconds)
		{
			NextPingSeconds = Now + kPingIntervalSeconds;
			SendRaw(UXWV_IPC_PING, 0, 0, 0, 0, 0, TArray<uint8>());
		}
	}

	return true;
}

bool FUXWebviewHostClient::PollPipe()
{
	if (!bPipeOpen || !PipeHandle)
	{
		return true;
	}

	DWORD Available = 0;
	if (!::PeekNamedPipe(static_cast<HANDLE>(PipeHandle), nullptr, 0, nullptr, &Available, nullptr))
	{
		HandleDisconnect();
		return false;
	}
	if (Available == 0)
	{
		return true;
	}

	const int32 Start = ReadBuffer.Num();
	ReadBuffer.SetNumUninitialized(Start + static_cast<int32>(Available));

	DWORD Read = 0;
	if (!::ReadFile(static_cast<HANDLE>(PipeHandle), ReadBuffer.GetData() + Start,
			Available, &Read, nullptr) || Read == 0)
	{
		ReadBuffer.SetNum(Start);
		HandleDisconnect();
		return false;
	}
	ReadBuffer.SetNum(Start + static_cast<int32>(Read));

	while (ReadBuffer.Num() >= static_cast<int32>(sizeof(UxIpcMessage)))
	{
		UxIpcMessage Header{};
		FMemory::Memcpy(&Header, ReadBuffer.GetData(), sizeof(UxIpcMessage));

		if (Header.ProtocolVersion != UXWV_IPC_VERSION || Header.Size > UXWV_IPC_MAX_PAYLOAD)
		{
			UE_LOG(LogUXWebviewHostClient, Warning,
				TEXT("malformed IPC head: version=%u size=%u"), Header.ProtocolVersion, Header.Size);
			HandleDisconnect();
			return false;
		}

		const int32 Total = static_cast<int32>(sizeof(UxIpcMessage)) + static_cast<int32>(Header.Size);
		if (ReadBuffer.Num() < Total)
		{
			break;
		}

		FUXWebviewIpcEnvelope Envelope;
		if (!UXWebviewDecodeIpcEnvelope(ReadBuffer.GetData(), Total, Envelope))
		{
			UE_LOG(LogUXWebviewHostClient, Warning, TEXT("failed to decode IPC message type=%u"), Header.Type);
			HandleDisconnect();
			return false;
		}

		ReadBuffer.RemoveAt(0, Total);
		Dispatch(Envelope);
	}

	return true;
}

void FUXWebviewHostClient::HandleDisconnect()
{
	if (PipeHandle)
	{
		::CloseHandle(static_cast<HANDLE>(PipeHandle));
		PipeHandle = nullptr;
	}
	bPipeOpen = false;
	bHelloSent = false;
	bHelloAck = false;

	if (!bExitHandled)
	{
		HandleProcessExit(FPlatformTime::Seconds(), "IPC connection closed");
	}
}

void FUXWebviewHostClient::HandleProcessExit(double NowSeconds, const char* Reason)
{
	if (bExitHandled)
	{
		return;
	}
	bExitHandled = true;

	UE_LOG(LogUXWebviewHostClient, Warning, TEXT("%s"), ANSI_TO_TCHAR(Reason));

	if (PipeHandle)
	{
		::CloseHandle(static_cast<HANDLE>(PipeHandle));
		PipeHandle = nullptr;
	}
	bPipeOpen = false;
	bHelloSent = false;
	bHelloAck = false;
	ReadBuffer.Reset();

	TerminateHost();

	NotifyAllCrashed(FString(Reason));

	// An idle host exits on purpose; only keep retrying while views need it.
	if (Instances.Num() > 0)
	{
		Recovery.OnExited(NowSeconds);
	}
	else
	{
		Recovery.Reset();
	}
}

void FUXWebviewHostClient::NotifyAllCrashed(const FString& Reason)
{
	// Callbacks may destroy instances, so collect the targets first.
	struct FTarget
	{
		uint32 Id = 0;
		IUXWebviewHostInstanceClient* Client = nullptr;
	};
	TArray<FTarget> Targets;
	Instances.ForEach([&Targets](uint32 Id, FInstance& Instance)
	{
		Targets.Add(FTarget{ Id, Instance.Client });
	});

	for (const FTarget& Target : Targets)
	{
		if (FInstance* Instance = Instances.Find(Target.Id))
		{
			Instance->bReady = false;
			Instance->bRecoverPending = true;
			Instance->bCreateSent = false;
		}
		if (Target.Client)
		{
			Target.Client->OnHostCrash(Reason);
		}
	}
}

void FUXWebviewHostClient::ResetInstanceGeneration()
{
	const uint32 Pid = CurrentProcessId();
	Instances.ForEach([Pid, this](uint32 Id, FInstance& Instance)
	{
		Instance.SharedMemoryName = FString::Printf(TEXT("UXWebviewFrame-%u-%u-%u"),
			Pid, SessionCounter, Id);
		Instance.bCreateSent = false;
		Instance.bReady = false;
	});
}

void FUXWebviewHostClient::FlushPendingCreates()
{
	Instances.ForEach([this](uint32 Id, FInstance& Instance)
	{
		if (Instance.bCreateSent)
		{
			return;
		}

		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("shmem"), Instance.SharedMemoryName);
		Object->SetNumberField(TEXT("width"), Instance.Spec.Size.X);
		Object->SetNumberField(TEXT("height"), Instance.Spec.Size.Y);
		Object->SetNumberField(TEXT("scale"), Instance.Spec.DpiScale);
		Object->SetNumberField(TEXT("fps"), Instance.Spec.FrameRate);
		Object->SetBoolField(TEXT("visible"), Instance.Spec.bVisible);
		Object->SetStringField(TEXT("url"), Instance.Spec.Url);

		TArray<TSharedPtr<FJsonValue>> Origins;
		for (const FString& Origin : UUXWebviewSettings::Get().BridgeAllowedOrigins)
		{
			Origins.Add(MakeShared<FJsonValueString>(Origin));
		}
		Object->SetArrayField(TEXT("origins"), Origins);

		SendJson(UXWV_IPC_CREATE, Id, Id, 0, 0, SerializeJson(Object));
		Instance.bCreateSent = true;
	});
}

void FUXWebviewHostClient::Dispatch(const FUXWebviewIpcEnvelope& Envelope)
{
	if (Envelope.Type == UXWV_IPC_HELLO_ACK)
	{
		if (Envelope.A != UXWV_IPC_VERSION)
		{
			UE_LOG(LogUXWebviewHostClient, Error,
				TEXT("host protocol %u does not match plugin protocol %u"),
				Envelope.A, UXWV_IPC_VERSION);
			HandleDisconnect();
			return;
		}

		bHelloAck = true;
		Recovery.OnConnected(FPlatformTime::Seconds());
		NextPingSeconds = FPlatformTime::Seconds() + kPingIntervalSeconds;
		UE_LOG(LogUXWebviewHostClient, Log, TEXT("host handshake accepted"));
		FlushPendingCreates();
		return;
	}

	if (Envelope.Type == UXWV_IPC_PONG)
	{
		Recovery.OnHeartbeatPong(FPlatformTime::Seconds());
		return;
	}

	FInstance* Instance = Instances.Find(Envelope.Instance);
	if (!Instance)
	{
		UE_LOG(LogUXWebviewHostClient, Verbose,
			TEXT("dropping message type %u for unknown instance %u"),
			Envelope.Type, Envelope.Instance);
		return;
	}

	IUXWebviewHostInstanceClient* Client = Instance->Client;
	const TSharedPtr<FJsonObject> Object = ParseJsonObject(Envelope.Payload);

	switch (Envelope.Type)
	{
	case UXWV_IPC_CREATE_ACK:
	{
		const bool bOk = JsonBool(Object, TEXT("ok"), false);
		if (!bOk && Client)
		{
			Instance->bCreateSent = false;
			Client->OnHostCrash(TEXT("the host rejected the browser creation"));
		}
		break;
	}
	case UXWV_IPC_EVENT_READY:
	{
		const bool bRecovered = Instance->bRecoverPending;
		Instance->bReady = true;
		Instance->bRecoverPending = false;
		if (Client)
		{
			if (bRecovered)
			{
				Client->OnHostRecovered();
			}
			else
			{
				Client->OnHostReady();
			}
		}
		break;
	}
	case UXWV_IPC_EVENT_LOAD_START:
		if (Client)
		{
			Client->OnHostLoadStart();
		}
		break;
	case UXWV_IPC_EVENT_LOAD_END:
		if (Client)
		{
			Client->OnHostLoadEnd(JsonString(Object, TEXT("url")),
				JsonInt(Object, TEXT("code"), 0));
		}
		break;
	case UXWV_IPC_EVENT_TITLE:
		if (Client)
		{
			Client->OnHostTitleChanged(JsonString(Object, TEXT("title")));
		}
		break;
	case UXWV_IPC_EVENT_URL:
		if (Client)
		{
			Client->OnHostUrlChanged(JsonString(Object, TEXT("url")));
		}
		break;
	case UXWV_IPC_EVENT_CRASH:
	{
		Instance->bReady = false;
		if (Client)
		{
			Client->OnHostCrash(JsonString(Object, TEXT("reason")));
		}
		break;
	}
	case UXWV_IPC_EXEC_RESULT:
		if (Client)
		{
			Client->OnHostScriptResult(
				static_cast<uint32>(JsonInt(Object, TEXT("request"), 0)),
				JsonBool(Object, TEXT("success"), false),
				JsonString(Object, TEXT("result")));
		}
		break;
	case UXWV_IPC_EVENT_BRIDGE:
		if (Client)
		{
			Client->OnHostBridgeMessage(JsonString(Object, TEXT("name")),
				JsonString(Object, TEXT("payload")),
				static_cast<uint32>(JsonInt(Object, TEXT("request"), 0)));
		}
		break;
	case UXWV_IPC_EVENT_DEVTOOLS:
		if (Client)
		{
			Client->OnHostDevTools(JsonString(Object, TEXT("url")));
		}
		break;
	default:
		UE_LOG(LogUXWebviewHostClient, Verbose,
			TEXT("unhandled host message type %u for instance %u"),
			Envelope.Type, Envelope.Instance);
		break;
	}
}

bool FUXWebviewHostClient::SendRaw(uint32 Type, uint32 InstanceId, uint32 RequestId,
	uint32 A, uint32 B, uint32 Flags, const TArray<uint8>& Payload)
{
	if (!bPipeOpen || !PipeHandle)
	{
		return false;
	}

	TArray<uint8> Bytes;
	if (!UXWebviewEncodeIpcMessage(Type, InstanceId, RequestId, A, B, Flags, Payload, Bytes))
	{
		UE_LOG(LogUXWebviewHostClient, Error,
			TEXT("refusing to send IPC type %u with a %d byte payload"), Type, Payload.Num());
		return false;
	}

	DWORD Written = 0;
	if (!::WriteFile(static_cast<HANDLE>(PipeHandle), Bytes.GetData(),
			static_cast<DWORD>(Bytes.Num()), &Written, nullptr)
		|| Written != static_cast<DWORD>(Bytes.Num()))
	{
		UE_LOG(LogUXWebviewHostClient, Warning, TEXT("IPC write failed, type=%u"), Type);
		HandleDisconnect();
		return false;
	}
	return true;
}

bool FUXWebviewHostClient::SendJson(uint32 Type, uint32 InstanceId, uint32 RequestId,
	uint32 A, uint32 B, const FString& Json)
{
	return SendRaw(Type, InstanceId, RequestId, A, B, 0, StringToPayload(Json));
}

FUXWebviewResult FUXWebviewHostClient::Navigate(uint32 InstanceId, const FString& Url)
{
	if (!IsInstanceReady(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host instance is not ready"));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("url"), Url);
	return SendJson(UXWV_IPC_NAVIGATE, InstanceId, 0, 0, 0, SerializeJson(Object))
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed, TEXT("IPC write failed"));
}

FUXWebviewResult FUXWebviewHostClient::LoadHtml(uint32 InstanceId, const FString& Html, const FString& BaseUrl)
{
	if (!IsInstanceReady(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host instance is not ready"));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("html"), Html);
	Object->SetStringField(TEXT("base"), BaseUrl);
	return SendJson(UXWV_IPC_LOAD_HTML, InstanceId, 0, 0, 0, SerializeJson(Object))
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed, TEXT("IPC write failed"));
}

FUXWebviewResult FUXWebviewHostClient::Reload(uint32 InstanceId)
{
	if (!IsInstanceReady(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host instance is not ready"));
	}
	return SendRaw(UXWV_IPC_RELOAD, InstanceId, 0, 0, 0, 0, TArray<uint8>())
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed, TEXT("IPC write failed"));
}

FUXWebviewResult FUXWebviewHostClient::StopLoading(uint32 InstanceId)
{
	if (!IsInstanceReady(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host instance is not ready"));
	}
	return SendRaw(UXWV_IPC_STOP, InstanceId, 0, 0, 0, 0, TArray<uint8>())
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed, TEXT("IPC write failed"));
}

FUXWebviewResult FUXWebviewHostClient::Resize(uint32 InstanceId, const FIntPoint& Size, float DpiScale)
{
	if (!Instances.Contains(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotFound, TEXT("unknown host instance"));
	}

	FInstance* Instance = Instances.Find(InstanceId);
	Instance->Spec.Size = Size;
	Instance->Spec.DpiScale = DpiScale;

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("scale"), DpiScale);
	return SendJson(UXWV_IPC_RESIZE, InstanceId, 0, static_cast<uint32>(Size.X),
		static_cast<uint32>(Size.Y), SerializeJson(Object))
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host is not connected"));
}

FUXWebviewResult FUXWebviewHostClient::SetVisible(uint32 InstanceId, bool bVisible)
{
	FInstance* Instance = Instances.Find(InstanceId);
	if (!Instance)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotFound, TEXT("unknown host instance"));
	}
	Instance->Spec.bVisible = bVisible;
	return SendRaw(UXWV_IPC_SET_VISIBLE, InstanceId, 0, bVisible ? 1 : 0, 0, 0, TArray<uint8>())
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host is not connected"));
}

FUXWebviewResult FUXWebviewHostClient::SetFrameRate(uint32 InstanceId, int32 FramesPerSecond)
{
	FInstance* Instance = Instances.Find(InstanceId);
	if (!Instance)
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotFound, TEXT("unknown host instance"));
	}
	Instance->Spec.FrameRate = FramesPerSecond;
	return SendRaw(UXWV_IPC_SET_FRAME_RATE, InstanceId, 0,
		static_cast<uint32>(FMath::Max(FramesPerSecond, 0)), 0, 0, TArray<uint8>())
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host is not connected"));
}

FUXWebviewResult FUXWebviewHostClient::SetFocus(uint32 InstanceId, bool bFocused)
{
	if (!Instances.Contains(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotFound, TEXT("unknown host instance"));
	}
	return SendRaw(UXWV_IPC_FOCUS, InstanceId, 0, bFocused ? 1 : 0, 0, 0, TArray<uint8>())
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host is not connected"));
}

FUXWebviewResult FUXWebviewHostClient::BeginFrame(uint32 InstanceId)
{
	if (!IsInstanceReady(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host instance is not ready"));
	}
	return SendRaw(UXWV_IPC_BEGIN_FRAME, InstanceId, 0, 0, 0, 0, TArray<uint8>())
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed, TEXT("IPC write failed"));
}

FUXWebviewResult FUXWebviewHostClient::SendInput(uint32 InstanceId, const FUXWebviewInputEvent& Event)
{
	if (!IsInstanceReady(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host instance is not ready"));
	}

	TArray<uint8> Payload;
	if (!UXWebviewEncodeInputEvent(Event, Payload))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::InvalidArgs, TEXT("input event is malformed"));
	}

	return SendRaw(UXWV_IPC_INPUT, InstanceId, 0, 0, 0, 0, Payload)
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed, TEXT("IPC write failed"));
}

FUXWebviewResult FUXWebviewHostClient::ExecuteScript(uint32 InstanceId, uint32 RequestId, const FString& Script)
{
	if (!IsInstanceReady(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host instance is not ready"));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("script"), Script);
	return SendJson(UXWV_IPC_EXEC, InstanceId, RequestId, 0, 0, SerializeJson(Object))
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed, TEXT("IPC write failed"));
}

FUXWebviewResult FUXWebviewHostClient::EmitToWeb(uint32 InstanceId, const FString& Event,
	const FString& PayloadJson)
{
	if (!IsInstanceReady(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host instance is not ready"));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("event"), Event);
	Object->SetStringField(TEXT("payload"), PayloadJson);
	return SendJson(UXWV_IPC_EMIT, InstanceId, 0, 0, 0, SerializeJson(Object))
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed, TEXT("IPC write failed"));
}

FUXWebviewResult FUXWebviewHostClient::ReplyBridge(uint32 InstanceId, uint32 RequestId,
	bool bSuccess, const FString& Result)
{
	if (!Instances.Contains(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotFound, TEXT("unknown host instance"));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("request"), static_cast<double>(RequestId));
	Object->SetBoolField(TEXT("success"), bSuccess);
	Object->SetStringField(TEXT("result"), Result);
	return SendJson(UXWV_IPC_BRIDGE_RESULT, InstanceId, RequestId, 0, 0, SerializeJson(Object))
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::HostCrashed, TEXT("IPC write failed"));
}

FUXWebviewResult FUXWebviewHostClient::RequestDevTools(uint32 InstanceId)
{
	if (!Instances.Contains(InstanceId))
	{
		return FUXWebviewResult::Fail(EUXWebviewResultCode::NotFound, TEXT("unknown host instance"));
	}
	return SendRaw(UXWV_IPC_REQUEST_DEVTOOLS, InstanceId, 0, 0, 0, 0, TArray<uint8>())
		? FUXWebviewResult::Ok()
		: FUXWebviewResult::Fail(EUXWebviewResultCode::NotReady, TEXT("host is not connected"));
}

FString FUXWebviewHostClient::GetSharedMemoryName(uint32 InstanceId) const
{
	const FInstance* Instance = Instances.Find(InstanceId);
	return Instance ? Instance->SharedMemoryName : FString();
}

bool FUXWebviewHostClient::IsInstanceReady(uint32 InstanceId) const
{
	const FInstance* Instance = Instances.Find(InstanceId);
	return Instance && Instance->bReady;
}

void FUXWebviewHostClient::Shutdown()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (bPipeOpen)
	{
		SendRaw(UXWV_IPC_SHUTDOWN, 0, 0, 0, 0, 0, TArray<uint8>());
		const double Deadline = FPlatformTime::Seconds() + kShutdownWaitSeconds;
		while (HostProcess.IsValid() && FPlatformProcess::IsProcRunning(HostProcess)
			&& FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.02f);
		}
	}

	if (PipeHandle)
	{
		::CloseHandle(static_cast<HANDLE>(PipeHandle));
		PipeHandle = nullptr;
	}
	bPipeOpen = false;
	bHelloSent = false;
	bHelloAck = false;

	TerminateHost();
	Instances.Clear();
	Recovery.Reset();
	bExitHandled = false;
	bGiveUp = false;
	ReadBuffer.Reset();
}

#endif // PLATFORM_WINDOWS
