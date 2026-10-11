#pragma once
#include <cstdint>

namespace glasswall::core
{
    enum class PauseReason : std::uint32_t
    {
        User = 1, DesktopInactive = 2, SessionLocked = 4, DisplayOff = 8,
        SystemSuspended = 16, Recovery = 32, OutputBlocked = 64, SessionDisconnected = 128
    };
    inline constexpr std::uint32_t AllPauseReasons = 255;
    struct PausePolicy
    {
        std::uint32_t reasons{};
        bool Has(PauseReason reason) const noexcept
        { return (reasons & static_cast<std::uint32_t>(reason)) != 0; }
        bool Set(PauseReason reason, bool active) noexcept
        {
            auto before = reasons;
            auto bit = static_cast<std::uint32_t>(reason);
            reasons = active ? reasons | bit : reasons & ~bit;
            return before != reasons;
        }
        bool CanPlay(bool requested) const noexcept { return requested && reasons == 0; }
    };
}
