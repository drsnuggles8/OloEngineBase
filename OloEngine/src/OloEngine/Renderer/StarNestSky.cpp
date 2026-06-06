#include "OloEnginePCH.h"
#include "OloEngine/Renderer/StarNestSky.h"

#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/SkyCubemapBake.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstring>

namespace OloEngine
{
    StarNestSkyUBO StarNestSky::ComputeUBO(const StarNestParameters& params)
    {
        OLO_PROFILE_FUNCTION();

        // Sanitise the camera offset — a NaN/inf here would propagate through
        // the entire marched field and poison the baked cubemap (one bad texel
        // ruins the IBL irradiance convolution).
        glm::vec3 offset = params.Offset;
        for (int i = 0; i < 3; ++i)
        {
            if (!std::isfinite(offset[i]))
                offset = glm::vec3(1.0f, 0.5f, 0.5f);
        }

        const auto safeFinite = [](f32 v, f32 fallback) noexcept
        {
            return std::isfinite(v) ? v : fallback;
        };

        const f32 rotation1 = safeFinite(params.Rotation1, 0.0f);
        const f32 rotation2 = safeFinite(params.Rotation2, 0.0f);

        // Clamp the tuning constants into sane ranges. These mirror the shader's
        // expectations: stepsize/tile must be strictly positive (division /
        // modulo by them), fades live in [0,1], saturation in [0,1].
        const f32 formuparam = glm::clamp(safeFinite(params.Formuparam, 0.53f), 0.0f, 2.0f);
        const f32 stepsize = glm::clamp(safeFinite(params.StepSize, 0.1f), 1e-3f, 1.0f);
        const f32 tile = glm::clamp(safeFinite(params.Tile, 0.85f), 1e-2f, 4.0f);
        const f32 brightness = glm::clamp(safeFinite(params.Brightness, 0.0015f), 0.0f, 1.0f);
        const f32 darkmatter = glm::clamp(safeFinite(params.DarkMatter, 0.3f), 0.0f, 5.0f);
        const f32 distfading = glm::clamp(safeFinite(params.DistFading, 0.73f), 0.0f, 1.0f);
        const f32 saturation = glm::clamp(safeFinite(params.Saturation, 0.85f), 0.0f, 1.0f);
        const f32 intensity = glm::clamp(safeFinite(params.Intensity, 1.0f), 0.0f, 100.0f);

        const i32 iterations = glm::clamp(params.Iterations, 1, kStarNestMaxIterations);
        const i32 volsteps = glm::clamp(params.VolSteps, 1, kStarNestMaxVolSteps);

        StarNestSkyUBO ubo{};
        ubo.Offset = glm::vec4(offset, rotation1);
        ubo.Params0 = glm::vec4(formuparam, stepsize, tile, rotation2);
        ubo.Params1 = glm::vec4(brightness, darkmatter, distfading, saturation);
        ubo.Params2 = glm::vec4(intensity, static_cast<f32>(iterations), static_cast<f32>(volsteps), 0.0f);
        return ubo;
    }

    glm::vec3 StarNestSky::EvaluateAtDirection(const StarNestSkyUBO& ubo, const glm::vec3& viewDir)
    {
        // Direct CPU mirror of StarNestSky.glsl::main. Kept structurally
        // identical so a typo in either side is observable from a CPU test.
        const f32 a1 = ubo.Offset.w;
        const f32 a2 = ubo.Params0.w;
        const glm::mat2 rot1(std::cos(a1), std::sin(a1), -std::sin(a1), std::cos(a1));
        const glm::mat2 rot2(std::cos(a2), std::sin(a2), -std::sin(a2), std::cos(a2));

        glm::vec3 dir = glm::normalize(viewDir);
        glm::vec2 dirXZ = glm::vec2(dir.x, dir.z) * rot1;
        dir.x = dirXZ.x;
        dir.z = dirXZ.y;
        glm::vec2 dirXY = glm::vec2(dir.x, dir.y) * rot2;
        dir.x = dirXY.x;
        dir.y = dirXY.y;

        glm::vec3 from(ubo.Offset);
        glm::vec2 fromXZ = glm::vec2(from.x, from.z) * rot1;
        from.x = fromXZ.x;
        from.z = fromXZ.y;
        glm::vec2 fromXY = glm::vec2(from.x, from.y) * rot2;
        from.x = fromXY.x;
        from.y = fromXY.y;

        const f32 formuparam = ubo.Params0.x;
        const f32 stepsize = ubo.Params0.y;
        const f32 tile = ubo.Params0.z;
        const f32 brightness = ubo.Params1.x;
        const f32 darkmatter = ubo.Params1.y;
        const f32 distfading = ubo.Params1.z;
        const f32 saturation = ubo.Params1.w;
        const f32 intensity = ubo.Params2.x;
        const i32 iterations = static_cast<i32>(ubo.Params2.y);
        const i32 volsteps = static_cast<i32>(ubo.Params2.z);

        f32 s = 0.1f;
        f32 fade = 1.0f;
        glm::vec3 v(0.0f);
        for (i32 r = 0; r < volsteps; ++r)
        {
            glm::vec3 p = from + s * dir * 0.5f;
            const glm::vec3 period(tile * 2.0f);
            // GLSL mod: x - y*floor(x/y).
            glm::vec3 m = p - period * glm::floor(p / period);
            p = glm::abs(glm::vec3(tile) - m);

            f32 pa = 0.0f;
            f32 a = 0.0f;
            for (i32 i = 0; i < iterations; ++i)
            {
                const f32 d = std::max(glm::dot(p, p), 1e-6f);
                p = glm::abs(p) / d - formuparam;
                const f32 lp = glm::length(p);
                a += std::abs(lp - pa);
                pa = lp;
            }

            const f32 dm = std::max(0.0f, darkmatter - a * a * 0.001f);
            a *= a * a;
            if (r > 6)
                fade *= 1.0f - dm;
            v += fade;
            v += glm::vec3(s, s * s, s * s * s * s) * a * brightness * fade;
            fade *= distfading;
            s += stepsize;
        }

        v = glm::mix(glm::vec3(glm::length(v)), v, saturation);
        return glm::max(v * 0.01f * intensity, glm::vec3(0.0f));
    }

    u64 StarNestSky::HashParameters(const StarNestParameters& params, u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

        constexpr u64 kFnvOffset = 1469598103934665603ull;
        constexpr u64 kFnvPrime = 1099511628211ull;

        auto mix = [](u64 h, u32 v) noexcept
        {
            h ^= static_cast<u64>(v);
            h *= kFnvPrime;
            return h;
        };
        const auto bitsOf = [](f32 v) noexcept
        {
            u32 bits;
            std::memcpy(&bits, &v, sizeof(bits));
            return bits;
        };

        u64 hash = kFnvOffset;
        hash = mix(hash, bitsOf(params.Offset.x));
        hash = mix(hash, bitsOf(params.Offset.y));
        hash = mix(hash, bitsOf(params.Offset.z));
        hash = mix(hash, bitsOf(params.Rotation1));
        hash = mix(hash, bitsOf(params.Rotation2));
        hash = mix(hash, bitsOf(params.Formuparam));
        hash = mix(hash, bitsOf(params.StepSize));
        hash = mix(hash, bitsOf(params.Tile));
        hash = mix(hash, bitsOf(params.Brightness));
        hash = mix(hash, bitsOf(params.DarkMatter));
        hash = mix(hash, bitsOf(params.DistFading));
        hash = mix(hash, bitsOf(params.Saturation));
        hash = mix(hash, bitsOf(params.Intensity));
        hash = mix(hash, static_cast<u32>(params.Iterations));
        hash = mix(hash, static_cast<u32>(params.VolSteps));
        hash = mix(hash, resolution);
        return hash;
    }

    Ref<EnvironmentMap> StarNestSky::Generate(const StarNestParameters& params, u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

        if (resolution < 8 || resolution > 4096)
        {
            OLO_CORE_WARN("StarNestSky::Generate: clamping resolution {} to [8, 4096]", resolution);
            resolution = glm::clamp(resolution, 8u, 4096u);
        }

        ShaderLibrary& shaderLibrary = Renderer3D::GetShaderLibrary();
        if (!shaderLibrary.Exists("StarNestSky"))
        {
            OLO_CORE_ERROR("StarNestSky::Generate: StarNestSky shader not found in shader library");
            return nullptr;
        }
        auto shader = shaderLibrary.Get("StarNestSky");

        CubemapSpecification cubeSpec;
        cubeSpec.Width = resolution;
        cubeSpec.Height = resolution;
        cubeSpec.Format = ImageFormat::RGBA32F;
        cubeSpec.GenerateMips = false;
        auto cubemap = TextureCubemap::Create(cubeSpec);
        if (!cubemap)
        {
            OLO_CORE_ERROR("StarNestSky::Generate: failed to allocate cubemap");
            return nullptr;
        }

        auto cameraUBO = UniformBuffer::Create(
            ShaderBindingLayout::CameraUBO::GetSize(),
            ShaderBindingLayout::UBO_CAMERA);
        if (!cameraUBO)
        {
            OLO_CORE_ERROR("StarNestSky::Generate: failed to allocate camera UBO");
            return nullptr;
        }

        const auto ubo = ComputeUBO(params);
        auto skyUBO = UniformBuffer::Create(
            sizeof(StarNestSkyUBO),
            ShaderBindingLayout::UBO_STAR_NEST_SKY);
        if (!skyUBO)
        {
            OLO_CORE_ERROR("StarNestSky::Generate: failed to allocate Star Nest sky UBO");
            return nullptr;
        }
        skyUBO->SetData(&ubo, sizeof(StarNestSkyUBO));

        if (!SkyBake::RenderSkyToCubemap(cubemap, shader, cameraUBO, skyUBO))
        {
            OLO_CORE_ERROR("StarNestSky::Generate: cubemap bake failed");
            return nullptr;
        }

        // Disable the IBL disk cache: the generated cubemap's path is a constant
        // debug name, so the cache key can't distinguish two different nebulae —
        // leaving it on would serve a stale IBL when the parameters change.
        IBLConfiguration iblConfig;
        iblConfig.UseDiskCache = false;
        return EnvironmentMap::CreateFromCubemap(cubemap, iblConfig);
    }
} // namespace OloEngine
