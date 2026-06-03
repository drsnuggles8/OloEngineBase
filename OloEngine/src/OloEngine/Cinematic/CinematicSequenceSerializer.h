#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <string>

namespace OloEngine
{
    class CinematicSequence;

    /**
     * @brief YAML (de)serialization for CinematicSequence (`.olocine`).
     *
     * Mirrors AnimationGraphSerializer: pure string/file round-trip helpers,
     * used both by the editor asset pipeline (CinematicSequenceAssetSerializer)
     * and directly by tests. All floats read back are validated with
     * std::isfinite; malformed keys are skipped rather than poisoning the
     * sequence with NaNs.
     */
    class CinematicSequenceSerializer
    {
      public:
        [[nodiscard("serialized YAML must be used")]] static std::string SerializeToString(const Ref<CinematicSequence>& sequence);
        [[nodiscard("deserialized sequence must be assigned")]] static Ref<CinematicSequence> DeserializeFromString(const std::string& yamlString);

        [[nodiscard("serialization success must be checked")]] static bool Serialize(const Ref<CinematicSequence>& sequence, const std::string& filepath);
        [[nodiscard("deserialized sequence must be assigned")]] static Ref<CinematicSequence> DeserializeAsset(const std::string& filepath);
    };
} // namespace OloEngine
