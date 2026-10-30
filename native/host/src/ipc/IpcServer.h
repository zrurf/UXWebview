#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "uxwebview_ipc.h"

namespace uxwv
{
// Single-client control channel. The host is the pipe server so the plugin
// only has to open the pipe by name. Messages are a fixed header followed by
// Size payload bytes; the pipe runs in byte mode so both sides can stream.
//
// The pipe is created with FILE_FLAG_OVERLAPPED on purpose: a synchronous
// named pipe serialises every request on the handle, so a blocking ReadFile
// on the worker thread would stall a WriteFile on the UI thread. Each
// connection therefore owns a read and a write event, and Stop() signals a
// third event to break the worker out of any pending I/O.
class IpcServer
{
public:
	using MessageHandler = std::function<void(const UxIpcMessage&, const std::vector<uint8_t>&)>;
	using DisconnectHandler = std::function<void()>;

	IpcServer(std::string pipeName, MessageHandler onMessage, DisconnectHandler onDisconnect);
	~IpcServer();

	bool Start();
	void Stop();
	bool Send(const UxIpcMessage& message, const std::vector<uint8_t>& payload = {});
	bool IsConnected() const { return bConnected.load(); }

private:
	void ThreadMain();
	bool ReadExact(void* buffer, uint32_t bytes);
	void CloseEvents();

	std::string PipeName;
	MessageHandler OnMessage;
	DisconnectHandler OnDisconnect;

	std::thread Worker;
	std::mutex WriteMutex;
	std::atomic<bool> bRunning{false};
	std::atomic<bool> bConnected{false};
	void* PipeHandle = nullptr;  // HANDLE, opened and closed by the worker
	void* StopEvent = nullptr;   // HANDLE, manual reset
	void* ReadEvent = nullptr;   // HANDLE, auto reset
	void* WriteEvent = nullptr;  // HANDLE, auto reset
};
} // namespace uxwv
