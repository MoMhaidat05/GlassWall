#pragma once
#include <string>
namespace glasswall::media
{
    // Shared path validation used by playback and the persistent reference store.
    std::wstring ValidateLocalPath(std::wstring const& path);
}
