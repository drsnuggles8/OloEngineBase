#pragma once

namespace OloEngine::Automation
{
    class AutomationRegistry;

    // Structured build invocation (issue #1163, Epic H slice 3): olo_build_list
    // and olo_build_run.
    //
    // These spawn `.claude/skills/run-oloengine/build-lock.ps1` as a CHILD
    // PROCESS from the automation handler thread. They never marshal onto the
    // game thread and never touch the renderer — a build has nothing to do with
    // a frame, and going through the game thread would stall the editor for the
    // length of a build.
    //
    // The concurrency contract with the lock is the substance of the slice, not
    // an implementation detail; it is stated in
    // docs/agent-rules/automation-build-invocation.md and enforced here.
    void RegisterBuildCommands(AutomationRegistry& registry);
} // namespace OloEngine::Automation
