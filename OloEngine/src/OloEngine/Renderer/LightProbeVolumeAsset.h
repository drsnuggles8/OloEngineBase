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
            if (Resolution.x <= 0 || Resolution.y <= 0 || Resolution.z <= 0)
            {
                return 0;
            }
            return Resolution.x * Resolution.y * Resolution.z;
        }

        [[nodiscard]] bool HasBakedData() const noexcept
        {
            return !CoefficientData.empty();
        }

        void AllocateCoefficients()
        {
            i32 const totalProbes = GetTotalProbeCount();
            if (totalProbes <= 0)
            {
                return;
            }
            auto const totalCoeffs = static_cast<u64>(totalProbes) * SH_COEFFICIENT_COUNT;
            if (totalCoeffs > 10'000'000)
            {
                return;
            }
            CoefficientData.resize(static_cast<size_t>(totalCoeffs), glm::vec4(0.0f));
        }

        // Store baked SH data for a single probe at the given linear index
        void SetProbeData(i32 probeIndex, const SHCoefficients& sh, f32 validity = 1.0f)
        {
            size_t const maxProbes = CoefficientData.size() / SH_COEFFICIENT_COUNT;
            if (probeIndex < 0 || static_cast<size_t>(probeIndex) >= maxProbes)
            {
                return;
            }
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
            size_t const maxProbes = CoefficientData.size() / SH_COEFFICIENT_COUNT;
            if (probeIndex < 0 || static_cast<size_t>(probeIndex) >= maxProbes)
            {
                return {};
            }
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
