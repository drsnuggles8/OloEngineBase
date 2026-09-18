#pragma once

namespace OloEngine::Automation
{
    class AutomationRegistry;

    /// olo_groom_bind — the automation surface for the inspector's Build
    /// Binding action (issue #1249).
    ///
    /// It exists because the cook was otherwise unreachable without a human:
    /// binding a groom needs the body's live MeshSource, so it cannot be done
    /// offline, and the only trigger was a button in a scrolling inspector. That
    /// left the BOUND path unverifiable from a non-interactive session — which
    /// is exactly how the Vulkan cells of #1249's verification matrix went
    /// unrun on its first PR.
    void RegisterGroomCommands(AutomationRegistry& registry);
} // namespace OloEngine::Automation
