#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"

#include <vector>

namespace OloEngine
{
    class Scene;

    // Captures and restores full scene state (components + settings) to/from binary.
    // Used by SaveGameManager for saving/loading game states.
    class SaveGameSerializer
    {
      public:
        // Capture all entity components + scene settings to binary blob.
        // outEntityCount receives the number of entities the blob holds, which
        // excludes runtime-owned entities (#1712), so a header or a slot list
        // never counts the dialogue UI or debris the save left out.
        static std::vector<u8> CaptureSceneState(Scene& scene, u32* outEntityCount = nullptr);

        // Clear scene and restore entities + settings from a blob written by
        // CaptureSceneState in this build (kSaveGameFormatVersion; the file header is
        // checked before this is called). Every settings struct and every component is a
        // length-prefixed block that must be consumed exactly, so a truncated or
        // otherwise malformed blob fails and leaves the scene untouched.
        static bool RestoreSceneState(Scene& scene, const std::vector<u8>& data);
    };

} // namespace OloEngine
