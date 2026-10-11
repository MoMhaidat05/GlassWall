#include "BackgroundControl.h"
#include "../desktop-host/DesktopHost.h"
#include <wtsapi32.h>
#include <cstring>

namespace glasswall::engine
{
    thread_local BackgroundControl* BackgroundControl::s_owner{};
    BackgroundControl::~BackgroundControl() { Reset(); }
    void BackgroundControl::Wake()
    {
        if (!m_wakePending)
            m_wakePending = PostMessageW(m_control, BackgroundChangedMessage, 0, 0) != FALSE;
    }
    void CALLBACK BackgroundControl::ForegroundEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD)
    {
        if ((event >= EVENT_OBJECT_DESTROY && event <= EVENT_OBJECT_HIDE) ||
            event == EVENT_OBJECT_STATECHANGE || event == EVENT_OBJECT_LOCATIONCHANGE ||
            event == EVENT_OBJECT_CLOAKED || event == EVENT_OBJECT_UNCLOAKED)
        {
            if (object != OBJID_WINDOW || child != CHILDID_SELF) return;
            // Destroy notifications may carry an already-invalid HWND. Other
            // object events must describe a top-level window, not UI controls.
            if (event != EVENT_OBJECT_DESTROY && (!window || GetAncestor(window, GA_ROOT) != window)) return;
        }
        // Coalesce rapid switching. Sample the current foreground on delivery,
        // rather than applying stale HWNDs from an earlier event.
        if (s_owner) s_owner->Wake();
    }
    void BackgroundControl::Start(HWND control)
    {
        m_control = control; s_owner = this;
        m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        m_foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
        m_desktopSwitch = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH,
            nullptr, ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
        m_minimizeEnd = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
            nullptr, ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
        m_visibility = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE,
            nullptr, ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        m_windowState = SetWinEventHook(EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        m_cloaking = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED,
            nullptr, ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!m_foreground || !m_desktopSwitch || !m_minimizeEnd || !m_visibility || !m_windowState || !m_cloaking)
            throw platform::Error("Register desktop activity hooks", GetLastError());
        m_sessionRegistered = WTSRegisterSessionNotification(control, NOTIFY_FOR_THIS_SESSION) != FALSE;
        if (!m_sessionRegistered) throw platform::Error("Register session notifications", GetLastError());
        LPWSTR sessionData{}; DWORD sessionBytes{};
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
            WTSSessionInfoEx, &sessionData, &sessionBytes))
        {
            if (sessionBytes >= sizeof(WTSINFOEXW))
            {
                auto info = reinterpret_cast<WTSINFOEXW const*>(sessionData);
                if (info->Level == 1)
                {
                    m_policy.Set(core::PauseReason::SessionLocked,
                        info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK);
                    m_policy.Set(core::PauseReason::SessionDisconnected,
                        info->Data.WTSInfoExLevel1.SessionState == WTSDisconnected);
                }
            }
            WTSFreeMemory(sessionData);
        }
        // Registration delivers the initial display state as well as changes.
        m_display = RegisterPowerSettingNotification(control, &GUID_SESSION_DISPLAY_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (!m_display) platform::Log(L"engine", L"Session display power notifications unavailable; display-off policy is not available on this run.");
        RefreshDesktop();
        m_icon.cbSize = sizeof(m_icon); m_icon.hWnd = control; m_icon.uID = 1;
        m_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        m_icon.uCallbackMessage = TrayMessage;
        // Select the embedded small icon for the system tray; shared handles need no destruction.
        m_icon.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        m_tooltip = L"GlassWall - stopped";
        wcscpy_s(m_icon.szTip, m_tooltip.c_str());
        AddTray();
        if (!m_added) { m_trayRetries = 60; SetTimer(m_control, 0x4757, 1000, nullptr); }
    }
    void BackgroundControl::AddTray()
    {
        m_added = Shell_NotifyIconW(NIM_ADD, &m_icon) != FALSE;
        if (m_added)
        {
            m_icon.uVersion = NOTIFYICON_VERSION_4;
            if (!Shell_NotifyIconW(NIM_SETVERSION, &m_icon))
                platform::Log(L"engine", L"Tray version negotiation failed.");
        }
        else platform::Log(L"engine", L"Tray icon unavailable; bounded startup retry / TaskbarCreated.");
    }
    void BackgroundControl::RefreshDesktop()
    {
        m_policy.Set(core::PauseReason::DesktopInactive,
            !desktop::DesktopHost::IsDesktopUsable(GetForegroundWindow()));
    }
    bool BackgroundControl::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        using core::PauseReason;
        if (message == WM_TIMER && wParam == 0x4757)
        {
            if (!m_added && m_trayRetries) { --m_trayRetries; AddTray(); }
            if (m_added || !m_trayRetries) KillTimer(m_control, 0x4757);
            return true;
        }
        if (message == BackgroundChangedMessage)
        { m_wakePending = false; RefreshDesktop(); return true; }
        if (m_taskbarCreated && message == m_taskbarCreated)
        {
            m_added = false; AddTray();
            if (!m_added) { m_trayRetries = 60; SetTimer(m_control, 0x4757, 1000, nullptr); }
            RefreshDesktop(); Wake(); return false;
        }
        if (message == WM_WTSSESSION_CHANGE)
        {
            if (wParam == WTS_SESSION_LOCK)
                m_policy.Set(PauseReason::SessionLocked, true);
            if (wParam == WTS_SESSION_UNLOCK)
                m_policy.Set(PauseReason::SessionLocked, false);
            if (wParam == WTS_CONSOLE_DISCONNECT || wParam == WTS_REMOTE_DISCONNECT)
                m_policy.Set(PauseReason::SessionDisconnected, true);
            if (wParam == WTS_CONSOLE_CONNECT || wParam == WTS_REMOTE_CONNECT)
                m_policy.Set(PauseReason::SessionDisconnected, false);
            RefreshDesktop(); Wake(); return true;
        }
        if (message == WM_POWERBROADCAST)
        {
            if (wParam == PBT_APMSUSPEND)
            {
                if (!m_policy.Has(PauseReason::SystemSuspended)) m_sleepStarted = GetTickCount64();
                m_policy.Set(PauseReason::SystemSuspended, true);
            }
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
            {
                if (m_policy.Has(PauseReason::SystemSuspended)) m_sleepDuration += GetTickCount64() - m_sleepStarted;
                m_policy.Set(PauseReason::SystemSuspended, false);
            }
            if (wParam == PBT_POWERSETTINGCHANGE && lParam)
            {
                auto setting = reinterpret_cast<POWERBROADCAST_SETTING const*>(lParam);
                if (IsEqualGUID(setting->PowerSetting, GUID_SESSION_DISPLAY_STATUS) && setting->DataLength == sizeof(DWORD))
                {
                    DWORD state{}; std::memcpy(&state, setting->Data, sizeof(state));
                    m_policy.Set(PauseReason::DisplayOff, state == 0); // dimmed is still on
                }
            }
            RefreshDesktop(); Wake(); return true;
        }
        if (message == TrayMessage)
        {
            auto event = LOWORD(lParam);
            if (event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_RBUTTONUP) Menu();
            return true;
        }
        return false;
    }
    void BackgroundControl::Menu()
    {
        auto menu = CreatePopupMenu();
        if (!menu) return;
        auto flags = MF_STRING | (m_loaded ? 0u : MF_GRAYED);
        AppendMenuW(menu, flags, TrayToggle, m_manual ? L"Resume" : L"Pause");
        AppendMenuW(menu, flags, TrayStop, L"Stop Wallpaper");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, TrayExit, L"Exit GlassWall");
        POINT point{}; GetCursorPos(&point);
        SetForegroundWindow(m_control);
        auto choice = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, m_control, nullptr);
        DestroyMenu(menu);
        PostMessageW(m_control, WM_NULL, 0, 0);
        if (choice) PostMessageW(m_control, TrayCommandMessage, static_cast<WPARAM>(choice), 0);
        RefreshDesktop(); Wake();
    }
    ULONGLONG BackgroundControl::TakeSleepDuration() noexcept
    { auto duration = m_sleepDuration; m_sleepDuration = 0; return duration; }
    void BackgroundControl::UpdateTray(ipc::PlaybackState state, bool manual, bool loaded)
    {
        m_manual = manual; m_loaded = loaded;
        auto text = std::wstring(L"GlassWall - ") +
            (state == ipc::PlaybackState::Playing ? L"playing" : manual ? L"manually paused" :
             state == ipc::PlaybackState::Paused ? L"automatically suspended" :
             state == ipc::PlaybackState::Recovering ? L"recovering" :
             state == ipc::PlaybackState::Error || state == ipc::PlaybackState::HostUnavailable ? L"unavailable" : L"stopped / ready");
        if (text == m_tooltip) return;
        m_tooltip = std::move(text); wcscpy_s(m_icon.szTip, m_tooltip.c_str());
        if (m_added) { m_icon.uFlags = NIF_TIP | NIF_SHOWTIP; Shell_NotifyIconW(NIM_MODIFY, &m_icon); }
        m_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    }
    void BackgroundControl::Reset() noexcept
    {
        if (m_control) KillTimer(m_control, 0x4757);
        if (m_foreground) UnhookWinEvent(m_foreground);
        if (m_desktopSwitch) UnhookWinEvent(m_desktopSwitch);
        if (m_minimizeEnd) UnhookWinEvent(m_minimizeEnd);
        if (m_visibility) UnhookWinEvent(m_visibility);
        if (m_windowState) UnhookWinEvent(m_windowState);
        if (m_cloaking) UnhookWinEvent(m_cloaking);
        if (m_display) UnregisterPowerSettingNotification(m_display);
        if (m_sessionRegistered) WTSUnRegisterSessionNotification(m_control);
        if (m_added) Shell_NotifyIconW(NIM_DELETE, &m_icon);
        m_foreground = m_desktopSwitch = m_minimizeEnd = nullptr; m_display = nullptr;
        m_visibility = nullptr;
        m_windowState = m_cloaking = nullptr;
        m_sessionRegistered = m_added = false; m_control = nullptr;
        if (s_owner == this) s_owner = nullptr;
    }
}
