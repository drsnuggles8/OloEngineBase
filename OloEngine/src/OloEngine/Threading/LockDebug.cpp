#include "OloEnginePCH.h"
#include "OloEngine/Threading/LockDebug.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine::LockDebug
{
    void ReportSelfDeadlock(const void* mutex, EMode wanted)
    {
        const EMode held = HeldMode(mutex);
        const auto modeName = [](EMode mode)
        { return mode == EMode::Exclusive ? "exclusive" : "shared"; };

        // Logged, not asserted, so the diagnosis survives a build with no debugger
        // attached — which is every CI run and every Release editor. This is the message
        // that issues #439 and #863 each cost a live cdb session to work out.
        OLO_CORE_ERROR(
            "Self-deadlock on a non-recursive lock at {}: this thread already holds it "
            "({}), and is now asking for it {}. The acquisition below this line will park "
            "the thread FOREVER — no further log lines, no CPU, no crash record. Something "
            "further up this call chain took the lock; a helper that locks internally is "
            "the usual culprit (issues #439 and #863 were both SerializeAssetRegistry "
            "reached from inside a locked scope). Fix the callee if you can: a public "
            "method that locks a non-recursive mutex constrains every caller forever. "
            "See docs/agent-rules/non-recursive-lock-self-locking-helper.md.",
            mutex, modeName(held), modeName(wanted));

        // Break where the debugger can show the offending stack. Compiles to nothing
        // outside a Debug engine build, in which case the log line above is the whole
        // report and execution continues into the park — the pre-existing behaviour,
        // now explained.
        OLO_DEBUGBREAK();
    }
} // namespace OloEngine::LockDebug
