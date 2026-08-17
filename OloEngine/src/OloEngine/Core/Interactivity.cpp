#include "OloEnginePCH.h"
#include "OloEngine/Core/Interactivity.h"

#include "OloEngine/Core/Environment.h"
#include "OloEngine/Core/Log.h"

#include <atomic>

namespace OloEngine
{
    namespace
    {
        // Default: assume a human. A wrong "interactive" guess costs one hung
        // automated run; a wrong "non-interactive" guess silently auto-answers a
        // real user's prompts, which is far worse. Fail toward the prompt.
        //
        // OLO_NON_INTERACTIVE lets a launcher set this without a command-line
        // flag, for the case the engine is started by something that cannot pass
        // argv through (a detached Start-Process, a service host).
        std::atomic s_NonInteractive{ Env::IsTruthy("OLO_NON_INTERACTIVE") };
    } // namespace

    void SetNonInteractive(bool nonInteractive)
    {
        if (const bool previous = s_NonInteractive.exchange(nonInteractive); previous != nonInteractive)
        {
            OLO_CORE_INFO("Interactivity: non-interactive mode {} — blocking modals will {}",
                          nonInteractive ? "ON" : "OFF",
                          nonInteractive ? "take their safe default" : "prompt");
        }
        // An assert dialog is a blocking modal like any other, so it follows the
        // same switch rather than needing its own.
        SetAssertDialogsEnabled(!nonInteractive);
    }

    bool IsNonInteractive()
    {
        return s_NonInteractive.load();
    }
} // namespace OloEngine
