#pragma once

#include "CoreMinimal.h"

#include "UXWebviewTypes.generated.h"

/** Lifecycle state of a webview instance. */
UENUM(BlueprintType)
enum class EUXWebviewState : uint8
{
	Created UMETA(DisplayName = "Created"),
	Initializing UMETA(DisplayName = "Initializing"),
	Ready UMETA(DisplayName = "Ready"),
	Navigating UMETA(DisplayName = "Navigating"),
	Suspended UMETA(DisplayName = "Suspended"),
	Crashed UMETA(DisplayName = "Crashed"),
	Destroyed UMETA(DisplayName = "Destroyed")
};

/** Error codes returned by every public call. */
UENUM(BlueprintType)
enum class EUXWebviewResultCode : uint8
{
	Ok UMETA(DisplayName = "Ok"),
	InvalidState UMETA(DisplayName = "InvalidState"),
	NotReady UMETA(DisplayName = "NotReady"),
	NotFound UMETA(DisplayName = "NotFound"),
	NotSupported UMETA(DisplayName = "NotSupported"),
	HostCrashed UMETA(DisplayName = "HostCrashed"),
	Timeout UMETA(DisplayName = "Timeout"),
	InvalidArgs UMETA(DisplayName = "InvalidArgs"),
	BridgeDenied UMETA(DisplayName = "BridgeDenied"),
	Unknown UMETA(DisplayName = "Unknown")
};

USTRUCT(BlueprintType)
struct UXWEBVIEW_API FUXWebviewResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	EUXWebviewResultCode Code = EUXWebviewResultCode::Unknown;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	FString Message;

	static FUXWebviewResult Ok()
	{
		FUXWebviewResult Result;
		Result.bSuccess = true;
		Result.Code = EUXWebviewResultCode::Ok;
		return Result;
	}

	static FUXWebviewResult Fail(EUXWebviewResultCode InCode, const FString& InMessage)
	{
		FUXWebviewResult Result;
		Result.Code = InCode;
		Result.Message = InMessage;
		return Result;
	}
};

/** What a provider can do. Values are fixed for the provider lifetime. */
USTRUCT(BlueprintType)
struct UXWEBVIEW_API FUXWebviewCapabilities
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	bool bGpuSharedTexture = false;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	bool bCpuPixelBuffer = false;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	bool bOverlayMode = false;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	bool bTransparency = false;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	bool bExternalBeginFrame = false;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	bool bDevTools = false;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	bool bOfflineCache = false;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	int32 MaxFrameRate = 60;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	FIntPoint MaxViewportSize = FIntPoint::ZeroValue;
};

/** Identifies a provider and whether its runtime is usable right now. */
USTRUCT(BlueprintType)
struct UXWEBVIEW_API FUXWebviewProviderInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	FString Version;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	FString Platform;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	bool bAvailable = false;

	UPROPERTY(BlueprintReadOnly, Category = "UXWebview")
	FString UnavailableReason;
};

UENUM(BlueprintType)
enum class EUXWebviewDisplayMode : uint8
{
	/** Off-screen rendering copied into a texture. */
	Texture UMETA(DisplayName = "Texture"),
	/** Native child window composited over the game. */
	Overlay UMETA(DisplayName = "Overlay")
};

/** Per-instance parameters; these never write back to global settings. */
USTRUCT(BlueprintType)
struct UXWEBVIEW_API FUXWebviewCreateParams
{
	GENERATED_BODY()

	/** Empty means "resolve from settings or platform default". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	FString ProviderName;

	/** Only matters when ProviderName is set and unavailable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	bool bAllowProviderFallback = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	EUXWebviewDisplayMode DisplayMode = EUXWebviewDisplayMode::Texture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	FIntPoint ViewportSize = FIntPoint(1280, 720);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	float DpiScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	int32 TargetFrameRate = 60;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	bool bTransparent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	FString InitialUrl;
};

/** Plain struct handed to providers; not exposed to Blueprint. */
struct FUXWebviewInitParams
{
	FIntPoint ViewportSize = FIntPoint(1280, 720);
	float DpiScale = 1.0f;
	int32 TargetFrameRate = 60;
	bool bTransparent = false;
	bool bOverlay = false;
	FString UserAgent;
	FString InitialUrl;
};

UENUM(BlueprintType)
enum class EUXWebviewInputType : uint8
{
	MouseMove,
	MouseDown,
	MouseUp,
	MouseWheel,
	KeyDown,
	KeyUp,
	Char,
	/** Interim IME text shown in the page; replaces the previous composition. */
	ImeComposition,
	/** Final IME text inserted into the page. */
	ImeCommit,
	/** Discards the current IME composition. */
	ImeCancel
};

/** Input event routed to the provider. Coordinates are in view CSS pixels. */
USTRUCT(BlueprintType)
struct UXWEBVIEW_API FUXWebviewInputEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	EUXWebviewInputType Type = EUXWebviewInputType::MouseMove;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	FVector2D Position = FVector2D::ZeroVector;

	/** 0 = left, 1 = right, 2 = middle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	int32 Button = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	float WheelDelta = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	int32 KeyCode = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	bool bShift = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	bool bControl = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	bool bAlt = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UXWebview")
	FString Text;
};
