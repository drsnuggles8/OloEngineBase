#pragma once

namespace OloEngine::Automation
{
    class AutomationRegistry;

    // Structural scene authoring modules share the transport-independent registry.
    void RegisterEntityAuthoringCommands(AutomationRegistry& registry);
    void RegisterComponentAuthoringCommands(AutomationRegistry& registry);
    void RegisterSceneLifecycleCommands(AutomationRegistry& registry);
} // namespace OloEngine::Automation
