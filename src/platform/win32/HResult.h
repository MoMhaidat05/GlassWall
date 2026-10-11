#pragma once
#include "Support.h"
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <source_location>

namespace glasswall::platform
{
    inline std::wstring HexResult(HRESULT result)
    {
        std::wostringstream text;
        text << L"0x" << std::hex << std::uppercase << std::setfill(L'0')
             << std::setw(8) << static_cast<std::uint32_t>(result);
        return text.str();
    }
    class Failure : public std::exception
    {
    public:
        Failure(HRESULT value, wchar_t const* where,
            std::source_location location = std::source_location::current()) : code(value), operation(where)
        {
            auto widen = [](char const* text) { return std::wstring(text, text + std::char_traits<char>::length(text)); };
            Log(L"engine", L"Failure function=" + widen(location.function_name()) +
                L" operation=" + operation + L" HRESULT=" + HexResult(code) +
                L" source=" + widen(location.file_name()) + L":" + std::to_wstring(location.line()));
        }
        char const* what() const noexcept override { return "Windows graphics/media operation failed"; }
        HRESULT code;
        std::wstring operation;
    };
    inline void Check(HRESULT result, wchar_t const* operation,
        std::source_location location = std::source_location::current())
    {
        if (FAILED(result)) throw Failure(result, operation, location);
    }
    inline LONGLONG Clock100ns()
    {
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        return (now.QuadPart / frequency.QuadPart) * 10000000 +
            ((now.QuadPart % frequency.QuadPart) * 10000000) / frequency.QuadPart;
    }
}
