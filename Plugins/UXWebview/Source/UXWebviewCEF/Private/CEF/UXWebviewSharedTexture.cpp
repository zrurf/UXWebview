#include "CEF/UXWebviewSharedTexture.h"

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "ID3D11DynamicRHI.h"
#include "ID3D12DynamicRHI.h"

DEFINE_LOG_CATEGORY_STATIC(LogUXWebviewSharedTexture, Log, All);

namespace UXWebviewSharedTexture
{
uint64 GetAdapterLuid()
{
	ID3D12DynamicRHI* RHI = IsRHID3D12() ? GetID3D12DynamicRHI() : nullptr;
	ID3D12Device* Device = RHI ? RHI->RHIGetDevice_NoMGPU() : nullptr;
	if (!Device)
	{
		return 0;
	}

	// IDXGIDevice is not exposed by D3D12 devices, so read the LUID directly.
	const LUID Luid = Device->GetAdapterLuid();
	return (static_cast<uint64>(Luid.HighPart) << 32) | Luid.LowPart;
}

static FTextureRHIRef OpenD3D12(uint64 HandleValue)
{
	ID3D12DynamicRHI* RHI = GetID3D12DynamicRHI();
	ID3D12Device* Device = RHI ? RHI->RHIGetDevice_NoMGPU() : nullptr;
	if (!Device)
	{
		UE_LOG(LogUXWebviewSharedTexture, Error, TEXT("D3D12 device is null"));
		return nullptr;
	}

	ID3D12Resource* Resource = nullptr;
	const HRESULT Hr = Device->OpenSharedHandle(
		reinterpret_cast<HANDLE>(HandleValue), IID_PPV_ARGS(&Resource));
	if (FAILED(Hr) || Resource == nullptr)
	{
		UE_LOG(LogUXWebviewSharedTexture, Error,
			TEXT("D3D12 OpenSharedHandle failed, hr=0x%08X"), static_cast<uint32>(Hr));
		return nullptr;
	}

	FTextureRHIRef Wrapped = RHI->RHICreateTexture2DFromResource(
		PF_B8G8R8A8, TexCreate_ShaderResource, FClearValueBinding::None, Resource);
	Resource->Release();
	return Wrapped;
}

static FTextureRHIRef OpenD3D11(uint64 HandleValue)
{
	ID3D11DynamicRHI* RHI = GetID3D11DynamicRHI();
	ID3D11Device* Device = RHI ? RHI->RHIGetDevice() : nullptr;
	if (!Device)
	{
		UE_LOG(LogUXWebviewSharedTexture, Error, TEXT("D3D11 device is null"));
		return nullptr;
	}

	ID3D11Device1* Device1 = nullptr;
	const HRESULT QueryHr = Device->QueryInterface(IID_PPV_ARGS(&Device1));
	if (FAILED(QueryHr) || Device1 == nullptr)
	{
		UE_LOG(LogUXWebviewSharedTexture, Error,
			TEXT("ID3D11Device1 unavailable, hr=0x%08X"), static_cast<uint32>(QueryHr));
		return nullptr;
	}

	ID3D11Texture2D* Resource = nullptr;
	const HRESULT OpenHr = Device1->OpenSharedResource1(
		reinterpret_cast<HANDLE>(HandleValue), IID_PPV_ARGS(&Resource));
	Device1->Release();
	if (FAILED(OpenHr) || Resource == nullptr)
	{
		UE_LOG(LogUXWebviewSharedTexture, Error,
			TEXT("D3D11 OpenSharedResource1 failed, hr=0x%08X"), static_cast<uint32>(OpenHr));
		return nullptr;
	}

	FTextureRHIRef Wrapped = RHI->RHICreateTexture2DFromResource(
		PF_B8G8R8A8, TexCreate_ShaderResource, FClearValueBinding::None, Resource);
	Resource->Release();
	return Wrapped;
}

FTextureRHIRef Open(uint64 HandleValue)
{
	if (HandleValue == 0)
	{
		return nullptr;
	}
	if (IsRHID3D12())
	{
		return OpenD3D12(HandleValue);
	}
	if (IsRHID3D11())
	{
		return OpenD3D11(HandleValue);
	}
	UE_LOG(LogUXWebviewSharedTexture, Error, TEXT("CEF provider requires a D3D12 or D3D11 RHI"));
	return nullptr;
}
} // namespace UXWebviewSharedTexture

#endif // PLATFORM_WINDOWS
