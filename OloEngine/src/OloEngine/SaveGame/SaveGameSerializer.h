#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    class Scene;

    // Captures and restores full scene state (components + settings) to/from binary.
    // Used by SaveGameManager for saving/loading game states.
    class SaveGameSerializer
    {
      public:
        // Capture all entity components + scene settings to binary blob
        static std::vector<u8> CaptureSceneState(Scene& scene);

        // Clear scene and restore entities + settings from binary blob
        static bool RestoreSceneState(Scene& scene, const std::vector<u8>& data);
    };

} // namespace OloEngine
