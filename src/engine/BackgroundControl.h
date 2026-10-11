#pragma once
#include "../core/PlaybackProtocol.h"
#include "../core/PausePolicy.h"
#include "../platform/win32/Support.h"
#include <shellapi.h>

namespace glasswall::engine
{
    inline constexpr UINT BackgroundChangedMessage = WM_APP + 20;
    inline constexpr UINT TrayMessage = WM_APP + 21;
    inline constexpr UINT TrayCommandMessage = WM_APP + 22;
    inline constexpr UINT TrayToggle = 1, TrayStop = 2, TrayExit = 3;
    // Owner thread only. Callbacks update small policy flags, never the media pipeline.
    class BackgroundControl
    {
    public:
        ~BackgroundControl();
        void Start(HWND control);
        void Reset() noexcept;
        bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
        void RefreshDesktop();
        std::uint32_t Reasons() const noexcept { return m_policy.reasons; }
        ULONGLONG TakeSleepDuration() noexcept;
        void UpdateTray(ipc::PlaybackState state, bool manual, bool loaded);
    private:
        static void CALLBACK ForegroundEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
        void Wake();
        void AddTray();
        void Menu();
        HWND m_control{};
        HWINEVENTHOOK m_foreground{}, m_desktopSwitch{}, m_minimizeEnd{}, m_visibility{};
        HWINEVENTHOOK m_windowState{}, m_cloaking{};
        HPOWERNOTIFY m_display{};
        bool m_sessionRegistered{}, m_added{}, m_wakePending{}, m_manual{}, m_loaded{};
        UINT m_taskbarCreated{};
        UINT m_trayRetries{};
        core::PausePolicy m_policy;
        NOTIFYICONDATAW m_icon{};
        std::wstring m_tooltip;
        ULONGLONG m_sleepStarted{}, m_sleepDuration{};
        static thread_local BackgroundControl* s_owner;
    };
}
