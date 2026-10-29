#include "frame/FramePublisher.h"

#include "util/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#if defined(_WIN32)
#include <Windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
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

uint64_t LuidValue(const LUID& luid)
{
	return (static_cast<uint64_t>(luid.HighPart) << 32) | luid.LowPart;
}

// Finds the DXGI adapter with a matching LUID so the host renders on the
// same GPU as the game; shared handles do not cross adapters.
ComPtr<IDXGIAdapter> FindAdapterByLuid(uint64_t luid)
{
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
	{
		return nullptr;
	}

	ComPtr<IDXGIAdapter1> adapter;
	for (UINT index = 0;
		factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND;
		++index)
	{
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(adapter->GetDesc1(&desc)) && LuidValue(desc.AdapterLuid) == luid)
		{
			return adapter;
		}
		adapter.Reset();
	}
	return nullptr;
}

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
	ComPtr<ID3D12Device> Device12;

	// Pool textures are D3D12 resources so the Unreal side (D3D12) can open
	// them; D3D11 opens the same shared handles as copy destinations.
	// D3D11 keyed-mutex textures are not accepted by D3D12 at all.
	ComPtr<ID3D12Resource> PooledShared[UXWV_FRAME_POOL_SIZE];
	ComPtr<ID3D11Texture2D> Pooled[UXWV_FRAME_POOL_SIZE];
	// Handles opened by this process.
	HANDLE SharedHandles[UXWV_FRAME_POOL_SIZE] = {nullptr, nullptr, nullptr};
	// The same handles duplicated into the game process; NT handles are
	// process-local, so raw values are not valid there otherwise.
	HANDLE ClientHandles[UXWV_FRAME_POOL_SIZE] = {nullptr, nullptr, nullptr};
	HANDLE OwnerProcess = nullptr;
	uint64_t AdapterLuid = 0;
	int PoolWidth = 0;
	int PoolHeight = 0;
	uint32_t Generation = 0;
	uint32_t NextBuffer = 0;

	HANDLE Mapping = nullptr;
	UxFrameHeader* Header = nullptr;

	uint32_t FrameCount = 0;
	int64_t LastSummaryTicks = 0;

	// Copy + flush + wait wall time per frame, i.e. what handing a frame to the
	// shared pool costs on the host GPU.
	uint64_t CopyTotalMicros = 0;
	uint64_t CopyMaxMicros = 0;
	uint32_t CopySamples = 0;

	bool CreateDevice()
	{
		D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_11_0;
		ComPtr<IDXGIAdapter> adapter;
		D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_HARDWARE;
		if (AdapterLuid != 0)
		{
			adapter = FindAdapterByLuid(AdapterLuid);
			if (adapter)
			{
				driverType = D3D_DRIVER_TYPE_UNKNOWN;
			}
			else
			{
				UXWV_LOG_WARN("adapter LUID 0x%llX not found, using the default adapter",
					static_cast<unsigned long long>(AdapterLuid));
			}
		}

		HRESULT hr = D3D11CreateDevice(
			adapter.Get(), driverType, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			levels, _countof(levels), D3D11_SDK_VERSION,
			&Device, &achieved, &Context);
		if (FAILED(hr))
		{
			UXWV_LOG_ERROR("D3D11 device creation failed (0x%08X)", static_cast<unsigned>(hr));
			return false;
		}

		Device.As(&Device1);

		ComPtr<IDXGIDevice> dxgiDevice;
		Device.As(&dxgiDevice);
		ComPtr<IDXGIAdapter> usedAdapter;
		DXGI_ADAPTER_DESC adapterDesc{};
		if (dxgiDevice
			&& SUCCEEDED(dxgiDevice->GetAdapter(&usedAdapter))
			&& usedAdapter
			&& SUCCEEDED(usedAdapter->GetDesc(&adapterDesc)))
		{
			UXWV_LOG_INFO("D3D11 device on adapter LUID 0x%llX '%ls'",
				static_cast<unsigned long long>(LuidValue(adapterDesc.AdapterLuid)),
				adapterDesc.Description);
		}
		return Device1 != nullptr;
	}

	void ReleasePool()
	{
		for (auto& texture : Pooled)
		{
			texture.Reset();
		}
		for (auto& resource : PooledShared)
		{
			resource.Reset();
		}
		for (int i = 0; i < UXWV_FRAME_POOL_SIZE; ++i)
		{
			if (ClientHandles[i] && ClientHandles[i] != SharedHandles[i])
			{
				CloseHandle(ClientHandles[i]);
			}
			ClientHandles[i] = nullptr;

			if (SharedHandles[i])
			{
				CloseHandle(SharedHandles[i]);
				SharedHandles[i] = nullptr;
			}
		}
		PoolWidth = 0;
		PoolHeight = 0;
	}

	bool CreateD3D12Device()
	{
		if (Device12)
		{
			return true;
		}

		ComPtr<IDXGIDevice> dxgiDevice;
		Device.As(&dxgiDevice);
		ComPtr<IDXGIAdapter> adapter;
		if (!dxgiDevice || FAILED(dxgiDevice->GetAdapter(&adapter))
			|| !adapter)
		{
			UXWV_LOG_ERROR("failed to query the D3D11 adapter");
			return false;
		}

		HRESULT hr = D3D12CreateDevice(
			adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device12));
		if (FAILED(hr) || !Device12)
		{
			UXWV_LOG_ERROR("D3D12 device creation failed (0x%08X)", static_cast<unsigned>(hr));
			return false;
		}
		return true;
	}

	bool CreatePool(int width, int height)
	{
		ReleasePool();

		if (!CreateD3D12Device())
		{
			return false;
		}

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = static_cast<UINT64>(width);
		desc.Height = static_cast<UINT>(height);
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		// ALLOW_RENDER_TARGET is required for D3D11 to open the resource;
		// simultaneous access keeps it usable across D3D11 and D3D12.
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
			| D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

		D3D12_HEAP_PROPERTIES heapProps{};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapProps.CreationNodeMask = 1;
		heapProps.VisibleNodeMask = 1;

		for (int i = 0; i < UXWV_FRAME_POOL_SIZE; ++i)
		{
			HRESULT hr = Device12->CreateCommittedResource(
				&heapProps, D3D12_HEAP_FLAG_SHARED, &desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&PooledShared[i]));
			if (FAILED(hr) || !PooledShared[i])
			{
				UXWV_LOG_ERROR("failed to create pooled D3D12 texture %d (%dx%d), hr=0x%08X",
					i, width, height, static_cast<unsigned>(hr));
				ReleasePool();
				return false;
			}

			hr = Device12->CreateSharedHandle(
				PooledShared[i].Get(), nullptr, GENERIC_ALL, nullptr, &SharedHandles[i]);
			if (FAILED(hr))
			{
				UXWV_LOG_ERROR("failed to create shared handle for pooled texture %d, hr=0x%08X",
					i, static_cast<unsigned>(hr));
				ReleasePool();
				return false;
			}

			hr = Device1->OpenSharedResource1(
				SharedHandles[i], __uuidof(ID3D11Texture2D),
				reinterpret_cast<void**>(Pooled[i].GetAddressOf()));
			if (FAILED(hr) || !Pooled[i])
			{
				UXWV_LOG_ERROR("failed to open pooled texture %d on D3D11, hr=0x%08X",
					i, static_cast<unsigned>(hr));
				ReleasePool();
				return false;
			}

			if (OwnerProcess)
			{
				if (!DuplicateHandle(GetCurrentProcess(), SharedHandles[i], OwnerProcess,
					&ClientHandles[i], 0, FALSE, DUPLICATE_SAME_ACCESS))
				{
					UXWV_LOG_ERROR("failed to duplicate handle %d into the game process", i);
					ClientHandles[i] = nullptr;
					ReleasePool();
					return false;
				}
			}
			else
			{
				ClientHandles[i] = SharedHandles[i];
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
				static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ClientHandles[i]));
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
			const uint64_t avgMicros = CopySamples > 0 ? CopyTotalMicros / CopySamples : 0;
			UXWV_LOG_INFO("published %u frames in 5s, copyAvg=%lluus copyMax=%lluus, generation %u",
				FrameCount,
				static_cast<unsigned long long>(avgMicros),
				static_cast<unsigned long long>(CopyMaxMicros),
				Generation);
			FrameCount = 0;
			CopyTotalMicros = 0;
			CopyMaxMicros = 0;
			CopySamples = 0;
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

bool FramePublisher::Start(const std::string& sharedMemoryName, uint64_t adapterLuid, uint32_t ownerPid)
{
#if defined(_WIN32)
	Data->AdapterLuid = adapterLuid;
	if (ownerPid != 0)
	{
		Data->OwnerProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, ownerPid);
		if (!Data->OwnerProcess)
		{
			UXWV_LOG_WARN("failed to open game process %u, shared handles stay local",
				static_cast<unsigned>(ownerPid));
		}
	}

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
	if (Data->OwnerProcess)
	{
		CloseHandle(Data->OwnerProcess);
		Data->OwnerProcess = nullptr;
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

	// Pool textures come from D3D12 and carry no keyed mutex; the sequence
	// published after WaitForGpu provides the needed ordering instead.
	ComPtr<IDXGIKeyedMutex> keyedMutex;
	Data->Pooled[bufferIndex].As(&keyedMutex);
	if (keyedMutex)
	{
		keyedMutex->AcquireSync(0, INFINITE);
	}
	const int64_t copyStart = std::chrono::steady_clock::now().time_since_epoch().count();
	Data->Context->CopyResource(Data->Pooled[bufferIndex].Get(), source.Get());
	if (keyedMutex)
	{
		keyedMutex->ReleaseSync(0);
	}

	WaitForGpu(Data->Device.Get(), Data->Context.Get());
	const int64_t copyMicros =
		(std::chrono::steady_clock::now().time_since_epoch().count() - copyStart) / 1000;
	Data->CopyTotalMicros += static_cast<uint64_t>(copyMicros);
	Data->CopySamples++;
	Data->CopyMaxMicros = std::max(Data->CopyMaxMicros, static_cast<uint64_t>(copyMicros));
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
