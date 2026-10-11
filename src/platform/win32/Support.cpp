#include "Support.h"
#include <sddl.h>
#include <ShlObj.h>
#include <filesystem>
#include <cstddef>
#include <cwchar>
#include <mutex>
#include <vector>

namespace glasswall::platform
{
    Identity CurrentIdentity()
    {
        HANDLE rawToken{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
            throw Error("OpenProcessToken", GetLastError());
        Handle token(rawToken);
        DWORD size{};
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            throw Error("GetTokenInformation(size)", GetLastError());
        std::vector<std::byte> buffer(size);
        if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size))
            throw Error("GetTokenInformation", GetLastError());
        auto user = reinterpret_cast<TOKEN_USER const*>(buffer.data());
        LPWSTR rawSid{};
        if (!ConvertSidToStringSidW(user->User.Sid, &rawSid))
            throw Error("ConvertSidToStringSidW", GetLastError());
        std::wstring sid(rawSid);
        LocalFree(rawSid);
        DWORD session{};
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
            throw Error("ProcessIdToSessionId", GetLastError());
        auto suffix = sid + L"." + std::to_wstring(session);
        return { sid, session, L"\\\\.\\pipe\\GlassWall.Stage1.v1." + suffix,
            L"Local\\GlassWall.Stage1.v1." + suffix };
    }

    UserSecurity::UserSecurity(std::wstring const& sid)
    {
        auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &m_descriptor, nullptr))
            throw Error("ConvertStringSecurityDescriptor", GetLastError());
        m_attributes.nLength = sizeof(m_attributes);
        m_attributes.lpSecurityDescriptor = m_descriptor;
        m_attributes.bInheritHandle = FALSE;
    }

    UserSecurity::~UserSecurity() { if (m_descriptor) LocalFree(m_descriptor); }

    bool IsSupportedWindows() noexcept
    {
        OSVERSIONINFOEXW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        version.dwMajorVersion = 10;
        version.dwMinorVersion = 0;
        version.dwBuildNumber = 22000;
        version.wProductType = VER_NT_WORKSTATION;
        constexpr DWORD typeMask = VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER |
            VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR | VER_PRODUCT_TYPE;
        DWORDLONG mask{};
        mask = VerSetConditionMask(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
        mask = VerSetConditionMask(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
        mask = VerSetConditionMask(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
        // A major-version test also requires minor and service-pack comparisons (both packs are zero).
        mask = VerSetConditionMask(mask, VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
        mask = VerSetConditionMask(mask, VER_SERVICEPACKMINOR, VER_GREATER_EQUAL);
        mask = VerSetConditionMask(mask, VER_PRODUCT_TYPE, VER_EQUAL);
        return VerifyVersionInfoW(&version, typeMask, mask) != FALSE;
    }

    std::wstring ExecutableDirectory()
    {
        std::vector<wchar_t> buffer(32768);
        auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
            throw Error("GetModuleFileNameW", length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path().wstring();
    }

    std::wstring ErrorText(DWORD code)
    {
        LPWSTR buffer{};
        auto count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
            reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        std::wstring text = count ? std::wstring(buffer, count) : L"Unknown Windows error";
        if (buffer) LocalFree(buffer);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
        return L"Win32 " + std::to_wstring(code) + L": " + text;
    }

    void Log(std::wstring const& component, std::wstring const& message) noexcept
    {
        try
        {
            static std::mutex lock;
            std::scoped_lock guard(lock);
            SYSTEMTIME time{};
            GetSystemTime(&time);
            wchar_t prefix[128]{};
            swprintf_s(prefix, L"%04u-%02u-%02uT%02u:%02u:%02uZ [%lu] ",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                GetCurrentProcessId());
            auto line = std::wstring(prefix) + component + L": " + message + L"\r\n";
            OutputDebugStringW(line.c_str());
            PWSTR rawPath{};
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath))) return;
            std::filesystem::path directory(rawPath);
            CoTaskMemFree(rawPath);
            directory /= L"GlassWall";
            directory /= L"Logs";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) return;
            auto path = directory / (component + L".log");
            auto size = std::filesystem::file_size(path, error);
            if (!error && size > 256 * 1024)
                MoveFileExW(path.c_str(), (directory / (component + L".previous.log")).c_str(),
                    MOVEFILE_REPLACE_EXISTING);
            Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file) return;
            int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                nullptr, 0, nullptr, nullptr);
            if (bytes <= 0) return;
            std::string utf8(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                utf8.data(), bytes, nullptr, nullptr);
            DWORD written{};
            WriteFile(file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
        catch (...) { /* Diagnostics must not interrupt lifecycle or IPC. */ }
    }
}
