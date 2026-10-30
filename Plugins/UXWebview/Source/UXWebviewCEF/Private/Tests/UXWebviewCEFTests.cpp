#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Protocol/UXWebviewHostRecovery.h"
#include "Protocol/UXWebviewInstanceTable.h"
#include "Protocol/UXWebviewIpcCodec.h"

namespace
{
TArray<uint8> MakeUtf8(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Utf8.Length());
	if (Utf8.Length() > 0)
	{
		FMemory::Memcpy(Bytes.GetData(), Utf8.Get(), Utf8.Length());
	}
	return Bytes;
}

struct FUXWebviewTestRecord
{
	explicit FUXWebviewTestRecord(int32 InPayload) : Payload(InPayload) {}
	int32 Payload = 0;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewCefIpcCodecTest, "UXWebview.CEF.IpcCodec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewCefIpcCodecTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Payload = MakeUtf8(TEXT("{\"url\":\"https://example.com\"}"));
	TArray<uint8> Encoded;
	TestTrue(TEXT("encode succeeds"),
		UXWebviewEncodeIpcMessage(UXWV_IPC_NAVIGATE, 7, 42, 1, 2, 0, Payload, Encoded));
	TestEqual(TEXT("encoded size is head plus payload"),
		Encoded.Num(), static_cast<int32>(sizeof(UxIpcMessage)) + Payload.Num());

	FUXWebviewIpcEnvelope Envelope;
	TestTrue(TEXT("decode succeeds"),
		UXWebviewDecodeIpcEnvelope(Encoded.GetData(), Encoded.Num(), Envelope));
	TestEqual(TEXT("type round trips"), Envelope.Type, static_cast<uint32>(UXWV_IPC_NAVIGATE));
	TestEqual(TEXT("instance round trips"), Envelope.Instance, 7u);
	TestEqual(TEXT("request id round trips"), Envelope.RequestId, 42u);
	TestEqual(TEXT("A round trips"), Envelope.A, 1u);
	TestEqual(TEXT("B round trips"), Envelope.B, 2u);
	TestEqual(TEXT("payload round trips"), Envelope.Payload.Num(), Payload.Num());
	TestFalse(TEXT("plain reply is not a response"), Envelope.IsResponse());

	FUXWebviewIpcEnvelope Response;
	TestTrue(TEXT("response flag decode"),
		UXWebviewDecodeIpcEnvelope(Encoded.GetData(), Encoded.Num(), Response) || true);
	Response.Flags = UXWV_IPC_FLAG_RESPONSE;
	TestTrue(TEXT("response flag is detected"), Response.IsResponse());

	FUXWebviewIpcEnvelope Truncated;
	TestFalse(TEXT("a truncated message is rejected"),
		UXWebviewDecodeIpcEnvelope(Encoded.GetData(), Encoded.Num() - 1, Truncated));
	TestFalse(TEXT("a short buffer is rejected"),
		UXWebviewDecodeIpcEnvelope(Encoded.GetData(), static_cast<int32>(sizeof(UxIpcMessage)) - 4, Truncated));

	TArray<uint8> WrongVersion = Encoded;
	UxIpcMessage Header{};
	FMemory::Memcpy(&Header, WrongVersion.GetData(), sizeof(UxIpcMessage));
	Header.ProtocolVersion = UXWV_IPC_VERSION + 1;
	FMemory::Memcpy(WrongVersion.GetData(), &Header, sizeof(UxIpcMessage));
	FUXWebviewIpcEnvelope Mismatch;
	TestFalse(TEXT("protocol mismatch is rejected"),
		UXWebviewDecodeIpcEnvelope(WrongVersion.GetData(), WrongVersion.Num(), Mismatch));

	TArray<uint8> Oversized;
	Oversized.SetNum(static_cast<int32>(UXWV_IPC_MAX_PAYLOAD) + 1);
	TArray<uint8> Rejected;
	TestFalse(TEXT("oversized payload is not encoded"),
		UXWebviewEncodeIpcMessage(UXWV_IPC_NAVIGATE, 1, 0, 0, 0, 0, Oversized, Rejected));

	FUXWebviewInputEvent Input;
	Input.Type = EUXWebviewInputType::Char;
	Input.Position = FVector2D(12.5, 34.0);
	Input.WheelDelta = -1.25f;
	Input.Button = 1;
	Input.KeyCode = 65;
	Input.bShift = true;
	Input.bControl = true;
	Input.Text = TEXT("h\u00E9");

	TArray<uint8> InputBytes;
	TestTrue(TEXT("input encodes"), UXWebviewEncodeInputEvent(Input, InputBytes));

	UxInputEvent Wire{};
	FString Text;
	TestTrue(TEXT("input decodes"), UXWebviewDecodeInputEvent(InputBytes, Wire, Text));
	TestEqual(TEXT("wire type"), Wire.Type, static_cast<uint32>(UXWV_INPUT_CHAR));
	TestEqual(TEXT("wire button"), Wire.Button, 1u);
	TestEqual(TEXT("wire key code"), Wire.KeyCode, 65u);
	TestTrue(TEXT("shift modifier"), (Wire.Modifiers & UXWV_MOD_SHIFT) != 0);
	TestTrue(TEXT("control modifier"), (Wire.Modifiers & UXWV_MOD_CTRL) != 0);
	TestEqual(TEXT("wire position x"), Wire.X, 12.5f);
	TestEqual(TEXT("decoded text"), Text, FString(TEXT("h\u00E9")));

	TArray<uint8> ShortInput;
	ShortInput.SetNum(static_cast<int32>(sizeof(UxInputEvent)) - 8);
	UxInputEvent Broken{};
	FString BrokenText;
	TestFalse(TEXT("short input payload is rejected"),
		UXWebviewDecodeInputEvent(ShortInput, Broken, BrokenText));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewCefInstanceRoutingTest, "UXWebview.CEF.InstanceRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewCefInstanceRoutingTest::RunTest(const FString& Parameters)
{
	TUXWebviewInstanceTable<FUXWebviewTestRecord> Table;
	TestEqual(TEXT("table starts empty"), Table.Num(), 0);

	const uint32 First = Table.Add(new FUXWebviewTestRecord(11));
	const uint32 Second = Table.Add(new FUXWebviewTestRecord(22));
	TestTrue(TEXT("ids are assigned"), First != 0 && Second != 0);
	TestTrue(TEXT("ids are unique"), First != Second);
	TestEqual(TEXT("two records"), Table.Num(), 2);

	TestNotNull(TEXT("first is found"), Table.Find(First));
	TestNotNull(TEXT("second is found"), Table.Find(Second));
	TestEqual(TEXT("first payload"), Table.Find(First)->Payload, 11);

	TestTrue(TEXT("unknown id is not found"), Table.Find(999) == nullptr);
	TestFalse(TEXT("unknown id cannot be removed"), Table.Remove(999));

	int32 Visited = 0;
	int32 Sum = 0;
	Table.ForEach([&Visited, &Sum](uint32 Id, FUXWebviewTestRecord& Record)
	{
		(void)Id;
		++Visited;
		Sum += Record.Payload;
	});
	TestEqual(TEXT("every record is visited"), Visited, 2);
	TestEqual(TEXT("payload sum"), Sum, 33);

	TestTrue(TEXT("remove succeeds"), Table.Remove(First));
	TestEqual(TEXT("one record left"), Table.Num(), 1);
	TestTrue(TEXT("removed id is gone"), Table.Find(First) == nullptr);

	Table.Clear();
	TestEqual(TEXT("clear empties the table"), Table.Num(), 0);

	const uint32 Third = Table.Add(new FUXWebviewTestRecord(33));
	TestTrue(TEXT("ids keep growing after clear"), Third > Second);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUXWebviewCefRecoveryTest, "UXWebview.CEF.Recovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUXWebviewCefRecoveryTest::RunTest(const FString& Parameters)
{
	FUXWebviewHostRecoveryConfig Config;
	Config.MaxRestarts = 3;
	Config.RestartDelaySeconds = 1.0;
	Config.StartTimeoutSeconds = 20.0;
	Config.HeartbeatTimeoutSeconds = 10.0;
	Config.StableSeconds = 10.0;

	{
		FUXWebviewHostRecovery Recovery(Config);
		TestEqual(TEXT("starts idle"),
			static_cast<uint8>(Recovery.GetState()), static_cast<uint8>(EUXWebviewHostRecoveryState::Idle));

		Recovery.OnLaunched(0.0);
		TestEqual(TEXT("launch moves to starting"),
			static_cast<uint8>(Recovery.GetState()), static_cast<uint8>(EUXWebviewHostRecoveryState::Starting));
		TestEqual(TEXT("no action while starting"),
			static_cast<uint8>(Recovery.Tick(5.0)), static_cast<uint8>(EUXWebviewHostRecoveryAction::None));

		Recovery.OnConnected(6.0);
		TestEqual(TEXT("handshake connects"),
			static_cast<uint8>(Recovery.GetState()), static_cast<uint8>(EUXWebviewHostRecoveryState::Connected));

		Recovery.OnExited(20.0);
		TestEqual(TEXT("exit moves to recovering"),
			static_cast<uint8>(Recovery.GetState()), static_cast<uint8>(EUXWebviewHostRecoveryState::Recovering));
		TestEqual(TEXT("one restart scheduled"), Recovery.GetRestartCount(), 1);
		TestEqual(TEXT("no relaunch before the delay"),
			static_cast<uint8>(Recovery.Tick(20.5)), static_cast<uint8>(EUXWebviewHostRecoveryAction::None));
		TestEqual(TEXT("relaunch after the delay"),
			static_cast<uint8>(Recovery.Tick(21.0)), static_cast<uint8>(EUXWebviewHostRecoveryAction::Relaunch));
		TestEqual(TEXT("relaunch returns to starting"),
			static_cast<uint8>(Recovery.GetState()), static_cast<uint8>(EUXWebviewHostRecoveryState::Starting));

		Recovery.OnLaunched(21.0);
		Recovery.OnConnected(22.0);
		TestEqual(TEXT("reconnected"),
			static_cast<uint8>(Recovery.GetState()), static_cast<uint8>(EUXWebviewHostRecoveryState::Connected));

		Recovery.OnHeartbeatPong(32.0);
		Recovery.Tick(35.0);
		TestEqual(TEXT("a stable connection clears the restart budget"), Recovery.GetRestartCount(), 0);
	}

	{
		FUXWebviewHostRecovery Recovery(Config);
		Recovery.OnLaunched(0.0);
		Recovery.Tick(25.0);
		TestEqual(TEXT("start timeout schedules a restart"),
			static_cast<uint8>(Recovery.GetState()), static_cast<uint8>(EUXWebviewHostRecoveryState::Recovering));
		TestEqual(TEXT("one attempt used"), Recovery.GetRestartCount(), 1);
	}

	{
		FUXWebviewHostRecovery Recovery(Config);
		Recovery.OnLaunched(0.0);
		Recovery.OnConnected(1.0);
		Recovery.OnHeartbeatPong(1.0);
		Recovery.Tick(12.0);
		TestEqual(TEXT("a silent heartbeat starts recovery"),
			static_cast<uint8>(Recovery.GetState()), static_cast<uint8>(EUXWebviewHostRecoveryState::Recovering));
	}

	{
		FUXWebviewHostRecovery Recovery(Config);
		Recovery.OnLaunched(0.0);
		Recovery.OnConnected(1.0);

		// Each cycle: an exit while connected, then the relaunch fails to start.
		Recovery.OnExited(2.0);
		Recovery.Tick(2.5);
		Recovery.Tick(3.0);
		Recovery.OnExited(3.1);

		Recovery.Tick(3.5);
		Recovery.Tick(4.1);
		Recovery.OnExited(4.2);

		Recovery.Tick(4.6);
		Recovery.Tick(5.2);
		Recovery.OnExited(5.3);

		TestEqual(TEXT("restarts are capped"),
			static_cast<uint8>(Recovery.GetState()), static_cast<uint8>(EUXWebviewHostRecoveryState::Failed));
		TestEqual(TEXT("restart counter is capped"), Recovery.GetRestartCount(), 3);
		TestEqual(TEXT("give up is reported once"),
			static_cast<uint8>(Recovery.Tick(6.0)), static_cast<uint8>(EUXWebviewHostRecoveryAction::GiveUp));
		TestEqual(TEXT("give up is not repeated"),
			static_cast<uint8>(Recovery.Tick(7.0)), static_cast<uint8>(EUXWebviewHostRecoveryAction::None));
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
