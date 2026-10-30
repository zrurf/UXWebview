#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS

#include "RHI.h"

/**
 * Windows shared texture plumbing for the CEF provider: adapter LUID for the
 * host handshake and NT handle opening on the render thread.
 */
namespace UXWebviewSharedTexture
{
	/** LUID of the RHI device as a 64 bit value; 0 when no D3D device exists. */
	uint64 GetAdapterLuid();

	/** Opens an NT shared handle and wraps it as an RHI texture. Render thread only. */
	FTextureRHIRef Open(uint64 HandleValue);
}

#endif // PLATFORM_WINDOWS
