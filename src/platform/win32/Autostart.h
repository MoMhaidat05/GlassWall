#pragma once
#include "Support.h"

namespace glasswall::platform
{
    // Registry preference survives uninstall; only the Run registration is removed.
    bool StartWithWindows();
    void SetStartWithWindows(bool enabled);
    void ConfigureInstalledStartup();
    void RemoveStartupRegistration();
}
