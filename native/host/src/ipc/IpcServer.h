#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "uxwebview_ipc.h"

namespace uxwv
{
// Single-client control channel. The host is the pipe server so the plugin
// only has to open the pipe by name.
class IpcServer
{
public:
	using MessageHandler = std::function<void(const UxIpcMessage&)>;
	using DisconnectHandler = std::function<void()>;

	IpcServer(std::string pipeName, MessageHandler onMessage, DisconnectHandler onDisconnect);
	~IpcServer();

	bool Start();
	void Stop();
	bool Send(const UxIpcMessage& message);
	bool IsConnected() const { return bConnected.load(); }

private:
	void ThreadMain();

	std::string PipeName;
	MessageHandler OnMessage;
	DisconnectHandler OnDisconnect;

	std::thread Worker;
	std::mutex WriteMutex;
	std::atomic<bool> bRunning{false};
	std::atomic<bool> bConnected{false};
	void* PipeHandle = nullptr; // HANDLE
};
} // namespace uxwv
