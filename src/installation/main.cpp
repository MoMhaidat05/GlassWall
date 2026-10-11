#include "../ipc/Client.h"
#include "../platform/win32/Autostart.h"
#include <filesystem>
#include <vector>
#include <string_view>
#include <shellapi.h>

// Short-lived GUI-subsystem maintenance tool. Never used for sign-in or playback.
namespace
{
    using namespace glasswall;
    bool SameExecutable(HANDLE process, std::filesystem::path const& expected)
    {
        wchar_t buffer[32768]{}; DWORD size = static_cast<DWORD>(std::size(buffer));
        if (!QueryFullProcessImageNameW(process, 0, buffer, &size)) return false;
        std::error_code error;
        return std::filesystem::equivalent(expected, std::filesystem::path(buffer), error) && !error;
    }
    void ShutdownInstallation(std::filesystem::path const& directory)
    {
        struct Windows
        {
            std::filesystem::path manager;
            std::vector<platform::Handle> processes;
        } windows{directory / L"GlassWall.exe", {}};
        // Request the manager's normal close path; no focus changes or force kills.
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL
        {
            auto& found = *reinterpret_cast<Windows*>(parameter);
            DWORD pid{}; GetWindowThreadProcessId(window, &pid);
            platform::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
            if (process && SameExecutable(process.get(), found.manager))
            {
                PostMessageW(window, WM_CLOSE, 0, 0);
                found.processes.push_back(std::move(process));
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&windows));
        for (auto const& process : windows.processes)
            if (WaitForSingleObject(process.get(), 10000) != WAIT_OBJECT_0)
                throw platform::Error("Close installed manager before maintenance", ERROR_BUSY);

        auto reply = ipc::Query();
        if (!reply && reply.error == ERROR_FILE_NOT_FOUND) return;
        if (!reply) throw platform::Error("Contact engine before maintenance", reply.error);
        platform::Handle engine(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, reply.response.processId));
        DWORD session{}, ownSession{};
        if (!engine || !ProcessIdToSessionId(reply.response.processId, &session) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) || session != ownSession ||
            !SameExecutable(engine.get(), directory / L"GlassWall.Engine.exe"))
            throw platform::Error("Running engine belongs to another folder; exit it using its tray", ERROR_INVALID_OWNER);
        auto stopped = ipc::Shutdown();
        if (!stopped) throw platform::Error("Request engine shutdown", stopped.error);
        if (WaitForSingleObject(engine.get(), 10000) != WAIT_OBJECT_0)
            throw platform::Error("Wait for engine shutdown", ERROR_BUSY);
    }
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    using namespace glasswall;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try
    {
        std::wstring_view command(commandLine);
        if (command == L"--shutdown") ShutdownInstallation(platform::ExecutableDirectory());
        else if (command.starts_with(L"--shutdown-directory "))
        {
            int count{};
            auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
            if (!arguments) return static_cast<int>(GetLastError());
            std::filesystem::path directory;
            if (count == 3) directory = arguments[2];
            LocalFree(arguments);
            if (directory.empty() || !directory.is_absolute()) return ERROR_INVALID_PARAMETER;
            ShutdownInstallation(directory);
        }
        else if (command == L"--configure-startup") platform::ConfigureInstalledStartup();
        else if (command == L"--remove-startup") platform::RemoveStartupRegistration();
        else if (command == L"--enable-startup") platform::SetStartWithWindows(true);
        else if (command == L"--disable-startup") platform::SetStartWithWindows(false);
        else return ERROR_INVALID_PARAMETER;
        return 0;
    }
    catch (platform::Error const& error)
    { platform::Log(L"setup", std::wstring(error.what(), error.what() + std::char_traits<char>::length(error.what()))); return static_cast<int>(error.code()); }
    catch (...)
    { platform::Log(L"setup", L"Installation maintenance failed."); return ERROR_GEN_FAILURE; }
}
