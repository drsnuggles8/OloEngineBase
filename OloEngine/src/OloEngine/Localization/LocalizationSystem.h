#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    // Pull-based update for entities marked with LocalizedTextComponent.
    //
    // On each call, this looks at LocalizationManager::GetGeneration() and,
    // if it's changed since the last call, walks every entity that owns both
    // LocalizedTextComponent and TextComponent and rewrites the latter's
    // TextString with the resolved translation for the current locale.
    //
    // Per-Scene state (the cached generation) lives on the Scene itself —
    // see Scene::m_LocalizationGeneration. This keeps multiple scenes (e.g.
    // active runtime + preview) independent.
    //
    // Cheap when nothing changed (single atomic load + integer compare), so
    // it's safe to invoke unconditionally from the per-frame tick.
    class LocalizationSystem
    {
      public:
        // Returns the number of entities whose text was refreshed this call
        // (0 when the generation hasn't moved since last call).
        static u32 UpdateLocalizedText(Scene& scene);
    };
} // namespace OloEngine
