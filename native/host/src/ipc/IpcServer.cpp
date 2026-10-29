#include "ipc/IpcServer.h"

#include "util/Log.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace uxwv
{
IpcServer::IpcServer(std::string pipeName, MessageHandler onMessage, DisconnectHandler onDisconnect)
	: PipeName(std::move(pipeName)), OnMessage(std::move(onMessage)), OnDisconnect(std::move(onDisconnect))
{
}

IpcServer::~IpcServer()
{
	Stop();
}

bool IpcServer::Start()
{
#if defined(_WIN32)
	if (bRunning.exchange(true))
	{
		return true;
	}

	Worker = std::thread(&IpcServer::ThreadMain, this);
	return true;
#else
	UXWV_LOG_WARN("IpcServer is not implemented on this platform yet");
	return false;
#endif
}

void IpcServer::Stop()
{
#if defined(_WIN32)
	if (!bRunning.exchange(false))
	{
		return;
	}

	{
		std::lock_guard<std::mutex> Lock(WriteMutex);
		if (PipeHandle)
		{
			CloseHandle(static_cast<HANDLE>(PipeHandle));
			PipeHandle = nullptr;
		}
	}

	if (Worker.joinable())
	{
		Worker.join();
	}
#endif
}

bool IpcServer::Send(const UxIpcMessage& message)
{
#if defined(_WIN32)
	std::lock_guard<std::mutex> Lock(WriteMutex);
	if (!PipeHandle || !bConnected.load())
	{
		return false;
	}

	DWORD written = 0;
	if (!WriteFile(static_cast<HANDLE>(PipeHandle), &message, sizeof(message), &written, nullptr) ||
		written != sizeof(message))
	{
		UXWV_LOG_WARN("IPC write failed");
		return false;
	}
	return true;
#else
	(void)message;
	return false;
#endif
}

void IpcServer::ThreadMain()
{
#if defined(_WIN32)
	while (bRunning.load())
	{
		std::wstring FullName = L"\\\\.\\pipe\\" + std::wstring(PipeName.begin(), PipeName.end());
		HANDLE pipe = CreateNamedPipeW(
			FullName.c_str(),
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			1, 4096, 4096, 0, nullptr);

		if (pipe == INVALID_HANDLE_VALUE)
		{
			UXWV_LOG_ERROR("failed to create named pipe");
			break;
		}

		{
			std::lock_guard<std::mutex> Lock(WriteMutex);
			PipeHandle = pipe;
		}

		BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (!bRunning.load())
		{
			break;
		}

		if (!connected)
		{
			if (bRunning.load())
			{
				UXWV_LOG_ERROR("IPC connect failed");
			}
			std::lock_guard<std::mutex> Lock(WriteMutex);
			if (PipeHandle)
			{
				CloseHandle(static_cast<HANDLE>(PipeHandle));
				PipeHandle = nullptr;
			}
			continue;
		}

		bConnected.store(true);
		UXWV_LOG_INFO("IPC client connected");

		bool bDisconnect = false;
		while (bRunning.load())
		{
			UxIpcMessage message{};
			DWORD read = 0;
			BOOL ok = ReadFile(pipe, &message, sizeof(message), &read, nullptr);
			if (!ok || read != sizeof(message))
			{
				bDisconnect = true;
				break;
			}

			if (message.ProtocolVersion != UXWV_IPC_VERSION)
			{
				UXWV_LOG_WARN("IPC protocol mismatch: got %u, expected %u",
					message.ProtocolVersion, UXWV_IPC_VERSION);
				bDisconnect = true;
				break;
			}

			if (OnMessage)
			{
				OnMessage(message);
			}
		}

		bConnected.store(false);
		DisconnectNamedPipe(pipe);
		{
			std::lock_guard<std::mutex> Lock(WriteMutex);
			CloseHandle(pipe);
			PipeHandle = nullptr;
		}

		if (bDisconnect && OnDisconnect)
		{
			OnDisconnect();
		}
	}
#endif
}
} // namespace uxwv
