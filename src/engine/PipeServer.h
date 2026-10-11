#pragma once
#include "../platform/win32/Support.h"

namespace glasswall::engine
{
    class WallpaperRuntime;
    inline constexpr UINT ShutdownMessage = WM_APP + 1;
    inline constexpr UINT FailureMessage = WM_APP + 2;
    void Serve(platform::Identity const& identity, HANDLE stop, HWND control, WallpaperRuntime& runtime) noexcept;
}
