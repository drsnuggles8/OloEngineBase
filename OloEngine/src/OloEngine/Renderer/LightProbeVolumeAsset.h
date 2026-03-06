#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class LightProbeVolumeAsset : public Asset
    {
      public:
        LightProbeVolumeAsset() = default;
        ~LightProbeVolumeAsset() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::LightProbeVolume;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // Volume parameters
        glm::vec3 BoundsMin = glm::vec3(-10.0f);
        glm::vec3 BoundsMax = glm::vec3(10.0f);
        glm::ivec3 Resolution = glm::ivec3(4, 2, 4);
        f32 Spacing = 5.0f;

        // Baked SH coefficient data — 9 vec4s per probe, flat array
        // Layout: probe[0] coeffs 0..8, probe[1] coeffs 0..8, ...
        std::vector<glm::vec4> CoefficientData;

        [[nodiscard]] i32 GetTotalProbeCount() const noexcept
        {
            return Resolution.x * Resolution.y * Resolution.z;
        }

        [[nodiscard]] bool HasBakedData() const noexcept
        {
            return !CoefficientData.empty();
        }

        void AllocateCoefficients()
        {
            CoefficientData.resize(static_cast<size_t>(GetTotalProbeCount()) * SH_COEFFICIENT_COUNT, glm::vec4(0.0f));
        }

        // Store baked SH data for a single probe at the given linear index
        void SetProbeData(i32 probeIndex, const SHCoefficients& sh, f32 validity = 1.0f)
        {
            auto const offset = static_cast<size_t>(probeIndex) * SH_COEFFICIENT_COUNT;
            std::array<glm::vec4, SH_COEFFICIENT_COUNT> gpuData{};
            sh.ToGPULayout(gpuData, validity);
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                CoefficientData[offset + i] = gpuData[i];
            }
        }

        // Retrieve SH data for a single probe
        [[nodiscard]] SHCoefficients GetProbeData(i32 probeIndex) const
        {
            auto const offset = static_cast<size_t>(probeIndex) * SH_COEFFICIENT_COUNT;
            std::array<glm::vec4, SH_COEFFICIENT_COUNT> gpuData{};
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                gpuData[i] = CoefficientData[offset + i];
            }
            SHCoefficients sh;
            sh.FromGPULayout(gpuData);
            return sh;
        }
    };
} // namespace OloEngine
