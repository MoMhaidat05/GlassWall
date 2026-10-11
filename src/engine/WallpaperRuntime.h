#pragma once
#include "../core/PlaybackProtocol.h"
#include "../desktop-host/DesktopHost.h"
#include "../graphics/d3d11/Renderer.h"
#include "../media/mf/VideoReader.h"
#include "BackgroundControl.h"
#include "../library/Library.h"
#include <deque>

namespace glasswall::engine
{
    // Only the owner/message-pump thread touches HWND, renderer or reader objects.
    // The pipe thread accesses Submit/Snapshot through this small mutex boundary.
    class WallpaperRuntime
    {
    public:
        WallpaperRuntime(HINSTANCE instance, HANDLE stop);
        ~WallpaperRuntime();
        ipc::PlaybackResponse Submit(ipc::Command command, std::wstring path);
        void SubmitTray(ipc::Command command);
        ipc::PlaybackResponse Snapshot() const;
        std::wstring LibrarySnapshot() const;
        int Run(HWND control);
        bool HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);
    private:
        struct Job { ipc::Command command; std::wstring path; std::uint64_t id; };
        void ProcessJobs();
        void ProcessStartupRestore();
        void ProcessFrames();
        void RecordEof(media::VideoFrame const& frame);
        void StartLoop();
        enum class LoadPurpose { Raw, Import, Apply, Recovery, Relink };
        void BeginLoad(std::wstring const& path, std::uint64_t id, LoadPurpose purpose = LoadPurpose::Raw,
            std::wstring selectedId = {});
        void RefreshLibrary();
        void Complete(std::uint64_t id, HRESULT result);
        void Publish();
        void PrepareGraphics();
        void CleanupPlayback(bool clearPath);
        void BeginRecovery(bool device, HRESULT reason);
        void FailCurrent(HRESULT result, std::wstring const& description);
        void LogFailure(HRESULT result, std::wstring const& operation, wchar_t const* stage, std::uint64_t request = 0) const;
        DWORD WaitTimeout() const;
        void ProcessMessages(bool& quit, int& code);
        void ApplyPausePolicy();

        HINSTANCE m_instance;
        HWND m_control{};
        HANDLE m_stop;
        platform::Handle m_commands, m_timer;
        bool m_com{}, m_mf{}, m_timerArmed{};
        HMODULE m_mfLibrary{}, m_readerLibrary{};
        mutable std::mutex m_mutex;
        std::deque<Job> m_jobs;
        ipc::PlaybackResponse m_published, m_status;
        std::uint64_t m_nextId{ 1 }, m_loadId{};
        std::uint64_t m_outstanding{};
        desktop::DesktopHost m_host;
        graphics::Renderer m_renderer;
        std::unique_ptr<media::VideoReader> m_current, m_candidate;
        std::optional<media::VideoFrame> m_frame;
        std::wstring m_path, m_candidatePath;
        bool m_candidateRecovery{}, m_wantPlay{}, m_eof{}, m_clockReset{}, m_shown{};
        bool m_latencyReady{};
        LONGLONG m_clockBase{}, m_pausedAt{}, m_savedPosition{};
        ULONGLONG m_loadDeadline{}, m_recoveryAt{}, m_presentDeadline{};
        UINT m_recoveryAttempts{}, m_taskbarCreated{};
        wchar_t const* m_loadStage{ L"Idle" };
        std::uint32_t m_emptyReads{};
        core::LoopProgress m_loop;
        ULONGLONG m_loopDeadline{};
        LONGLONG m_initialTimestamp{}, m_lastFrameEnd{};
        BackgroundControl m_background;
        core::PausePolicy m_pause;
        bool m_hostInvalidated{};
        bool m_startupRestorePending{};
        ULONGLONG m_startupRestoreAt{}, m_startupRestoreUntil{};
        std::uint64_t m_startupRestoreRequest{};
        std::unique_ptr<library::Store> m_library;
        library::Snapshot m_publishedLibrary;
        std::wstring m_activeId, m_candidateId;
        LoadPurpose m_loadPurpose{ LoadPurpose::Raw };
        ipc::PlaybackState m_beforeImport{ ipc::PlaybackState::Empty };
    };
}
