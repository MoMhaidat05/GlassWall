#include "PipeServer.h"
#include "WallpaperRuntime.h"
#include "../platform/win32/Support.h"
#include <thread>
#include <string_view>

namespace
{
    struct OwnedWindow
    {
        HWND value{};
        ~OwnedWindow() { if (value && IsWindow(value)) DestroyWindow(value); }
    };
    LRESULT CALLBACK ControlProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto runtime = reinterpret_cast<glasswall::engine::WallpaperRuntime*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (runtime && runtime->HandleWindowMessage(message, wParam, lParam))
            return message == WM_POWERBROADCAST ? TRUE : 0;
        if (runtime && message == glasswall::engine::TrayCommandMessage)
        {
            using namespace glasswall;
            if (wParam == engine::TrayExit) PostQuitMessage(0);
            else if (wParam == engine::TrayStop) runtime->SubmitTray(ipc::Command::StopWallpaper);
            else if (wParam == engine::TrayToggle)
            {
                auto state = runtime->Snapshot();
                runtime->SubmitTray((state.pauseReasons & static_cast<std::uint32_t>(core::PauseReason::User)) ?
                    ipc::Command::Resume : ipc::Command::Pause);
            }
            return 0;
        }
        if (message == glasswall::engine::ShutdownMessage || message == WM_CLOSE ||
            (message == WM_ENDSESSION && wParam))
        {
            PostQuitMessage(0);
            return 0;
        }
        if (message == glasswall::engine::FailureMessage)
        {
            PostQuitMessage(1);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
    using namespace glasswall;
    bool silent = std::wstring_view(commandLine) == L"--startup";
    if (silent) SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!platform::IsSupportedWindows())
    {
        if (!silent) MessageBoxW(nullptr, L"GlassWall targets Windows 11 desktop clients (build 22000 or later).",
            L"GlassWall Engine", MB_OK | MB_ICONERROR);
        return 1;
    }
    try
    {
        auto identity = platform::CurrentIdentity();
        platform::UserSecurity security(identity.sid);
        SetLastError(ERROR_SUCCESS);
        platform::Handle mutex(CreateMutexW(security.attributes(), FALSE, identity.mutexName.c_str()));
        auto mutexResult = GetLastError();
        if (!mutex) throw platform::Error("CreateMutexW", mutexResult);
        if (mutexResult == ERROR_ALREADY_EXISTS) return 0;
        // A second interactive session for the same user must not retain another
        // engine or contend for the shared catalog. Global mutexes need no service.
        auto userMutexName = L"Global\\GlassWall.Engine." + identity.sid;
        platform::Handle userMutex(CreateMutexW(security.attributes(), FALSE, userMutexName.c_str()));
        auto userMutexResult = GetLastError();
        if (!userMutex) throw platform::Error("Create per-user engine mutex", userMutexResult);
        if (userMutexResult == ERROR_ALREADY_EXISTS) return 0;
        platform::Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop) throw platform::Error("CreateEventW", GetLastError());
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.hInstance = instance;
        type.lpfnWndProc = ControlProcedure;
        type.lpszClassName = L"GlassWall.Stage1.EngineControl";
        if (!RegisterClassExW(&type)) throw platform::Error("RegisterClassExW", GetLastError());
        HWND control = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName,
            L"GlassWall Stage 1 Engine", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!control) throw platform::Error("CreateWindowExW", GetLastError());
        OwnedWindow owned{ control };
        engine::WallpaperRuntime runtime(instance, stop.get());
        platform::Log(L"engine", L"Starting native GlassWall engine. Graphics/media initialize only for media validation or remembered wallpaper restore.");
        std::jthread server([&] { engine::Serve(identity, stop.get(), control, runtime); });
        int result{};
        try
        {
            result = runtime.Run(control);
        }
        catch (...) { SetEvent(stop.get()); throw; } // jthread joins before runtime/handles are destroyed
        SetEvent(stop.get());
        server.join();
        DestroyWindow(control); owned.value = nullptr;
        UnregisterClassW(type.lpszClassName, instance);
        platform::Log(L"engine", L"Shutdown complete.");
        return result;
    }
    catch (platform::Failure const& failure)
    {
        auto description = failure.operation + L": " + platform::HexResult(failure.code);
        platform::Log(L"engine", description);
        if (!silent) MessageBoxW(nullptr, description.c_str(), L"GlassWall Engine failure", MB_OK | MB_ICONERROR);
    }
    catch (platform::Error const& error)
    {
        auto description = platform::ErrorText(error.code());
        platform::Log(L"engine", description);
        if (!silent) MessageBoxW(nullptr, description.c_str(), L"GlassWall Engine startup failed", MB_OK | MB_ICONERROR);
    }
    catch (...)
    {
        platform::Log(L"engine", L"Unexpected startup failure.");
        if (!silent) MessageBoxW(nullptr, L"Unexpected native engine startup failure.", L"GlassWall Engine",
            MB_OK | MB_ICONERROR);
    }
    return 1;
}
