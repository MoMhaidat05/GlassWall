#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace glasswall::platform
{
    class Handle
    {
    public:
        explicit Handle(HANDLE value = nullptr) noexcept : m_value(value) {}
        ~Handle() { reset(); }
        Handle(Handle const&) = delete;
        Handle& operator=(Handle const&) = delete;
        Handle(Handle&& other) noexcept : m_value(other.release()) {}
        Handle& operator=(Handle&& other) noexcept
        {
            if (this != &other) reset(other.release());
            return *this;
        }
        HANDLE get() const noexcept { return m_value; }
        explicit operator bool() const noexcept
        {
            return m_value != nullptr && m_value != INVALID_HANDLE_VALUE;
        }
        HANDLE release() noexcept { return std::exchange(m_value, nullptr); }
        void reset(HANDLE value = nullptr) noexcept
        {
            if (*this) CloseHandle(m_value);
            m_value = value;
        }
    private:
        HANDLE m_value;
    };

    class Error : public std::runtime_error
    {
    public:
        Error(char const* operation, DWORD code) :
            std::runtime_error(std::string(operation) + " failed (Win32 " + std::to_string(code) + ")"),
            m_code(code) {}
        DWORD code() const noexcept { return m_code; }
    private:
        DWORD m_code;
    };

    struct Identity
    {
        std::wstring sid;
        DWORD session{};
        std::wstring pipeName;
        std::wstring mutexName;
    };

    Identity CurrentIdentity();
    bool IsSupportedWindows() noexcept;
    std::wstring ExecutableDirectory();
    std::wstring ErrorText(DWORD code);
    void Log(std::wstring const& component, std::wstring const& message) noexcept;

    class UserSecurity
    {
    public:
        explicit UserSecurity(std::wstring const& sid);
        ~UserSecurity();
        UserSecurity(UserSecurity const&) = delete;
        UserSecurity& operator=(UserSecurity const&) = delete;
        SECURITY_ATTRIBUTES* attributes() noexcept { return &m_attributes; }
    private:
        PSECURITY_DESCRIPTOR m_descriptor{};
        SECURITY_ATTRIBUTES m_attributes{};
    };
}
