#pragma once

// A sky of one radiance in every direction, for evidence fixtures that need to
// know exactly what the environment hands a surface: a furnace (a known,
// constant irradiance) or a black sky (none at all).
//
// The black sky is what a fixture uses to take the ambient out of a frame.
// Without an environment the ambient ladder's last rung is the flat fill
// (AmbientLadder.glsl), which every surface takes, coats included since #1618,
// and which no shadow removes. A black sky selects the IBL rung at zero
// irradiance instead, for every surface alike.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <vector>

namespace OloEngine::Tests
{
    // A 32x32 RGBA32F cubemap of `radiance` everywhere, with its mip chain.
    [[nodiscard]] inline Ref<TextureCubemap> MakeUniformSkyCubemap(const glm::vec3& radiance)
    {
        constexpr u32 kResolution = 32;
        CubemapSpecification spec{};
        spec.Width = kResolution;
        spec.Height = kResolution;
        spec.Format = ImageFormat::RGBA32F;
        spec.GenerateMips = true;
        Ref<TextureCubemap> sky = TextureCubemap::Create(spec);
        if (!sky)
        {
            ADD_FAILURE() << "TextureCubemap::Create failed";
            return nullptr;
        }
        std::vector<f32> face(static_cast<sizet>(kResolution) * kResolution * 4u);
        for (sizet i = 0; i < face.size(); i += 4u)
        {
            face[i + 0u] = radiance.r;
            face[i + 1u] = radiance.g;
            face[i + 2u] = radiance.b;
            face[i + 3u] = 1.0f;
        }
        for (u32 f = 0; f < 6u; ++f)
        {
            if (!sky->SetFaceDataMip(f, 0, face.data(), static_cast<u32>(face.size() * sizeof(f32))))
            {
                ADD_FAILURE() << "SetFaceDataMip failed for face " << f;
                return nullptr;
            }
        }
        sky->GenerateMipmaps();
        return sky;
    }

    // Bakes `sky` into `entity`'s EnvironmentMapComponent the way the scene's
    // own loader does, and turns its IBL on. No skybox draw: the background
    // stays the clear colour.
    inline void SetUniformSkyEnvironment(Entity entity, const Ref<TextureCubemap>& sky, bool sphericalHarmonics = false)
    {
        IBLConfiguration config;
        config.UseSphericalHarmonics = sphericalHarmonics;
        // A generated cubemap has no file to key a cache entry on.
        config.UseDiskCache = false;
        auto& env = entity.HasComponent<EnvironmentMapComponent>() ? entity.GetComponent<EnvironmentMapComponent>()
                                                                    : entity.AddComponent<EnvironmentMapComponent>();
        env.m_EnableSkybox = false;
        env.m_EnableIBL = true;
        env.m_UseSphericalHarmonics = sphericalHarmonics;
        env.m_EnvironmentMap = EnvironmentMap::CreateFromCubemap(sky, config);
        ASSERT_TRUE(env.m_EnvironmentMap) << "EnvironmentMap::CreateFromCubemap failed";
        ASSERT_TRUE(env.m_EnvironmentMap->HasIBL()) << "the environment baked no IBL";
    }
} // namespace OloEngine::Tests
