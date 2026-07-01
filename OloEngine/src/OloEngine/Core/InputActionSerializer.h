#pragma once

#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"

#include <filesystem>
#include <optional>
#include <unordered_map>

namespace OloEngine
{
    class InputActionSerializer
    {
      public:
        // Persists one InputActionMap per context under an "InputActionContexts" sequence,
        // so authored per-context maps survive a save/reload. DeserializeContexts also reads
        // the legacy single-map format (an "InputActionMap" root node written by older
        // versions), mapping it to the Gameplay context, so pre-existing files still load.
        using ContextMaps = std::unordered_map<InputContextType, InputActionMap>;
        static bool SerializeContexts(const ContextMaps& contexts, const std::filesystem::path& filepath);
        static std::optional<ContextMaps> DeserializeContexts(const std::filesystem::path& filepath);
    };

} // namespace OloEngine
