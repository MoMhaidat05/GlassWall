#pragma once
#include <cstdint>

namespace glasswall::core
{
    enum class LoopPhase : std::uint32_t { Idle, Flushing, AwaitingDecode, AwaitingPresent };
    struct LoopProgress
    {
        LoopPhase phase{ LoopPhase::Idle };
        bool Begin() noexcept
        {
            if (phase != LoopPhase::Idle) return false;
            phase = LoopPhase::Flushing;
            return true;
        }
        void SeekFinished(bool success) noexcept
        {
            phase = success ? LoopPhase::AwaitingDecode : LoopPhase::Idle;
        }
        bool Decoded(std::int64_t timestamp, std::int64_t initialTimestamp, std::int64_t framePeriod) noexcept
        {
            if (phase == LoopPhase::Flushing) return false;
            if (phase != LoopPhase::AwaitingDecode) return true;
            // Seek success alone is insufficient; require a sample near the
            // original start, allowing normal key-frame/PTS offsets.
            if (framePeriod <= 0) return false;
            auto difference = timestamp >= initialTimestamp ?
                static_cast<std::uint64_t>(timestamp) - static_cast<std::uint64_t>(initialTimestamp) :
                static_cast<std::uint64_t>(initialTimestamp) - static_cast<std::uint64_t>(timestamp);
            if (difference > static_cast<std::uint64_t>(framePeriod) * 2) return false;
            phase = LoopPhase::AwaitingPresent;
            return true;
        }
        bool Presented() noexcept
        {
            if (phase != LoopPhase::AwaitingPresent) return false;
            phase = LoopPhase::Idle;
            return true;
        }
        void Reset() noexcept { phase = LoopPhase::Idle; }
    };
}
