#pragma once

#include "OloEngine/Core/InputAction.h"

#include <filesystem>
#include <optional>

namespace OloEngine
{
    class InputActionSerializer
    {
      public:
        static bool Serialize(const InputActionMap& map, const std::filesystem::path& filepath);
        static std::optional<InputActionMap> Deserialize(const std::filesystem::path& filepath);
    };

} // namespace OloEngine
