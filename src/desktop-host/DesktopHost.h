#pragma once
#include "../platform/win32/Support.h"

namespace glasswall::desktop
{
    inline constexpr UINT HostLostMessage = WM_APP + 10;
    inline constexpr UINT HostChangedMessage = WM_APP + 11;
    // All private shell names, topology rules and messages stay in this adapter.
    class DesktopHost
    {
    public:
        DesktopHost() = default;
        ~DesktopHost() { Reset(); }
        DesktopHost(DesktopHost const&) = delete;
        DesktopHost& operator=(DesktopHost const&) = delete;
        void Create(HINSTANCE instance, HWND control);
        bool IsValid() const noexcept;
        bool IsVisible() const noexcept;
        bool GeometryMatches(LPARAM size) const noexcept;
        void Show();
        void Reset() noexcept;
        static bool IsDesktopForeground(HWND window) noexcept;
        static bool IsShellReady() noexcept;
        static bool IsDesktopUsable(HWND foreground) noexcept;
        HWND Window() const noexcept { return m_window; }
    private:
        HWND m_window{}, m_parent{}, m_icons{};
        DWORD m_shellPid{};
        bool m_directProgman{};
        int m_width{}, m_height{};
    };
}
