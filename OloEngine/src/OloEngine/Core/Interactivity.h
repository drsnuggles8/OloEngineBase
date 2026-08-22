#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // @brief Is there a human at the keyboard?
    //
    // This exists because the engine kept re-learning the same lesson one modal
    // at a time. Three separate escape hatches were added, each after a hang:
    //
    //   * `OLO_EDITOR_AUTOSAVE_RECOVERY` — pre-answers the auto-save recovery
    //     modal, added when a headless attach hung on it (issue #316).
    //   * `OLO_EDITOR_UNSAVED_PROMPT` — pre-answers the unsaved-changes modal,
    //     added when WM_CLOSE with a dirty scene hung on it.
    //   * The assert dialog — `OLO_CORE_ASSERT` called `MessageBoxA`
    //     unconditionally, so any assert parked a test run in
    //     `NtUserWaitMessage` forever at ~0% CPU (issue #714).
    //
    // Three variables, three incidents, one actual problem: **a blocking modal
    // in a process nobody is watching is a hang, not a prompt.** Each new modal
    // added anywhere in the engine is the next instance, and the pattern says
    // it will be found the same way — by someone losing an afternoon to a
    // process that looks slow.
    //
    // So the question is asked once, of the process, rather than once per
    // dialog. A host that knows it is automated (the test binary, a launcher
    // starting the editor detached, CI) calls `SetNonInteractive(true)` at
    // startup; every blocking prompt then takes its configured or documented-safe
    // answer and logs what it did.
    //
    // **What this is NOT:** a way to suppress errors. An assert still logs, a
    // failed save still reports. Only the *blocking* is removed.
    //
    // Adding a modal? Ask `IsNonInteractive()` first, and give the automated
    // path the least destructive answer — never the convenient one. "Cancel"
    // and "keep what is on disk" are correct defaults; "discard" is not.
    void SetNonInteractive(bool nonInteractive);
    [[nodiscard]] bool IsNonInteractive();
} // namespace OloEngine
