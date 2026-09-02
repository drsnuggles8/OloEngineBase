#pragma once

#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

// The raster light consumer over the canonical light record (issue #993).
//
// Scene::ProcessScene3DSharedLogic used to hand-pack MultiLightData and the
// three Forward+ structs from the components. It now encodes one
// GPUSceneLight per light (EncodeGPUSceneLight with a zero render origin, so
// the positions are still world-space at this point; the two raster upload
// sites shift them themselves) and derives every raster struct from that
// record through the functions below. The record is therefore the only place
// the authored light is read, and the raster structs cannot disagree with it.
//
// Every function returns the SAME bytes the hand-packing produced, including
// the "no shadow entry" sentinel (-1): the shadow atlas / VSM layer base is a
// per-frame render allocation that Scene patches into the raster structs after
// allocation, exactly as before. GPUSceneLightAdapterTest pins the
// field-for-field equality per light type.
//
// The raster migration child (#994) removes this adapter by reading
// GPUSceneLight from the SSBO directly.
namespace OloEngine::GPUSceneLightAdapter
{
    // MultiLightData::Direction.w / GPUPointLight::ShadowAndAttenuation.x /
    // GPUSpotLight::SpotParams.z / GPUSphereAreaLight::RangeAndPadding.y before
    // the shadow allocation patches a winner.
    inline constexpr f32 kNoShadowEntry = -1.0f;

    [[nodiscard]] inline GPUSceneLightType TypeOf(const GPUSceneLight& light)
    {
        return static_cast<GPUSceneLightType>(light.Type);
    }

    [[nodiscard]] inline GPUPointLight ToForwardPlusPoint(const GPUSceneLight& light)
    {
        return GPUPointLight{
            .PositionAndRadius = light.PositionAndRange,
            .ColorAndIntensity = light.ColorAndIntensity,
            .ShadowAndAttenuation = glm::vec4(kNoShadowEntry, light.ShapeParams.z, 0.0f, 0.0f),
        };
    }

    [[nodiscard]] inline GPUSpotLight ToForwardPlusSpot(const GPUSceneLight& light)
    {
        // The Forward+ struct carries the normalised direction; the record
        // carries the sanitised authored one (LightCommon.h), as the UBO does.
        return GPUSpotLight{
            .PositionAndRadius = light.PositionAndRange,
            .DirectionAndAngle = glm::vec4(glm::normalize(glm::vec3(light.DirectionAndRadius)), light.ShapeParams.y),
            .ColorAndIntensity = light.ColorAndIntensity,
            .SpotParams = glm::vec4(light.ShapeParams.x, light.ShapeParams.z, kNoShadowEntry, 0.0f),
        };
    }

    [[nodiscard]] inline GPUSphereAreaLight ToForwardPlusSphereArea(const GPUSceneLight& light)
    {
        return GPUSphereAreaLight{
            .PositionAndRadius = glm::vec4(glm::vec3(light.PositionAndRange), light.DirectionAndRadius.w),
            .ColorAndIntensity = light.ColorAndIntensity,
            .RangeAndPadding = glm::vec4(light.PositionAndRange.w, kNoShadowEntry, 0.0f, 0.0f),
        };
    }

    // The MultiLightUBO entry. Type tags ride Position.w and SpotParams.w
    // exactly as PBRCommon.glsl decodes them.
    [[nodiscard]] inline UBOStructures::MultiLightData ToMultiLightData(const GPUSceneLight& light)
    {
        UBOStructures::MultiLightData data{};
        const glm::vec3 position(light.PositionAndRange);
        const glm::vec3 direction(light.DirectionAndRadius);
        const f32 range = light.PositionAndRange.w;
        data.Color = light.ColorAndIntensity;
        switch (TypeOf(light))
        {
            case GPUSceneLightType::Directional:
                data.Position = glm::vec4(direction, 0.0f);
                data.Direction = glm::vec4(direction, kNoShadowEntry);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                data.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                break;
            case GPUSceneLightType::Point:
                data.Position = glm::vec4(position, 1.0f);
                data.Direction = glm::vec4(0.0f, -1.0f, 0.0f, kNoShadowEntry);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, light.ShapeParams.z, range);
                data.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                break;
            case GPUSceneLightType::Spot:
                data.Position = glm::vec4(position, 2.0f);
                data.Direction = glm::vec4(direction, kNoShadowEntry);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, light.ShapeParams.z, range);
                data.SpotParams = glm::vec4(light.ShapeParams.x, light.ShapeParams.y, light.ShapeParams.w, 2.0f);
                break;
            case GPUSceneLightType::SphereArea:
                data.Position = glm::vec4(position, 3.0f);
                data.Direction = glm::vec4(0.0f, -1.0f, 0.0f, kNoShadowEntry);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, range);
                data.SpotParams = glm::vec4(0.0f, 0.0f, light.DirectionAndRadius.w, 3.0f);
                break;
            default:
                // A type tag outside the enum matches no branch of
                // calculateLightContribution, so the entry contributes nothing
                // instead of reading as a directional light with a zero direction.
                data.Position = glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
                data.Direction = glm::vec4(0.0f, -1.0f, 0.0f, kNoShadowEntry);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                data.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
                break;
        }
        return data;
    }
} // namespace OloEngine::GPUSceneLightAdapter
