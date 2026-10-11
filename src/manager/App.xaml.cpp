#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.h"
#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif
#include "platform/win32/Support.h"
#include "platform/win32/HResult.h"
#include <ShObjIdl.h>

namespace winrt::GlassWall::implementation
{
    App::App()
    {
        // Unpackaged runtime/resource loading must work when Start, Explorer or a
        // caller supplies an unrelated current directory. Paths for wallpapers
        // and user data remain absolute and independent of this process directory.
        auto directory = ::glasswall::platform::ExecutableDirectory();
        if (!SetCurrentDirectoryW(directory.c_str()))
            throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        // Install before C++/WinRT make() loads App.xaml after construction.
        // Preserve WinUI's failure semantics; this reports the actual parse error.
        UnhandledException([](auto const&, Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& error)
        {
            ::glasswall::platform::Log(L"manager", L"XAML unhandled " +
                ::glasswall::platform::HexResult(error.Exception()) + L": " + std::wstring(error.Message()));
        });
        ::glasswall::platform::Log(L"manager", L"App constructed; loading App.xaml next.");
    }

    void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        SetCurrentProcessExplicitAppUserModelID(L"GlassWall.Personal");
        ::glasswall::platform::Log(L"manager", L"App.xaml loaded; constructing MainWindow.xaml.");
        if (!::glasswall::platform::IsSupportedWindows())
        {
            MessageBoxW(nullptr, L"GlassWall targets Windows 11 desktop clients (build 22000 or later).",
                L"GlassWall", MB_OK | MB_ICONERROR);
            Exit();
            return;
        }
        try
        {
            m_window = make<MainWindow>();
            m_window.Activate();
            ::glasswall::platform::Log(L"manager", L"Management window activated; initialization succeeded.");
        }
        catch (winrt::hresult_error const& error)
        {
            ::glasswall::platform::Log(L"manager", L"Startup initialization failed " +
                ::glasswall::platform::HexResult(error.code()) + L": " + std::wstring(error.message()));
            MessageBoxW(nullptr, error.message().c_str(), L"GlassWall startup failed", MB_OK | MB_ICONERROR);
            // WinUI Exit() has no exit-code argument and reports success (0).
            // Initialization is fatal; the independent wallpaper engine survives.
            ::ExitProcess(static_cast<UINT>(error.code()));
        }
    }
}
