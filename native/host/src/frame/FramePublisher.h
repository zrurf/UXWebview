#pragma once

#include <cstdint>
#include <string>

#include "uxwebview_frame.h"

namespace uxwv
{
// Copies CEF accelerated frames into a pool of NT-handle shared textures and
// publishes the newest one through a shared memory header (T1 transport).
class FramePublisher
{
public:
	FramePublisher();
	~FramePublisher();

	FramePublisher(const FramePublisher&) = delete;
	FramePublisher& operator=(const FramePublisher&) = delete;

	// Creates the shared memory section. Returns false on failure.
	bool Start(const std::string& sharedMemoryName);
	void Stop();

	// Called from the CEF UI thread with a D3D11 NT handle that is only valid
	// for the duration of the callback.
	void PublishFromHandle(void* sharedTextureHandle, int width, int height);

	uint32_t LastSeq() const;
	uint32_t Generation() const;

private:
	struct Impl;
	Impl* Data = nullptr;
};
} // namespace uxwv
