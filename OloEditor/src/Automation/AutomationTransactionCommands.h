#pragma once

namespace OloEngine::Automation
{
    class AutomationRegistry;

    // Register the transaction command (issue #1127) onto `registry`.
    //
    // Exposed as a command like everything else, so it reaches MCP, oloctl and
    // any other frontend through the one registry rather than as a parallel
    // mechanism only one of them knows about. The handler closes over `registry`
    // because a transaction dispatches its steps back through it.
    void RegisterTransactionCommands(AutomationRegistry& registry);
} // namespace OloEngine::Automation
