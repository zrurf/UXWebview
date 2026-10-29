#include "frame/FramePublisher.h"

#include "util/Log.h"

#include <atomic>
#include <chrono>

#if defined(_WIN32)
#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace uxwv
{
#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

namespace
{
constexpr int kMaxTextureSize = 4096;

// Blocks until the GPU finished the commands submitted so far. This is the
// T1 trade-off: the host waits instead of the game thread.
void WaitForGpu(ID3D11Device* device, ID3D11DeviceContext* context)
{
	ComPtr<ID3D11Query> query;
	D3D11_QUERY_DESC queryDesc{};
	queryDesc.Query = D3D11_QUERY_EVENT;
	if (FAILED(device->CreateQuery(&queryDesc, &query)))
	{
		context->Flush();
		return;
	}

	context->End(query.Get());
	context->Flush();

	HRESULT result = S_FALSE;
	while (result == S_FALSE)
	{
		result = context->GetData(query.Get(), nullptr, 0, 0);
	}
}
} // namespace

struct FramePublisher::Impl
{
	ComPtr<ID3D11Device> Device;
	ComPtr<ID3D11Device1> Device1;
	ComPtr<ID3D11DeviceContext> Context;

	ComPtr<ID3D11Texture2D> Pooled[UXWV_FRAME_POOL_SIZE];
	HANDLE SharedHandles[UXWV_FRAME_POOL_SIZE] = {nullptr, nullptr, nullptr};
	int PoolWidth = 0;
	int PoolHeight = 0;
	uint32_t Generation = 0;
	uint32_t NextBuffer = 0;

	HANDLE Mapping = nullptr;
	UxFrameHeader* Header = nullptr;

	uint32_t FrameCount = 0;
	int64_t LastSummaryTicks = 0;

	bool CreateDevice()
	{
		D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_11_0;
		HRESULT hr = D3D11CreateDevice(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			levels, _countof(levels), D3D11_SDK_VERSION,
			&Device, &achieved, &Context);
		if (FAILED(hr))
		{
			UXWV_LOG_ERROR("D3D11 device creation failed (0x%08X)", static_cast<unsigned>(hr));
			return false;
		}

		Device.As(&Device1);
		return Device1 != nullptr;
	}

	void ReleasePool()
	{
		for (auto& texture : Pooled)
		{
			texture.Reset();
		}
		for (auto& handle : SharedHandles)
		{
			if (handle)
			{
				CloseHandle(handle);
				handle = nullptr;
			}
		}
		PoolWidth = 0;
		PoolHeight = 0;
	}

	bool CreatePool(int width, int height)
	{
		ReleasePool();

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(width);
		desc.Height = static_cast<UINT>(height);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		// D3D11 rejects NT-handle sharing without a keyed mutex. The host
		// acquires the mutex before copying; D3D12 readers cannot acquire it
		// and rely on the published sequence instead (T1 trade-off).
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

		for (int i = 0; i < UXWV_FRAME_POOL_SIZE; ++i)
		{
			HRESULT hr = Device->CreateTexture2D(&desc, nullptr, &Pooled[i]);
			if (FAILED(hr) || Pooled[i] == nullptr)
			{
				UXWV_LOG_ERROR("failed to create pooled texture %d (%dx%d), hr=0x%08X",
					i, width, height, static_cast<unsigned>(hr));
				ReleasePool();
				return false;
			}

			ComPtr<IDXGIResource1> resource;
			Pooled[i].As(&resource);
			hr = resource ? resource->CreateSharedHandle(
				nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
				nullptr, &SharedHandles[i]) : E_NOINTERFACE;
			if (FAILED(hr))
			{
				UXWV_LOG_ERROR("failed to create shared handle for pooled texture %d, hr=0x%08X",
					i, static_cast<unsigned>(hr));
				ReleasePool();
				return false;
			}
		}

		PoolWidth = width;
		PoolHeight = height;
		++Generation;
		UXWV_LOG_INFO("frame pool created: %dx%d generation %u", width, height, Generation);
		return true;
	}

	void Publish(int bufferIndex, int width, int height)
	{
		if (!Header)
		{
			return;
		}

		Header->Width = static_cast<uint32_t>(width);
		Header->Height = static_cast<uint32_t>(height);
		Header->Format = UXWV_FORMAT_BGRA8;
		Header->PoolSize = UXWV_FRAME_POOL_SIZE;
		Header->Generation = Generation;
		Header->BufferIndex = static_cast<uint32_t>(bufferIndex);
		for (int i = 0; i < UXWV_FRAME_POOL_SIZE; ++i)
		{
			Header->SharedHandles[i] =
				static_cast<uint64_t>(reinterpret_cast<uintptr_t>(SharedHandles[i]));
		}

		std::atomic_thread_fence(std::memory_order_release);
		InterlockedIncrement(reinterpret_cast<volatile LONG*>(&Header->Seq));
		Header->PublishTicks = 0;

		++FrameCount;
		const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
		if (LastSummaryTicks == 0)
		{
			LastSummaryTicks = now;
		}
		else if (now - LastSummaryTicks >= 5'000'000'000LL)
		{
			UXWV_LOG_INFO("published %u frames in 5s, generation %u", FrameCount, Generation);
			FrameCount = 0;
			LastSummaryTicks = now;
		}
	}
};
#else
struct FramePublisher::Impl
{
};
#endif

FramePublisher::FramePublisher() : Data(new Impl())
{
}

FramePublisher::~FramePublisher()
{
	Stop();
	delete Data;
}

bool FramePublisher::Start(const std::string& sharedMemoryName)
{
#if defined(_WIN32)
	if (!Data->CreateDevice())
	{
		return false;
	}

	std::wstring name(sharedMemoryName.begin(), sharedMemoryName.end());
	Data->Mapping = CreateFileMappingW(
		INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, name.c_str());
	if (!Data->Mapping)
	{
		UXWV_LOG_ERROR("failed to create shared memory '%s'", sharedMemoryName.c_str());
		return false;
	}

	Data->Header = static_cast<UxFrameHeader*>(MapViewOfFile(
		Data->Mapping, FILE_MAP_ALL_ACCESS, 0, 0, 4096));
	if (!Data->Header)
	{
		UXWV_LOG_ERROR("failed to map shared memory '%s'", sharedMemoryName.c_str());
		return false;
	}

	*Data->Header = {};
	Data->Header->Magic = UXWV_FRAME_MAGIC;
	Data->Header->Version = UXWV_FRAME_VERSION;
	Data->Header->PoolSize = UXWV_FRAME_POOL_SIZE;
	UXWV_LOG_INFO("frame shared memory '%s' ready", sharedMemoryName.c_str());
	return true;
#else
	(void)sharedMemoryName;
	UXWV_LOG_WARN("FramePublisher is not implemented on this platform yet");
	return false;
#endif
}

void FramePublisher::Stop()
{
#if defined(_WIN32)
	Data->ReleasePool();

	if (Data->Header)
	{
		UnmapViewOfFile(Data->Header);
		Data->Header = nullptr;
	}
	if (Data->Mapping)
	{
		CloseHandle(Data->Mapping);
		Data->Mapping = nullptr;
	}
#endif
}

void FramePublisher::PublishFromHandle(void* sharedTextureHandle, int width, int height)
{
#if defined(_WIN32)
	if (!Data->Header || !sharedTextureHandle || width <= 0 || height <= 0)
	{
		return;
	}

	// Clamp to the configured maximum; CEF already produced the full frame,
	// so scaling here would be expensive. Reject oversized frames instead.
	if (width > kMaxTextureSize || height > kMaxTextureSize)
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UXWV_LOG_WARN("frame %dx%d exceeds max texture size, frame dropped", width, height);
		}
		return;
	}

	ComPtr<ID3D11Texture2D> source;
	if (FAILED(Data->Device1->OpenSharedResource1(
		static_cast<HANDLE>(sharedTextureHandle),
		__uuidof(ID3D11Texture2D),
		reinterpret_cast<void**>(source.GetAddressOf())))
		|| !source)
	{
		UXWV_LOG_ERROR("failed to open CEF shared texture");
		return;
	}

	if (width != Data->PoolWidth || height != Data->PoolHeight)
	{
		if (!Data->CreatePool(width, height))
		{
			return;
		}
	}

	const int bufferIndex = static_cast<int>(Data->NextBuffer);
	Data->NextBuffer = (Data->NextBuffer + 1) % UXWV_FRAME_POOL_SIZE;

	ComPtr<IDXGIKeyedMutex> keyedMutex;
	Data->Pooled[bufferIndex].As(&keyedMutex);
	if (keyedMutex)
	{
		keyedMutex->AcquireSync(0, INFINITE);
	}
	Data->Context->CopyResource(Data->Pooled[bufferIndex].Get(), source.Get());
	if (keyedMutex)
	{
		keyedMutex->ReleaseSync(0);
	}

	WaitForGpu(Data->Device.Get(), Data->Context.Get());
	Data->Publish(bufferIndex, width, height);
#else
	(void)sharedTextureHandle;
	(void)width;
	(void)height;
#endif
}

uint32_t FramePublisher::LastSeq() const
{
#if defined(_WIN32)
	return Data->Header ? Data->Header->Seq : 0;
#else
	return 0;
#endif
}

uint32_t FramePublisher::Generation() const
{
#if defined(_WIN32)
	return Data->Generation;
#else
	return 0;
#endif
}
} // namespace uxwv
