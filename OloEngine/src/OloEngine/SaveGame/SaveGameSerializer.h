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
        // Capture all entity components + scene settings to binary blob
        static std::vector<u8> CaptureSceneState(Scene& scene);

        // Clear scene and restore entities + settings from binary blob.
        // formatVersion is the FormatVersion recorded in the save's header (defaults to
        // the current version, i.e. "no gating" -- every field is assumed present, which
        // matches data produced by CaptureSceneState in this build). A caller loading an
        // on-disk .olosave should pass the header's actual FormatVersion so per-component
        // Serialize() overloads can skip fields that didn't exist yet at that version.
        static bool RestoreSceneState(Scene& scene, const std::vector<u8>& data,
                                      u32 formatVersion = kSaveGameFormatVersion);
    };

} // namespace OloEngine
