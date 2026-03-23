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
        // cameraVP is the camera's view-projection matrix, used for
        // UIWorldAnchorComponent world-to-screen projection.
        static void ResolveLayout(Scene& scene, u32 viewportWidth, u32 viewportHeight,
                                  const glm::mat4& cameraVP = glm::mat4(1.0f));
    };
} // namespace OloEngine
