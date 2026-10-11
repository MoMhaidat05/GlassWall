#include "Autostart.h"
#include <filesystem>

namespace glasswall::platform
{
    namespace
    {
        constexpr wchar_t PreferenceKey[] = L"Software\\GlassWall";
        constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        struct Key
        {
            HKEY value{};
            ~Key() { if (value) RegCloseKey(value); }
        };
        void Check(LSTATUS result, char const* operation)
        { if (result != ERROR_SUCCESS) throw Error(operation, static_cast<DWORD>(result)); }
        void RegisterStartup(bool enabled)
        {
            Key run;
            Check(RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE,
                nullptr, &run.value, nullptr), "Open per-user startup registration");
            if (enabled)
            {
                auto engine = std::filesystem::path(ExecutableDirectory()) / L"GlassWall.Engine.exe";
                if (!std::filesystem::is_regular_file(engine))
                    throw Error("Locate installed wallpaper engine", ERROR_FILE_NOT_FOUND);
                auto command = L"\"" + engine.wstring() + L"\" --startup";
                // Run command lines are documented as limited to 260 characters.
                if (command.size() > 260) throw Error("Startup command too long", ERROR_FILENAME_EXCED_RANGE);
                Check(RegSetValueExW(run.value, L"GlassWall", 0, REG_SZ,
                    reinterpret_cast<BYTE const*>(command.c_str()),
                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))), "Register GlassWall startup");
            }
            else
            {
                auto result = RegDeleteValueW(run.value, L"GlassWall");
                if (result != ERROR_FILE_NOT_FOUND) Check(result, "Remove GlassWall startup");
            }
        }
    }
    bool StartWithWindows()
    {
        DWORD enabled{1}, bytes = sizeof(enabled);
        auto result = RegGetValueW(HKEY_CURRENT_USER, PreferenceKey, L"StartWithWindows",
            RRF_RT_REG_DWORD, nullptr, &enabled, &bytes);
        if (result == ERROR_FILE_NOT_FOUND) return true;
        Check(result, "Read startup preference");
        return enabled != 0;
    }
    void SetStartWithWindows(bool enabled)
    {
        auto previous = StartWithWindows();
        Key preference;
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, PreferenceKey, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &preference.value, nullptr), "Open startup preference");
        RegisterStartup(enabled);
        DWORD value = enabled ? 1u : 0u;
        auto result = RegSetValueExW(preference.value, L"StartWithWindows", 0, REG_DWORD,
            reinterpret_cast<BYTE const*>(&value), sizeof(value));
        if (result != ERROR_SUCCESS)
        {
            try { RegisterStartup(previous); } catch (...) {}
            Check(result, "Save startup preference");
        }
    }
    void ConfigureInstalledStartup() { SetStartWithWindows(StartWithWindows()); }
    void RemoveStartupRegistration() { RegisterStartup(false); }
}
