#include "DesktopHost.h"
#include "../platform/win32/HResult.h"
#include <filesystem>
#include <cwchar>
#include <dwmapi.h>

namespace glasswall::desktop
{
    namespace
    {
        constexpr UINT CreateWorkerMessage = 0x052C; // undocumented Explorer contract
        constexpr wchar_t WindowClass[] = L"GlassWall.NativeWallpaper";
        bool OwnedBy(HWND window, DWORD process)
        {
            DWORD actual{};
            return IsWindow(window) && GetWindowThreadProcessId(window, &actual) && actual == process;
        }
        bool ClassIs(HWND window, wchar_t const* name)
        {
            wchar_t actual[128]{};
            return GetClassNameW(window, actual, 128) && wcscmp(actual, name) == 0;
        }
        HWND IconView(HWND root)
        {
            return FindWindowExW(root, nullptr, L"SHELLDLL_DefView", nullptr);
        }
        bool ShellSurface(HWND window, DWORD process)
        {
            return OwnedBy(window, process) && (ClassIs(window, L"Progman") || ClassIs(window, L"WorkerW") ||
                ClassIs(window, L"Shell_TrayWnd") || ClassIs(window, L"Shell_SecondaryTrayWnd"));
        }
        bool BehindSibling(HWND above, HWND below)
        {
            for (auto next = GetWindow(above, GW_HWNDNEXT); next; next = GetWindow(next, GW_HWNDNEXT))
                if (next == below) return true;
            return false;
        }
        bool CoversPrimary(HWND window)
        {
            auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            RECT rectangle{};
            MONITORINFO monitor{ sizeof(monitor) };
            auto primary = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
            bool covers = GetWindowRect(window, &rectangle) && GetMonitorInfoW(primary, &monitor) &&
                rectangle.left <= monitor.rcMonitor.left && rectangle.top <= monitor.rcMonitor.top &&
                rectangle.right >= monitor.rcMonitor.right && rectangle.bottom >= monitor.rcMonitor.bottom;
            if (previous) SetThreadDpiAwarenessContext(previous);
            return covers;
        }
        LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_NCCREATE)
            {
                auto creation = reinterpret_cast<CREATESTRUCTW*>(lParam);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
            }
            auto control = reinterpret_cast<HWND>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
            if (message == WM_NCHITTEST) return HTTRANSPARENT;
            if (message == WM_ERASEBKGND) return 1;
            if (message == WM_PAINT)
            {
                PAINTSTRUCT paint{};
                BeginPaint(window, &paint);
                EndPaint(window, &paint);
                return 0;
            }
            if (control && message == WM_SIZE)
                PostMessageW(control, HostChangedMessage, reinterpret_cast<WPARAM>(window), lParam);
            if (control && message == WM_NCDESTROY)
                PostMessageW(control, HostLostMessage, reinterpret_cast<WPARAM>(window), 0);
            return DefWindowProcW(window, message, wParam, lParam);
        }
        struct DpiScope
        {
            DPI_AWARENESS_CONTEXT previous{};
            explicit DpiScope(HWND parent)
            {
                previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(parent));
                if (!previous) throw platform::Error("SetThreadDpiAwarenessContext", GetLastError());
            }
            ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
        };
    }

    void DesktopHost::Create(HINSTANCE instance, HWND control)
    {
        Reset();
        platform::Log(L"engine", L"stage=DesktopHost.Create; discovering and validating Explorer parent/icon topology.");
        auto shell = GetShellWindow();
        if (!shell || !ClassIs(shell, L"Progman")) throw platform::Failure(E_FAIL, L"Explorer Progman not available");
        GetWindowThreadProcessId(shell, &m_shellPid);
        platform::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_shellPid));
        wchar_t executable[32768]{};
        DWORD length = 32768, shellSession{}, ownSession{};
        if (!process || !QueryFullProcessImageNameW(process.get(), 0, executable, &length) ||
            _wcsicmp(std::filesystem::path(executable).filename().c_str(), L"explorer.exe") != 0 ||
            !ProcessIdToSessionId(m_shellPid, &shellSession) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) || shellSession != ownSession)
            throw platform::Failure(E_ACCESSDENIED, L"Desktop shell process/session validation");

        DWORD_PTR ignored{};
        SetLastError(ERROR_SUCCESS);
        if (!SendMessageTimeoutW(shell, CreateWorkerMessage, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored))
            throw platform::Failure(HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_TIMEOUT),
                L"Explorer WorkerW discovery request");

        struct Search { DWORD pid; HWND parent{}, icons{}; } search{ m_shellPid };
        EnumWindows([](HWND root, LPARAM parameter) -> BOOL
        {
            auto& found = *reinterpret_cast<Search*>(parameter);
            if (!OwnedBy(root, found.pid)) return TRUE;
            auto icons = IconView(root);
            if (!icons || !OwnedBy(icons, found.pid)) return TRUE;
            for (auto candidate = GetWindow(root, GW_HWNDNEXT); candidate; candidate = GetWindow(candidate, GW_HWNDNEXT))
            {
                if (OwnedBy(candidate, found.pid) && ClassIs(candidate, L"WorkerW") &&
                    !IconView(candidate) && CoversPrimary(candidate))
                {
                    found.parent = candidate; found.icons = icons; return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        if (search.parent)
        {
            m_parent = search.parent;
            m_icons = search.icons;
        }
        else
        {
            // Modern layouts can put the background WorkerW inside Progman.
            auto icons = IconView(shell);
            if (icons && OwnedBy(icons, m_shellPid) &&
                FindWindowExW(icons, nullptr, L"SysListView32", nullptr))
            {
                for (auto candidate = GetWindow(icons, GW_HWNDNEXT); candidate; candidate = GetWindow(candidate, GW_HWNDNEXT))
                    if (OwnedBy(candidate, m_shellPid) && ClassIs(candidate, L"WorkerW") &&
                        GetParent(candidate) == shell && !IconView(candidate) && CoversPrimary(candidate))
                    { m_parent = candidate; m_icons = icons; break; }
                // Only accept direct Progman hosting for the recognized redirected
                // Windows 11 layout with an explicit icon sibling above our child.
                if (!m_parent && (GetWindowLongPtrW(shell, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) &&
                    CoversPrimary(shell))
                {
                    m_parent = shell; m_icons = icons; m_directProgman = true;
                }
            }
        }
        if (!m_parent || !m_icons || !OwnedBy(m_parent, m_shellPid) ||
            !FindWindowExW(m_icons, nullptr, L"SysListView32", nullptr))
            throw platform::Failure(E_FAIL, L"No validated desktop layer behind icons");

        DpiScope dpi(m_parent);
        MONITORINFO monitor{ sizeof(monitor) };
        if (!GetMonitorInfoW(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY), &monitor))
            throw platform::Error("GetMonitorInfoW", GetLastError());
        POINT points[] = { { monitor.rcMonitor.left, monitor.rcMonitor.top },
            { monitor.rcMonitor.right, monitor.rcMonitor.bottom } };
        SetLastError(ERROR_SUCCESS);
        if (!MapWindowPoints(HWND_DESKTOP, m_parent, points, 2) && GetLastError())
            throw platform::Error("MapWindowPoints", GetLastError());
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type); type.hInstance = instance;
        type.lpfnWndProc = Procedure; type.lpszClassName = WindowClass;
        if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw platform::Error("Register wallpaper class", GetLastError());
        // Never create a top-level/popup wallpaper or reparent Explorer-owned windows.
        m_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WindowClass, L"GlassWall Wallpaper",
            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, points[0].x, points[0].y,
            points[1].x - points[0].x, points[1].y - points[0].y,
            m_parent, nullptr, instance, control);
        if (!m_window) throw platform::Error("Create wallpaper child", GetLastError());
        RECT client{};
        if (!GetClientRect(m_window, &client)) throw platform::Error("Get wallpaper client size", GetLastError());
        m_width = client.right; m_height = client.bottom;
        if (!SetWindowPos(m_window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) ||
            !IsValid())
        {
            Reset();
            throw platform::Failure(E_FAIL, L"Wallpaper parenting/z-order validation");
        }
        platform::Log(L"engine", L"Desktop host attached: Explorer PID " + std::to_wstring(m_shellPid) +
            (m_directProgman ? L"; guarded Progman-child layout." : L"; background WorkerW layout."));
    }

    bool DesktopHost::IsValid() const noexcept
    {
        if (!OwnedBy(m_parent, m_shellPid) || !OwnedBy(m_icons, m_shellPid) ||
            !OwnedBy(m_window, GetCurrentProcessId()) || !ClassIs(m_window, WindowClass) ||
            !ClassIs(m_icons, L"SHELLDLL_DefView") ||
            !ClassIs(m_parent, m_directProgman ? L"Progman" : L"WorkerW") ||
            GetParent(m_window) != m_parent) return false;
        if (m_directProgman) return GetParent(m_icons) == m_parent && BehindSibling(m_icons, m_window);
        auto iconRoot = GetAncestor(m_icons, GA_ROOT);
        return GetParent(m_parent) == GetParent(m_icons) ?
            BehindSibling(m_icons, m_parent) : BehindSibling(iconRoot, m_parent);
    }
    bool DesktopHost::IsShellReady() noexcept
    {
        auto shell = GetShellWindow();
        DWORD pid{};
        if (!shell || !ClassIs(shell, L"Progman") || !GetWindowThreadProcessId(shell, &pid)) return false;
        struct Search { DWORD pid; bool ready{}; } search{pid};
        EnumWindows([](HWND root, LPARAM parameter) -> BOOL
        {
            auto& found = *reinterpret_cast<Search*>(parameter);
            auto icons = OwnedBy(root, found.pid) ? IconView(root) : nullptr;
            if (icons && FindWindowExW(icons, nullptr, L"SysListView32", nullptr))
            { found.ready = true; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.ready && FindWindowW(L"Shell_TrayWnd", nullptr);
    }
    bool DesktopHost::IsDesktopForeground(HWND window) noexcept
    {
        auto shell = GetShellWindow();
        if (!window || !shell || !ClassIs(shell, L"Progman")) return false;
        DWORD pid{}; GetWindowThreadProcessId(shell, &pid);
        auto root = GetAncestor(window, GA_ROOT);
        if (!OwnedBy(root, pid)) return false;
        if (root == shell) return true;
        // An Explorer process also owns normal CabinetWClass file windows.
        // Only accept the actual icon-bearing desktop WorkerW topology.
        auto icons = ClassIs(root, L"WorkerW") ? IconView(root) : nullptr;
        return icons && OwnedBy(icons, pid) &&
            FindWindowExW(icons, nullptr, L"SysListView32", nullptr) != nullptr;
    }
    bool DesktopHost::IsDesktopUsable(HWND foreground) noexcept
    {
        auto shell = GetShellWindow();
        DWORD shellPid{};
        if (!shell || !ClassIs(shell, L"Progman") || !GetWindowThreadProcessId(shell, &shellPid)) return false;
        auto root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
        if (root && IsWindowVisible(root) && !IsIconic(root) && !ShellSurface(root, shellPid))
        {
            DWORD cloaked{};
            if (FAILED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || !cloaked) return false;
        }

        // Desktop, NULL, stale minimized/hidden foreground, or taskbar focus need a
        // visibility check. Run once per coalesced event, never per frame.
        auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        MONITORINFO monitor{ sizeof(monitor) };
        if (!GetMonitorInfoW(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY), &monitor))
        { if (previousDpi) SetThreadDpiAwarenessContext(previousDpi); return false; }
        struct Search { DWORD shellPid; RECT work; bool clear{ true }; } search{ shellPid, monitor.rcWork };
        auto enumerated = EnumWindows([](HWND window, LPARAM parameter) -> BOOL
        {
            auto& found = *reinterpret_cast<Search*>(parameter);
            if (!IsWindowVisible(window) || IsIconic(window) || ShellSurface(window, found.shellPid)) return TRUE;
            DWORD cloaked{};
            if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
            RECT bounds{}, overlap{};
            if (!GetWindowRect(window, &bounds) || IntersectRect(&overlap, &bounds, &found.work))
            { found.clear = false; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
        return enumerated && search.clear;
    }
    bool DesktopHost::IsVisible() const noexcept { return IsValid() && IsWindowVisible(m_window); }
    bool DesktopHost::GeometryMatches(LPARAM size) const noexcept
    { return LOWORD(size) == m_width && HIWORD(size) == m_height; }
    void DesktopHost::Show()
    {
        if (!IsValid()) throw platform::Failure(E_FAIL, L"Desktop host lost before show");
        if (!SetWindowPos(m_window, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE) || !IsValid())
        {
            ShowWindow(m_window, SW_HIDE);
            throw platform::Failure(E_FAIL, L"Wallpaper show/z-order validation failed");
        }
    }
    void DesktopHost::Reset() noexcept
    {
        if (OwnedBy(m_window, GetCurrentProcessId()) && ClassIs(m_window, WindowClass))
        {
            SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
            ShowWindow(m_window, SW_HIDE);
            DestroyWindow(m_window);
            if (OwnedBy(m_parent, m_shellPid))
                RedrawWindow(m_parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
        m_window = m_parent = m_icons = nullptr;
        m_shellPid = 0; m_directProgman = false;
        m_width = m_height = 0;
    }
}
