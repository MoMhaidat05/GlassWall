#pragma once
#include <cstdint>
#include <type_traits>

namespace glasswall::ipc
{
    // Fixed-size little-endian messages; no pointers or variable-length input.
    inline constexpr std::uint32_t Magic = 0x31535747; // "GWS1"
    inline constexpr std::uint32_t Version = 1;
    inline constexpr std::uint32_t Acknowledgement = 0x314B4341;
    inline constexpr std::uint32_t TimeoutMs = 2000;

    enum class Command : std::uint32_t
    {
        Status = 1, Shutdown = 2, LoadWallpaper = 3, Play = 4,
        Pause = 5, Resume = 6, StopWallpaper = 7, GetPlaybackState = 8,
        ImportWallpaper = 9, ApplyWallpaper = 10, RestoreWallpaper = 11,
        RenameWallpaper = 12, RemoveWallpaper = 13, GetLibrary = 14, RelinkWallpaper = 15
    };
    enum class EngineState : std::uint32_t { Ready = 1, Stopping = 2 };
    enum class Result : std::uint32_t { Success = 0, InvalidRequest = 1 };

    struct Request
    {
        std::uint32_t magic{ Magic };
        std::uint32_t version{ Version };
        Command command{ Command::Status };
        std::uint32_t reserved{};
    };

    struct Response
    {
        std::uint32_t magic{ Magic };
        std::uint32_t version{ Version };
        std::uint32_t processId{};
        EngineState state{ EngineState::Ready };
        Result result{ Result::Success };
        std::uint32_t reserved{};
    };

    constexpr bool IsValid(Request const& request) noexcept
    {
        return request.magic == Magic && request.version == Version &&
            request.reserved == 0 &&
            (request.command == Command::Status || request.command == Command::Shutdown);
    }

    constexpr bool IsValid(Response const& response) noexcept
    {
        return response.magic == Magic && response.version == Version &&
            response.reserved == 0 && response.processId != 0 &&
            (response.state == EngineState::Ready || response.state == EngineState::Stopping) &&
            (response.result == Result::Success || response.result == Result::InvalidRequest);
    }

    static_assert(sizeof(Request) == 16 && sizeof(Response) == 24);
    static_assert(std::is_trivially_copyable_v<Request> && std::is_trivially_copyable_v<Response>);
}
