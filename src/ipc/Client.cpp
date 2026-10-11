#include "Client.h"
#include "PipeIo.h"
#include "../platform/win32/Support.h"
#include "../core/LibraryProtocol.h"
#include <filesystem>
#include <vector>
#include <cstring>

namespace glasswall::ipc
{
    Reply Exchange(Request const& request)
    {
        auto identity = platform::CurrentIdentity();
        platform::Handle pipe(CreateFileW(identity.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!pipe && GetLastError() == ERROR_PIPE_BUSY)
        {
            if (!WaitNamedPipeW(identity.pipeName.c_str(), TimeoutMs)) return { GetLastError(), {} };
            pipe.reset(CreateFileW(identity.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        }
        if (!pipe) return { GetLastError(), {} };
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr))
            return { GetLastError(), {} };
        Request outgoing = request;
        auto write = PipeIo(pipe.get(), Operation::Write, &outgoing, sizeof(outgoing));
        if (write.error || write.bytes != sizeof(outgoing))
            return { write.error ? write.error : ERROR_INVALID_DATA, {} };
        Response incoming{};
        auto read = PipeIo(pipe.get(), Operation::Read, &incoming, sizeof(incoming));
        if (read.error || read.bytes != sizeof(incoming) || !IsValid(incoming))
            return { read.error ? read.error : ERROR_INVALID_DATA, {} };
        auto ack = Acknowledgement;
        auto acknowledged = PipeIo(pipe.get(), Operation::Write, &ack, sizeof(ack));
        if (acknowledged.error || acknowledged.bytes != sizeof(ack))
            return { acknowledged.error ? acknowledged.error : ERROR_INVALID_DATA, incoming };
        return { ERROR_SUCCESS, incoming };
    }

    DWORD StartEngine()
    {
        auto path = std::filesystem::path(platform::ExecutableDirectory()) / L"GlassWall.Engine.exe";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        // Explicit application path, no inherited handles and no parent-owned job.
        // If an existing job forbids breakaway, fail rather than couple lifetimes.
        if (!CreateProcessW(path.c_str(), nullptr, nullptr, nullptr, FALSE,
            CREATE_BREAKAWAY_FROM_JOB | CREATE_DEFAULT_ERROR_MODE, nullptr,
            path.parent_path().c_str(), &startup, &process))
            return GetLastError();
        platform::Handle thread(process.hThread);
        platform::Handle child(process.hProcess);
        return ERROR_SUCCESS;
    }

    Reply WaitForEngine(DWORD timeoutMs)
    {
        auto deadline = GetTickCount64() + timeoutMs;
        Reply reply;
        do
        {
            reply = Query();
            if (reply || (reply.error != ERROR_FILE_NOT_FOUND && reply.error != ERROR_PIPE_BUSY &&
                reply.error != ERROR_SEM_TIMEOUT)) return reply;
            // Bounded startup retry only; there is no background status polling.
            Sleep(50);
        } while (GetTickCount64() < deadline);
        return reply;
    }

    std::wstring Describe(Reply const& reply)
    {
        if (!reply) return L"Engine unavailable. " + platform::ErrorText(reply.error);
        if (reply.response.result != Result::Success) return L"Engine rejected this protocol request.";
        auto state = reply.response.state == EngineState::Ready ? L"Ready" : L"Stopping";
        return L"Engine PID " + std::to_wstring(reply.response.processId) + L" — " + state +
            L" (service connectivity; use --playback-state for wallpaper status)";
    }
    PlaybackReply Playback(Command command, std::wstring const& path, DWORD expectedProcessId)
    {
        static_assert(sizeof(wchar_t) == 2);
        if (!IsPlaybackCommand(command) || command == Command::GetLibrary ||
            (HasPayload(command) ? path.empty() || path.size() >= MaxPathCharacters : !path.empty()) ||
            path.find(L'\0') != std::wstring::npos) return { ERROR_INVALID_PARAMETER, {}, "Playback/validate request" };
        auto identity = platform::CurrentIdentity();
        char const* openOperation = "Playback/CreateFileW(open named pipe)";
        platform::Handle pipe(CreateFileW(identity.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!pipe && GetLastError() == ERROR_PIPE_BUSY)
        {
            if (!WaitNamedPipeW(identity.pipeName.c_str(), TimeoutMs))
                return { GetLastError(), {}, "Playback/WaitNamedPipeW" };
            openOperation = "Playback/CreateFileW(reopen after WaitNamedPipeW)";
            pipe.reset(CreateFileW(identity.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        }
        if (!pipe) return { GetLastError(), {}, openOperation };
        ULONG serverPid{};
        if (expectedProcessId)
        {
            if (!GetNamedPipeServerProcessId(pipe.get(), &serverPid))
                return { GetLastError(), {}, "Playback/GetNamedPipeServerProcessId" };
            if (serverPid != expectedProcessId) return { ERROR_INVALID_OWNER, {}, "Playback/verify server PID" };
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr))
            return { GetLastError(), {}, "Playback/SetNamedPipeHandleState" };
        PlaybackRequest request;
        request.command = command;
        request.pathBytes = HasPayload(command) ? static_cast<DWORD>((path.size() + 1) * 2) : 0;
        std::vector<std::byte> outgoing(sizeof(request) + request.pathBytes);
        std::memcpy(outgoing.data(), &request, sizeof(request));
        if (request.pathBytes) std::memcpy(outgoing.data() + sizeof(request), path.c_str(), request.pathBytes);
        auto written = PipeIo(pipe.get(), Operation::Write, outgoing.data(), static_cast<DWORD>(outgoing.size()));
        if (written.error || written.bytes != outgoing.size())
            return { written.error ? written.error : ERROR_INVALID_DATA, {}, "Playback/PipeIo(write request)" };
        PlaybackResponse incoming;
        auto read = PipeIo(pipe.get(), Operation::Read, &incoming, sizeof(incoming));
        if (read.error || read.bytes != sizeof(incoming) || !IsValid(incoming) ||
            (expectedProcessId && incoming.processId != expectedProcessId))
            return { read.error ? read.error : ERROR_INVALID_DATA, {}, read.error ?
                "Playback/PipeIo(read response)" : "Playback/validate response size, magic, version or PID" };
        auto ack = Acknowledgement;
        auto acknowledged = PipeIo(pipe.get(), Operation::Write, &ack, sizeof(ack));
        if (acknowledged.error || acknowledged.bytes != sizeof(ack))
            return { acknowledged.error ? acknowledged.error : ERROR_INVALID_DATA, incoming, "Playback/PipeIo(write acknowledgement)" };
        return { ERROR_SUCCESS, incoming };
    }
    LibraryReply QueryLibrary(DWORD expectedProcessId)
    {
        auto identity = platform::CurrentIdentity();
        char const* openOperation = "GetLibrary/CreateFileW(open named pipe)";
        platform::Handle pipe(CreateFileW(identity.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!pipe && GetLastError() == ERROR_PIPE_BUSY)
        {
            if (!WaitNamedPipeW(identity.pipeName.c_str(), TimeoutMs))
                return { GetLastError(), 0, {}, "GetLibrary/WaitNamedPipeW" };
            openOperation = "GetLibrary/CreateFileW(reopen after WaitNamedPipeW)";
            pipe.reset(CreateFileW(identity.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        }
        if (!pipe) return { GetLastError(), 0, {}, openOperation };
        ULONG serverPid{};
        if (!expectedProcessId) return { ERROR_INVALID_PARAMETER, 0, {}, "GetLibrary/validate expected PID" };
        if (!GetNamedPipeServerProcessId(pipe.get(), &serverPid))
            return { GetLastError(), 0, {}, "GetLibrary/GetNamedPipeServerProcessId" };
        if (serverPid != expectedProcessId) return { ERROR_INVALID_OWNER, 0, {}, "GetLibrary/verify server PID" };
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr))
            return { GetLastError(), 0, {}, "GetLibrary/SetNamedPipeHandleState" };
        PlaybackRequest request; request.command = Command::GetLibrary;
        auto written = PipeIo(pipe.get(), Operation::Write, &request, sizeof(request));
        if (written.error || written.bytes != sizeof(request))
            return { written.error ? written.error : ERROR_INVALID_DATA, 0, {}, "GetLibrary/PipeIo(write request)" };
        std::vector<std::byte> incoming(MaxLibraryMessage);
        auto read = PipeIo(pipe.get(), Operation::Read, incoming.data(), static_cast<DWORD>(incoming.size()));
        if (read.error || read.bytes < sizeof(LibraryResponse))
            return { read.error ? read.error : ERROR_INVALID_DATA, 0, {}, read.error ?
                "GetLibrary/PipeIo(read response)" : "GetLibrary/validate response size" };
        LibraryResponse header;
        std::memcpy(&header, incoming.data(), sizeof(header));
        if (!IsValid(header, read.bytes) || header.processId != expectedProcessId)
            return { ERROR_INVALID_DATA, 0, {}, "GetLibrary/validate response magic, version, size or PID" };
        std::wstring json(header.jsonBytes / 2, L'\0');
        std::memcpy(json.data(), incoming.data() + sizeof(header), header.jsonBytes);
        if (json.back() != L'\0' || json.find(L'\0') != json.size() - 1)
            return { ERROR_INVALID_DATA, 0, {}, "GetLibrary/validate UTF-16 terminator" };
        json.pop_back();
        auto ack = Acknowledgement;
        auto acknowledged = PipeIo(pipe.get(), Operation::Write, &ack, sizeof(ack));
        if (acknowledged.error || acknowledged.bytes != sizeof(ack))
            return { acknowledged.error ? acknowledged.error : ERROR_INVALID_DATA, 0, {}, "GetLibrary/PipeIo(write acknowledgement)" };
        return { ERROR_SUCCESS, header.processId, std::move(json) };
    }
}
