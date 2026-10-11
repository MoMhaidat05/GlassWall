#pragma once
#include "Protocol.h"
#include "LoopProgress.h"
#include "PausePolicy.h"
#include <cstddef>

namespace glasswall::ipc
{
    inline constexpr std::uint32_t PlaybackMagic = 0x32535747; // "GWS2"
    inline constexpr std::uint32_t PlaybackVersion = 5; // library commands; Stage 1 layout is unchanged
    inline constexpr std::size_t MaxPathCharacters = 4096; // including UTF-16 terminator
    inline constexpr std::size_t MaxPlaybackMessage = 16 + MaxPathCharacters * 2;
    enum class PlaybackState : std::uint32_t
    { Empty, Loading, Ready, Playing, Paused, Recovering, HostUnavailable, Error };
    enum class HardwarePath : std::uint32_t
    { Uninitialized, DxvaConfigured, GpuSamplesRequired, GpuSampleObserved };
    enum class PlaybackResult : std::uint32_t { Accepted, InvalidRequest, Busy };

    struct PlaybackRequest
    {
        std::uint32_t magic{ PlaybackMagic };
        std::uint32_t version{ PlaybackVersion };
        Command command{ Command::GetPlaybackState };
        std::uint32_t pathBytes{};
    };
    struct PlaybackResponse
    {
        std::uint32_t magic{ PlaybackMagic };
        std::uint32_t version{ PlaybackVersion };
        std::uint32_t processId{};
        PlaybackResult result{ PlaybackResult::Accepted };
        std::int32_t error{}; // immediate transport/request HRESULT, not queued completion
        PlaybackState state{ PlaybackState::Empty };
        HardwarePath hardware{ HardwarePath::Uninitialized };
        std::uint32_t flags{}; // bit 0: pending load; bit 1: first frame pending
        std::uint64_t requestId{};
        std::uint64_t completedRequestId{};
        std::uint64_t decodedFrames{};
        std::uint64_t presentedFrames{};
        std::uint64_t loops{};
        std::uint64_t hostRecoveries{};
        std::uint64_t deviceRecoveries{};
        std::uint64_t hostsCreated{};
        std::uint64_t devicesCreated{};
        std::uint64_t readersCreated{};
        std::int64_t position100ns{};
        std::int32_t operationError{};
        std::int32_t mediaError{};
        std::int32_t deviceRemovedReason{};
        std::uint32_t reserved{};
        std::uint64_t eofEvents{}, seekAttempts{}, loopCompletions{};
        std::int64_t lastEofTimestamp100ns{}, loopDecodedTimestamp100ns{}, loopPresentedTimestamp100ns{}, duration100ns{};
        std::int32_t lastSeekError{};
        core::LoopPhase loopPhase{ core::LoopPhase::Idle };
        std::uint32_t pauseReasons{}, playRequested{}, pendingReads{}, suspensionReserved{};
        std::uint64_t pauseTransitions{}, resumeTransitions{};
    };
    constexpr bool IsPlaybackCommand(Command command) noexcept
    {
        return command >= Command::LoadWallpaper && command <= Command::RelinkWallpaper;
    }
    constexpr bool HasPayload(Command command) noexcept
    {
        return command == Command::LoadWallpaper || command == Command::ImportWallpaper ||
            command == Command::ApplyWallpaper || command == Command::RenameWallpaper || command == Command::RemoveWallpaper ||
            command == Command::RelinkWallpaper;
    }
    constexpr bool IsValid(PlaybackRequest const& request, std::size_t messageBytes) noexcept
    {
        return request.magic == PlaybackMagic && request.version == PlaybackVersion &&
            IsPlaybackCommand(request.command) && request.pathBytes % 2 == 0 &&
            request.pathBytes <= MaxPathCharacters * 2 &&
            messageBytes == sizeof(PlaybackRequest) + request.pathBytes &&
            (HasPayload(request.command) ? request.pathBytes >= 4 : request.pathBytes == 0);
    }
    constexpr bool IsValid(PlaybackResponse const& response) noexcept
    {
        return response.magic == PlaybackMagic && response.version == PlaybackVersion &&
            response.processId != 0 && response.reserved == 0 && response.flags <= 3 &&
            response.state <= PlaybackState::Error && response.hardware <= HardwarePath::GpuSampleObserved &&
            response.result <= PlaybackResult::Busy && response.loopPhase <= core::LoopPhase::AwaitingPresent &&
            response.pauseReasons <= core::AllPauseReasons && response.playRequested <= 1 &&
            response.pendingReads <= 3 && response.suspensionReserved == 0;
    }
    static_assert(sizeof(PlaybackRequest) == 16 && sizeof(PlaybackResponse) == 232);
    static_assert(std::is_trivially_copyable_v<PlaybackResponse>);
}
