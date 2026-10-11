#pragma once
#include "../ipc/Client.h"
#include "../library/Library.h"

namespace glasswall::application
{
    // Short-lived controllers, including the future WinUI adapter, use this API.
    // Call from a COM-initialized worker thread; synchronous command waits never
    // own the engine lifetime. Destruction closes only this client's process handle.
    class LibraryService
    {
    public:
        explicit LibraryService(bool startIfMissing = true);
        library::Snapshot List();
        library::Entry Metadata(std::wstring const& id);
        library::Entry Import(std::wstring const& path);
        void Rename(std::wstring const& id, std::wstring const& name);
        void Remove(std::wstring const& id);
        void Relink(std::wstring const& id, std::wstring const& path);
        void Apply(std::wstring const& id);
        void Restore();
        ipc::PlaybackResponse State();
        DWORD ProcessId() const noexcept { return m_pid; }
    private:
        void CheckInstance() const;
        void Execute(ipc::Command command, std::wstring const& payload = {});
        platform::Handle m_engine;
        DWORD m_pid{};
    };
}
