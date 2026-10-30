#include "ipc/IpcServer.h"

#include <cstring>

#include "util/Log.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace uxwv
{
#if defined(_WIN32)
namespace
{
// Waits for an already pending overlapped operation. Returning false means
// the server is stopping or the I/O failed; the operation is always drained
// so the caller's OVERLAPPED can safely leave its stack frame.
bool CompletePending(HANDLE pipe, OVERLAPPED& overlapped, HANDLE ioEvent,
	HANDLE stopEvent, DWORD& transferred)
{
	HANDLE waits[2] = {ioEvent, stopEvent};
	const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
	if (result != WAIT_OBJECT_0)
	{
		CancelIoEx(pipe, &overlapped);
		DWORD ignored = 0;
		GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
		return false;
	}
	return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
}
} // namespace
#endif

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

	StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ReadEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	WriteEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!StopEvent || !ReadEvent || !WriteEvent)
	{
		UXWV_LOG_ERROR("failed to create IPC events");
		bRunning.store(false);
		CloseEvents();
		return false;
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
		if (Worker.joinable())
		{
			Worker.join();
		}
		return;
	}

	if (StopEvent)
	{
		SetEvent(static_cast<HANDLE>(StopEvent));
	}

	if (Worker.joinable())
	{
		Worker.join();
	}

	{
		std::lock_guard<std::mutex> Lock(WriteMutex);
		if (PipeHandle)
		{
			CloseHandle(static_cast<HANDLE>(PipeHandle));
			PipeHandle = nullptr;
		}
	}
	CloseEvents();
#endif
}

void IpcServer::CloseEvents()
{
#if defined(_WIN32)
	if (StopEvent)
	{
		CloseHandle(static_cast<HANDLE>(StopEvent));
		StopEvent = nullptr;
	}
	if (ReadEvent)
	{
		CloseHandle(static_cast<HANDLE>(ReadEvent));
		ReadEvent = nullptr;
	}
	if (WriteEvent)
	{
		CloseHandle(static_cast<HANDLE>(WriteEvent));
		WriteEvent = nullptr;
	}
#endif
}

bool IpcServer::Send(const UxIpcMessage& message, const std::vector<uint8_t>& payload)
{
#if defined(_WIN32)
	if (message.Size > UXWV_IPC_MAX_PAYLOAD || payload.size() != message.Size)
	{
		UXWV_LOG_ERROR("refusing to send malformed IPC message (size %u, payload %zu)",
			message.Size, payload.size());
		return false;
	}

	std::lock_guard<std::mutex> Lock(WriteMutex);
	if (!PipeHandle || !bConnected.load() || !bRunning.load())
	{
		return false;
	}

	std::vector<uint8_t> buffer(sizeof(UxIpcMessage) + payload.size());
	std::memcpy(buffer.data(), &message, sizeof(UxIpcMessage));
	if (!payload.empty())
	{
		std::memcpy(buffer.data() + sizeof(UxIpcMessage), payload.data(), payload.size());
	}

	OVERLAPPED overlapped{};
	overlapped.hEvent = static_cast<HANDLE>(WriteEvent);
	DWORD written = 0;
	BOOL ok = WriteFile(static_cast<HANDLE>(PipeHandle), buffer.data(),
		static_cast<DWORD>(buffer.size()), &written, &overlapped);
	if (!ok)
	{
		if (GetLastError() != ERROR_IO_PENDING ||
			!CompletePending(static_cast<HANDLE>(PipeHandle), overlapped,
				static_cast<HANDLE>(WriteEvent), static_cast<HANDLE>(StopEvent), written))
		{
			UXWV_LOG_WARN("IPC write failed");
			return false;
		}
	}

	if (written != buffer.size())
	{
		UXWV_LOG_WARN("IPC write truncated (%lu of %zu)", written, buffer.size());
		return false;
	}
	return true;
#else
	(void)message;
	(void)payload;
	return false;
#endif
}

bool IpcServer::ReadExact(void* buffer, uint32_t bytes)
{
#if defined(_WIN32)
	HANDLE pipe = static_cast<HANDLE>(PipeHandle);
	HANDLE readEvent = static_cast<HANDLE>(ReadEvent);
	HANDLE stopEvent = static_cast<HANDLE>(StopEvent);

	uint8_t* cursor = static_cast<uint8_t*>(buffer);
	uint32_t remaining = bytes;
	while (remaining > 0)
	{
		OVERLAPPED overlapped{};
		overlapped.hEvent = readEvent;
		DWORD read = 0;
		BOOL ok = ReadFile(pipe, cursor, remaining, &read, &overlapped);
		if (!ok)
		{
			if (GetLastError() != ERROR_IO_PENDING ||
				!CompletePending(pipe, overlapped, readEvent, stopEvent, read))
			{
				return false;
			}
		}
		if (read == 0)
		{
			return false;
		}
		cursor += read;
		remaining -= read;
	}
	return true;
#else
	(void)buffer;
	(void)bytes;
	return false;
#endif
}

void IpcServer::ThreadMain()
{
#if defined(_WIN32)
	HANDLE readEvent = static_cast<HANDLE>(ReadEvent);
	HANDLE stopEvent = static_cast<HANDLE>(StopEvent);

	while (bRunning.load())
	{
		std::wstring FullName = L"\\\\.\\pipe\\" + std::wstring(PipeName.begin(), PipeName.end());
		HANDLE pipe = CreateNamedPipeW(
			FullName.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
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

		OVERLAPPED connectOverlapped{};
		connectOverlapped.hEvent = readEvent;
		BOOL connected = ConnectNamedPipe(pipe, &connectOverlapped) ? TRUE : FALSE;
		if (!connected)
		{
			const DWORD error = GetLastError();
			if (error == ERROR_PIPE_CONNECTED)
			{
				connected = TRUE;
			}
			else if (error == ERROR_IO_PENDING)
			{
				DWORD ignored = 0;
				connected = CompletePending(pipe, connectOverlapped, readEvent, stopEvent, ignored)
					? TRUE : FALSE;
			}
		}

		if (!bRunning.load())
		{
			{
				std::lock_guard<std::mutex> Lock(WriteMutex);
				PipeHandle = nullptr;
			}
			CloseHandle(pipe);
			break;
		}

		if (!connected)
		{
			UXWV_LOG_ERROR("IPC connect failed");
			{
				std::lock_guard<std::mutex> Lock(WriteMutex);
				PipeHandle = nullptr;
			}
			CloseHandle(pipe);
			continue;
		}

		bConnected.store(true);
		UXWV_LOG_INFO("IPC client connected");

		bool bDisconnect = false;
		while (bRunning.load())
		{
			UxIpcMessage message{};
			if (!ReadExact(&message, sizeof(message)))
			{
				bDisconnect = true;
				break;
			}

			UXWV_LOG_DEBUG("IPC read type=%u size=%u instance=%u",
				message.Type, message.Size, message.Instance);

			if (message.ProtocolVersion != UXWV_IPC_VERSION)
			{
				UXWV_LOG_WARN("IPC protocol mismatch: got %u, expected %u",
					message.ProtocolVersion, UXWV_IPC_VERSION);
				bDisconnect = true;
				break;
			}

			if (message.Size > UXWV_IPC_MAX_PAYLOAD)
			{
				UXWV_LOG_ERROR("IPC message payload too large: %u", message.Size);
				bDisconnect = true;
				break;
			}

			std::vector<uint8_t> payload(message.Size);
			if (message.Size > 0 && !ReadExact(payload.data(), message.Size))
			{
				bDisconnect = true;
				break;
			}

			if (OnMessage)
			{
				OnMessage(message, payload);
			}
		}

		bConnected.store(false);
		DisconnectNamedPipe(pipe);
		{
			std::lock_guard<std::mutex> Lock(WriteMutex);
			PipeHandle = nullptr;
		}
		CloseHandle(pipe);

		if (bDisconnect && bRunning.load() && OnDisconnect)
		{
			OnDisconnect();
		}
	}
#endif
}
} // namespace uxwv
