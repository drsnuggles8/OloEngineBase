#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <string>

namespace OloEngine
{
    class FluidSettings;

    /**
     * @brief YAML (de)serialization for FluidSettings (`.olofluid`).
     *
     * Mirrors CinematicSequenceSerializer: pure string/file round-trip
     * helpers used by the asset pipeline (FluidSettingsAssetSerializer, incl.
     * asset packs) and directly by tests. Every float read back is validated
     * with std::isfinite and clamped to the range documented on the
     * FluidSettings field; a missing key keeps the constructor default.
     */
    class FluidSettingsSerializer
    {
      public:
        [[nodiscard("serialized YAML must be used")]] static std::string SerializeToString(const Ref<FluidSettings>& settings);
        [[nodiscard("deserialized settings must be assigned")]] static Ref<FluidSettings> DeserializeFromString(const std::string& yamlString);

        [[nodiscard("serialization success must be checked")]] static bool Serialize(const Ref<FluidSettings>& settings, const std::string& filepath);
        [[nodiscard("deserialized settings must be assigned")]] static Ref<FluidSettings> DeserializeAsset(const std::string& filepath);
    };
} // namespace OloEngine
