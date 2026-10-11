#pragma once
#include "../core/LoopProgress.h"
#include <mfidl.h>
#include <mfreadwrite.h>
#include <ostream>

namespace glasswall::diagnostics
{
    inline bool RunLoopTests(std::wostream& output)
    {
        using core::LoopPhase;
        core::LoopProgress progress;
        constexpr std::int64_t first = 666666, period = 333333;
        // EOF is a bit flag and can accompany the last valid sample.
        auto eof = static_cast<DWORD>(MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_STREAMTICK);
        if (!(eof & MF_SOURCE_READERF_ENDOFSTREAM) || progress.Presented() || !progress.Begin() || progress.Begin() ||
            progress.Decoded(first, first, period) || progress.Presented()) return false;
        progress.SeekFinished(true);
        if (progress.phase != LoopPhase::AwaitingDecode || progress.Presented() ||
            progress.Decoded(29000000, first, period) || progress.Decoded(first, first, 0)) return false;
        if (!progress.Decoded(first, first, period) || progress.phase != LoopPhase::AwaitingPresent ||
            !progress.Presented() || progress.Presented() || progress.phase != LoopPhase::Idle) return false;
        // A second genuine completion needs a new seek, decode and presentation.
        if (!progress.Begin()) return false;
        progress.SeekFinished(true);
        if (progress.Presented() || !progress.Decoded(first + period, first, period) || !progress.Presented()) return false;
        if (!progress.Begin()) return false;
        progress.SeekFinished(false);
        if (progress.phase != LoopPhase::Idle || progress.Presented()) return false;
        if (!progress.Begin()) return false;
        progress.SeekFinished(true);
        if (!progress.Decoded(-333333, -333333, period) || !progress.Presented()) return false;
        if (!progress.Begin()) return false;
        progress.SeekFinished(true);
        if (progress.Decoded(INT64_MAX, INT64_MIN, period)) return false;
        progress.Reset();
        output << L"Loop progress regression checks passed: EOF bits, flush gate, seek versus decode/present proof, rewind timestamps and failures. No Windows playback was tested.\n";
        return true;
    }
}
