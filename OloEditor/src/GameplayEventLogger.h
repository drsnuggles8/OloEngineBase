#pragma once

namespace OloEngine
{
    class Scene;

    // Editor diagnostic: subscribes a set of OLO_CORE_INFO loggers to the
    // scene's GameplayEventBus so every quest/inventory event streams into the
    // editor Console panel (and OloEngine.log) when you press Play. The
    // subscriptions are dropped automatically by Scene::OnRuntimeStop (which
    // Clear()s the bus), so this is safe to call once per Play session.
    //
    // This lives in OloEditor (not the engine library) on purpose: it is a
    // development aid, not behaviour shipped games should pay for.
    void AttachGameplayEventLogger(Scene& scene);

} // namespace OloEngine
