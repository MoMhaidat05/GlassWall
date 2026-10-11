#pragma once
#include "../platform/win32/Support.h"
#include "../core/Protocol.h"

namespace glasswall::ipc
{
    enum class Operation { Connect, Read, Write };
    struct IoResult { DWORD error{}; DWORD bytes{}; };

    // Each operation owns its OVERLAPPED until completion, including cancellation.
    inline IoResult PipeIo(HANDLE pipe, Operation operation, void* buffer, DWORD size,
        HANDLE stop = nullptr, DWORD timeout = TimeoutMs)
    {
        platform::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return { GetLastError(), 0 };
        OVERLAPPED pending{};
        pending.hEvent = event.get();
        DWORD bytes{};
        BOOL completed{};
        switch (operation)
        {
        case Operation::Connect: completed = ConnectNamedPipe(pipe, &pending); break;
        case Operation::Read: completed = ReadFile(pipe, buffer, size, nullptr, &pending); break;
        case Operation::Write: completed = WriteFile(pipe, buffer, size, nullptr, &pending); break;
        }
        if (completed)
        {
            if (operation != Operation::Connect &&
                !GetOverlappedResult(pipe, &pending, &bytes, FALSE)) return { GetLastError(), bytes };
            return { ERROR_SUCCESS, bytes };
        }
        auto error = GetLastError();
        if (operation == Operation::Connect && error == ERROR_PIPE_CONNECTED) return {};
        if (error != ERROR_IO_PENDING) return { error, bytes };

        HANDLE waits[] = { stop, event.get() };
        auto result = stop ? WaitForMultipleObjects(2, waits, FALSE, timeout) :
            WaitForSingleObject(event.get(), timeout);
        bool ready = result == (stop ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0);
        if (!ready)
        {
            auto failure = result == WAIT_TIMEOUT ? ERROR_TIMEOUT :
                (stop && result == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : GetLastError());
            CancelIoEx(pipe, &pending);
            // CancelIoEx is not a completion fence. Drain before the stack/event dies.
            GetOverlappedResult(pipe, &pending, &bytes, TRUE);
            return { failure, bytes };
        }
        if (!GetOverlappedResult(pipe, &pending, &bytes, FALSE)) return { GetLastError(), bytes };
        return { ERROR_SUCCESS, bytes };
    }
}
