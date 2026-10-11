#pragma once
#include "../core/PausePolicy.h"
#include <ostream>
#include <initializer_list>

namespace glasswall::diagnostics
{
    inline bool RunPauseTests(std::wostream& output)
    {
        using core::PauseReason;
        core::PausePolicy policy;
        if (policy.CanPlay(false) || !policy.CanPlay(true)) return false;
        for (auto reason : { PauseReason::User, PauseReason::DesktopInactive, PauseReason::SessionLocked,
            PauseReason::DisplayOff, PauseReason::SystemSuspended, PauseReason::Recovery, PauseReason::OutputBlocked,
            PauseReason::SessionDisconnected })
        {
            if (!policy.Set(reason, true) || policy.Set(reason, true) || policy.CanPlay(true)) return false;
            if (!policy.Set(reason, false) || policy.Set(reason, false) || !policy.CanPlay(true)) return false;
        }
        policy.Set(PauseReason::User, true);
        policy.Set(PauseReason::DesktopInactive, true);
        policy.Set(PauseReason::SessionLocked, true);
        policy.Set(PauseReason::DisplayOff, true);
        policy.Set(PauseReason::SystemSuspended, true);
        policy.Set(PauseReason::DesktopInactive, false);
        policy.Set(PauseReason::SessionLocked, false);
        policy.Set(PauseReason::DisplayOff, false);
        policy.Set(PauseReason::SystemSuspended, false);
        if (policy.CanPlay(true) || !policy.Has(PauseReason::User)) return false;
        policy.Set(PauseReason::User, false);
        if (!policy.CanPlay(true)) return false;
        policy.Set(PauseReason::SessionLocked, true);
        policy.Set(PauseReason::SessionDisconnected, true);
        policy.Set(PauseReason::SessionDisconnected, false);
        if (policy.CanPlay(true) || !policy.Has(PauseReason::SessionLocked)) return false;
        policy.Set(PauseReason::SessionLocked, false);
        if (!policy.CanPlay(true)) return false;
        for (unsigned i = 0; i < 1000; ++i)
        {
            policy.Set(PauseReason::DesktopInactive, true);
            if (policy.CanPlay(true)) return false;
            policy.Set(PauseReason::DesktopInactive, false);
            if (!policy.CanPlay(true)) return false;
        }
        output << L"Independent pause reason checks passed; Windows event delivery and suspension are not tested here.\n";
        return true;
    }
}
