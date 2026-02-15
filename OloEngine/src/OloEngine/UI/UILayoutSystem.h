#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class Scene;

    class UILayoutSystem
    {
      public:
        // Resolve all UIRectTransformComponents under every UICanvasComponent
        // into UIResolvedRectComponents (pixel-space rects).
        static void ResolveLayout(Scene& scene, u32 viewportWidth, u32 viewportHeight);
    };
} // namespace OloEngine
