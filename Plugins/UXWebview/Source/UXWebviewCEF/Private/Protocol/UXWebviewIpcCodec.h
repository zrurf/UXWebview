#pragma once

#include "CoreMinimal.h"

#include <cstring>

#include "UXWebviewTypes.h"
#include "uxwebview_ipc.h"

static_assert(sizeof(UxIpcMessage) == 32, "IPC header layout changed");
static_assert(sizeof(UxInputEvent) == 32, "input event layout changed");

/** One decoded IPC message; payload is UTF-8 JSON or binary input bytes. */
struct FUXWebviewIpcEnvelope
{
	uint32 Type = 0;
	uint32 ProtocolVersion = UXWV_IPC_VERSION;
	uint32 Instance = 0;
	uint32 RequestId = 0;
	uint32 A = 0;
	uint32 B = 0;
	uint32 Flags = 0;
	TArray<uint8> Payload;

	bool IsResponse() const { return (Flags & UXWV_IPC_FLAG_RESPONSE) != 0; }
};

/** Serializes a header plus payload. Fails when the payload is too large. */
inline bool UXWebviewEncodeIpcMessage(
	uint32 Type, uint32 Instance, uint32 RequestId,
	uint32 A, uint32 B, uint32 Flags,
	const TArray<uint8>& Payload, TArray<uint8>& Out)
{
	if (Payload.Num() > static_cast<int32>(UXWV_IPC_MAX_PAYLOAD))
	{
		return false;
	}

	UxIpcMessage Header{};
	Header.Size = static_cast<uint32>(Payload.Num());
	Header.Type = Type;
	Header.ProtocolVersion = UXWV_IPC_VERSION;
	Header.Instance = Instance;
	Header.RequestId = RequestId;
	Header.A = A;
	Header.B = B;
	Header.Flags = Flags;

	Out.SetNumUninitialized(sizeof(UxIpcMessage) + Payload.Num());
	std::memcpy(Out.GetData(), &Header, sizeof(UxIpcMessage));
	if (Payload.Num() > 0)
	{
		std::memcpy(Out.GetData() + sizeof(UxIpcMessage), Payload.GetData(), Payload.Num());
	}
	return true;
}

/** Decodes one complete message; truncated or oversized input is rejected. */
inline bool UXWebviewDecodeIpcEnvelope(
	const uint8* Data, int32 Length, FUXWebviewIpcEnvelope& Out)
{
	if (!Data || Length < static_cast<int32>(sizeof(UxIpcMessage)))
	{
		return false;
	}

	UxIpcMessage Header{};
	std::memcpy(&Header, Data, sizeof(UxIpcMessage));
	if (Header.ProtocolVersion != UXWV_IPC_VERSION)
	{
		return false;
	}
	if (Header.Size > UXWV_IPC_MAX_PAYLOAD)
	{
		return false;
	}
	if (Length != static_cast<int32>(sizeof(UxIpcMessage)) + static_cast<int32>(Header.Size))
	{
		return false;
	}

	Out.Type = Header.Type;
	Out.ProtocolVersion = Header.ProtocolVersion;
	Out.Instance = Header.Instance;
	Out.RequestId = Header.RequestId;
	Out.A = Header.A;
	Out.B = Header.B;
	Out.Flags = Header.Flags;
	Out.Payload.SetNumUninitialized(static_cast<int32>(Header.Size));
	if (Header.Size > 0)
	{
		std::memcpy(Out.Payload.GetData(), Data + sizeof(UxIpcMessage), Header.Size);
	}
	return true;
}

/** Maps the UE enum to the wire enum shared with the host. */
inline uint32 UXWebviewToWireInputType(EUXWebviewInputType Type)
{
	switch (Type)
	{
	case EUXWebviewInputType::MouseMove: return UXWV_INPUT_MOUSE_MOVE;
	case EUXWebviewInputType::MouseDown: return UXWV_INPUT_MOUSE_DOWN;
	case EUXWebviewInputType::MouseUp: return UXWV_INPUT_MOUSE_UP;
	case EUXWebviewInputType::MouseWheel: return UXWV_INPUT_MOUSE_WHEEL;
	case EUXWebviewInputType::KeyDown: return UXWV_INPUT_KEY_DOWN;
	case EUXWebviewInputType::KeyUp: return UXWV_INPUT_KEY_UP;
	case EUXWebviewInputType::Char: return UXWV_INPUT_CHAR;
	case EUXWebviewInputType::ImeComposition: return UXWV_INPUT_IME_COMPOSITION;
	case EUXWebviewInputType::ImeCommit: return UXWV_INPUT_IME_COMMIT;
	case EUXWebviewInputType::ImeCancel: return UXWV_INPUT_IME_CANCEL;
	default: return UXWV_INPUT_MOUSE_MOVE;
	}
}

/** Compact binary input payload: fixed UxInputEvent plus optional UTF-8 text. */
inline bool UXWebviewEncodeInputEvent(const FUXWebviewInputEvent& Event, TArray<uint8>& Out)
{
	FString Utf8Source = Event.Text;
	const FTCHARToUTF8 Utf8(*Utf8Source);
	const int32 TextBytes = FMath::Min(Utf8.Length(), static_cast<int32>(UXWV_IPC_MAX_INPUT_TEXT));

	UxInputEvent Wire{};
	Wire.Type = UXWebviewToWireInputType(Event.Type);
	Wire.Button = static_cast<uint32>(FMath::Max(Event.Button, 0));
	Wire.KeyCode = static_cast<uint32>(FMath::Max(Event.KeyCode, 0));
	Wire.Modifiers = (Event.bShift ? UXWV_MOD_SHIFT : 0)
		| (Event.bControl ? UXWV_MOD_CTRL : 0)
		| (Event.bAlt ? UXWV_MOD_ALT : 0);
	Wire.X = static_cast<float>(Event.Position.X);
	Wire.Y = static_cast<float>(Event.Position.Y);
	Wire.WheelDelta = Event.WheelDelta;
	Wire.TextBytes = static_cast<uint32>(TextBytes);

	Out.SetNumUninitialized(sizeof(UxInputEvent) + TextBytes);
	std::memcpy(Out.GetData(), &Wire, sizeof(UxInputEvent));
	if (TextBytes > 0)
	{
		std::memcpy(Out.GetData() + sizeof(UxInputEvent), Utf8.Get(), TextBytes);
	}
	return true;
}

/** Reverse of UXWebviewEncodeInputEvent; used by tests and diagnostics. */
inline bool UXWebviewDecodeInputEvent(
	const TArray<uint8>& Payload, UxInputEvent& OutEvent, FString& OutText)
{
	if (Payload.Num() < static_cast<int32>(sizeof(UxInputEvent)))
	{
		return false;
	}

	std::memcpy(&OutEvent, Payload.GetData(), sizeof(UxInputEvent));
	const int32 Available = Payload.Num() - static_cast<int32>(sizeof(UxInputEvent));
	if (static_cast<int32>(OutEvent.TextBytes) > Available
		|| OutEvent.TextBytes > UXWV_IPC_MAX_INPUT_TEXT)
	{
		return false;
	}

	if (OutEvent.TextBytes > 0)
	{
		// The bytes were written by FTCHARToUTF8, so interpret them as UTF-8.
		// They are not null terminated inside the payload, so terminate a copy
		// instead of letting the converter read past the message.
		TArray<uint8> Utf8(Payload.GetData() + sizeof(UxInputEvent), static_cast<int32>(OutEvent.TextBytes));
		Utf8.Add(0);
		OutText = FString(UTF8_TO_TCHAR(Utf8.GetData()));
	}
	else
	{
		OutText.Reset();
	}
	return true;
}
