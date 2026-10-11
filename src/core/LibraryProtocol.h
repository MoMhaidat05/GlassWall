#pragma once
#include "PlaybackProtocol.h"
namespace glasswall::ipc
{
    inline constexpr std::uint32_t LibraryMagic = 0x344C5747; // GWL4
    inline constexpr std::uint32_t LibraryVersion = 1;
    inline constexpr std::size_t MaxLibraryMessage = 8 * 1024 * 1024;
    // GetLibrary uses the existing request frame and a bounded UTF-16 JSON reply.
    struct LibraryResponse
    {
        std::uint32_t magic{ LibraryMagic }, version{ LibraryVersion }, processId{}, jsonBytes{};
    };
    constexpr bool IsValid(LibraryResponse const& value, std::size_t bytes) noexcept
    {
        return value.magic == LibraryMagic && value.version == LibraryVersion && value.processId != 0 &&
            value.jsonBytes >= 4 && value.jsonBytes % 2 == 0 &&
            bytes == sizeof(value) + value.jsonBytes && bytes <= MaxLibraryMessage;
    }
    static_assert(sizeof(LibraryResponse) == 16);
}
