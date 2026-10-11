#pragma once
#include "library/Library.h"
#include <winrt/Windows.Storage.Streams.h>
#include <filesystem>

namespace glasswall::manager
{
    // Call only on the manager's single thumbnail worker, initialized for MTA.
    struct ThumbnailResult
    {
        winrt::Windows::Storage::Streams::IRandomAccessStream stream{ nullptr };
        std::wstring diagnostic;
    };
    // Shell first, then a separate, short-lived MF still reader. No engine objects.
    // Optional cache/mode arguments let the headless checker avoid the user's cache.
    ThumbnailResult OpenThumbnail(library::Entry const& entry,
        std::filesystem::path const& cacheDirectory = {}, bool shellFirst = true);
    // Deletes only manager-owned cache files for this GUID, never the source path.
    void InvalidateThumbnail(std::wstring const& id, std::filesystem::path const& cacheDirectory = {});
}
