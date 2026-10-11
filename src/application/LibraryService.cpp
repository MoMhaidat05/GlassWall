#include "LibraryService.h"
#include "../media/mf/MediaPath.h"
#include "../platform/win32/HResult.h"
#include <filesystem>

namespace glasswall::application
{
    namespace
    {
        template<typename Reply> void CheckIpc(Reply const& reply, char const* context)
        {
            if (!reply)
            {
                auto operation = std::string(context) + " / " + (reply.operation ? reply.operation : "IPC transport");
                throw platform::Error(operation.c_str(), reply.error);
            }
        }
    }
    LibraryService::LibraryService(bool startIfMissing)
    {
        auto reply = ipc::Playback(ipc::Command::GetPlaybackState);
        if (!reply && reply.error == ERROR_FILE_NOT_FOUND && startIfMissing)
        {
            auto error = ipc::StartEngine();
            if (error) throw platform::Error("Start independent engine", error);
            auto started = ipc::WaitForEngine();
            if (!started) throw platform::Error("Wait for engine", started.error);
            reply = ipc::Playback(ipc::Command::GetPlaybackState);
        }
        CheckIpc(reply, "Connect to matching GlassWall engine");
        m_pid = reply.response.processId;
        m_engine.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, m_pid));
        if (!m_engine) throw platform::Error("Open responding engine", GetLastError());
        DWORD session{}, ownSession{};
        wchar_t executable[32768]{}; DWORD length = static_cast<DWORD>(std::size(executable));
        if (!ProcessIdToSessionId(m_pid, &session) || !ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) ||
            !QueryFullProcessImageNameW(m_engine.get(), 0, executable, &length))
            throw platform::Error("Identify responding engine", GetLastError());
        auto expected = std::filesystem::canonical(std::filesystem::path(platform::ExecutableDirectory()) / L"GlassWall.Engine.exe").wstring();
        auto actual = std::filesystem::canonical(executable).wstring();
        if (session != ownSession || !library::SamePath(expected, actual))
            throw platform::Failure(HRESULT_FROM_WIN32(ERROR_INVALID_OWNER), L"The running engine belongs to another installation/session; use its tray to exit it");
        CheckInstance();
    }
    void LibraryService::CheckInstance() const
    {
        if (WaitForSingleObject(m_engine.get(), 0) != WAIT_TIMEOUT)
            throw platform::Failure(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED), L"The connected engine exited; reconnect explicitly");
    }
    ipc::PlaybackResponse LibraryService::State()
    {
        CheckInstance();
        auto reply = ipc::Playback(ipc::Command::GetPlaybackState, {}, m_pid);
        CheckIpc(reply, "Get playback state");
        return reply.response;
    }
    library::Snapshot LibraryService::List()
    {
        auto deadline = GetTickCount64() + 10000;
        for (;;)
        {
            CheckInstance();
            auto reply = ipc::QueryLibrary(m_pid);
            CheckIpc(reply, "Read library snapshot");
            auto snapshot = library::DecodeSnapshot(reply.json);
            if (snapshot.processId != m_pid) throw platform::Error("Library response identity", ERROR_INVALID_DATA);
            if (snapshot.ready) return snapshot;
            if (GetTickCount64() >= deadline) throw platform::Error("Library startup timeout", ERROR_TIMEOUT);
            Sleep(50); // bounded foreground command/startup wait only
        }
    }
    void LibraryService::Execute(ipc::Command command, std::wstring const& payload)
    {
        auto snapshot = List();
        platform::Check(snapshot.storageError, (L"Library storage unavailable: " + snapshot.warning).c_str());
        auto deadline = GetTickCount64() + 15000;
        ipc::PlaybackReply accepted;
        for (;;)
        {
            CheckInstance();
            accepted = ipc::Playback(command, payload, m_pid);
            CheckIpc(accepted, "Send library command");
            if (accepted.response.result != ipc::PlaybackResult::Busy) break;
            if (GetTickCount64() >= deadline) throw platform::Error("Library command busy", ERROR_BUSY);
            Sleep(50);
        }
        if (accepted.response.result != ipc::PlaybackResult::Accepted)
            throw platform::Failure(accepted.response.error, L"Engine rejected library command");
        for (;;)
        {
            auto state = State();
            if (state.completedRequestId == accepted.response.requestId)
            { platform::Check(state.operationError, L"Library command completion"); return; }
            if (state.completedRequestId > accepted.response.requestId)
                throw platform::Failure(HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED), L"Another controller superseded this command; inspect library/playback state");
            if (GetTickCount64() >= deadline)
                throw platform::Failure(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Command completion timed out; it may still complete, inspect state before retrying");
            Sleep(50);
        }
    }
    library::Entry LibraryService::Metadata(std::wstring const& id)
    { return library::Find(List().catalog, id); }
    library::Entry LibraryService::Import(std::wstring const& path)
    {
        auto normalized = media::ValidateLocalPath(std::filesystem::absolute(path).wstring());
        Execute(ipc::Command::ImportWallpaper, normalized);
        auto snapshot = List(); auto entry = library::FindPath(snapshot.catalog, normalized);
        if (!entry) throw platform::Failure(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"Imported entry was removed by another controller");
        return *entry;
    }
    void LibraryService::Rename(std::wstring const& id, std::wstring const& name)
    { Execute(ipc::Command::RenameWallpaper, library::RenamePayload(id, name)); }
    void LibraryService::Remove(std::wstring const& id) { Execute(ipc::Command::RemoveWallpaper, id); }
    void LibraryService::Relink(std::wstring const& id, std::wstring const& path)
    {
        auto normalized = media::ValidateLocalPath(std::filesystem::absolute(path).wstring());
        Execute(ipc::Command::RelinkWallpaper, library::RelinkPayload(id, normalized));
    }
    void LibraryService::Apply(std::wstring const& id) { Execute(ipc::Command::ApplyWallpaper, id); }
    void LibraryService::Restore() { Execute(ipc::Command::RestoreWallpaper); }
}
