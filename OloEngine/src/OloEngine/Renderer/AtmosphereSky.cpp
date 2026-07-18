#include "OloEnginePCH.h"
#include "OloEngine/Renderer/AtmosphereSky.h"

#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/SkyCubemapBake.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] f32 SanitizeClamped(f32 v, f32 lo, f32 hi, f32 fallback)
        {
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
        }

        // ── CPU mirrors of the AtmosphereSky.glsl night-layer helpers ──
        // Structurally identical, not bit-exact (same contract as StarNestSky's
        // CPU evaluator). Keep BOTH sides in sync — the shader names each
        // mirrored function.

        [[nodiscard]] f32 Hash13(const glm::vec3& p)
        {
            const f32 h = std::sin(glm::dot(p, glm::vec3(127.1f, 311.7f, 74.7f))) * 43758.5453f;
            return h - std::floor(h);
        }

        [[nodiscard]] glm::vec3 Hash33(const glm::vec3& p)
        {
            return { Hash13(p), Hash13(p + glm::vec3(19.19f, 0.0f, 0.0f)),
                     Hash13(p + glm::vec3(0.0f, 47.31f, 0.0f)) };
        }

        [[nodiscard]] f32 SmoothStepF(f32 edge0, f32 edge1, f32 x)
        {
            const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Mirrors starField() in AtmosphereSky.glsl.
        [[nodiscard]] f32 StarField(const glm::vec3& dir, f32 rotation, f32 intensity)
        {
            const f32 c = std::cos(rotation);
            const f32 s = std::sin(rotation);
            const glm::vec3 d(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);

            const glm::vec3 p = d * 60.0f;
            const glm::vec3 cell = glm::floor(p);
            const glm::vec3 f = p - cell;
            const glm::vec3 starPos = Hash33(cell);
            const f32 dist = glm::length(f - starPos);
            // Sparse bright stars: gate most cells off, sharpen the rest.
            const f32 lum = std::pow(Hash13(cell + glm::vec3(17.0f)), 14.0f);
            const f32 star = SmoothStepF(0.18f, 0.0f, dist) * lum;
            return star * intensity * 60.0f;
        }

        // Mirrors nightLayer() in AtmosphereSky.glsl: base gradient + moon
        // glow + moon disk + stars, all scaled by the night brightness lane.
        [[nodiscard]] glm::vec3 NightLayer(const glm::vec3& dir, const AtmosphereSkyUBO& ubo)
        {
            const glm::vec3 moonDir(ubo.MoonDirection);
            const f32 starIntensity = ubo.NightParams.y;
            const f32 moonIntensity = ubo.NightParams.z;
            const f32 nightBrightness = ubo.NightParams.w;
            const f32 starRotation = ubo.NightParams2.x;
            const f32 moonIllum = ubo.NightParams2.y;

            const f32 up = std::clamp(dir.y, 0.0f, 1.0f);
            // Horizon slightly brighter than the zenith (airglow); moonlight
            // lifts the whole dome with its illuminated fraction.
            glm::vec3 night = glm::mix(glm::vec3(0.035f, 0.045f, 0.085f),
                                       glm::vec3(0.010f, 0.013f, 0.026f), up);
            night *= 1.0f + 1.5f * moonIllum * std::min(moonIntensity, 1.0f);

            // Broad moon halo.
            const f32 cosGamma = std::clamp(glm::dot(dir, moonDir), -1.0f, 1.0f);
            night += glm::vec3(0.28f, 0.33f, 0.42f) *
                     (std::pow(std::max(cosGamma, 0.0f), 24.0f) * 0.6f * moonIntensity);

            // Moon disk (uniform shading scaled by illumination — the
            // crescent terminator is a future nicety, documented in the .h).
            if (cosGamma > ubo.MoonDirection.w)
            {
                const f32 t = std::clamp((cosGamma - ubo.MoonDirection.w) /
                                             std::max(1.0f - ubo.MoonDirection.w, 1e-6f),
                                         0.0f, 1.0f);
                night += glm::vec3(0.95f, 0.93f, 0.88f) *
                         (4.0f * moonIntensity * (0.2f + 0.8f * moonIllum) * SmoothStepF(0.0f, 1.0f, t));
            }

            // Stars fade near the horizon (atmospheric extinction).
            night += glm::vec3(0.9f, 0.95f, 1.0f) *
                     (StarField(dir, starRotation, starIntensity) * SmoothStepF(-0.05f, 0.15f, dir.y));

            return night * nightBrightness;
        }

        // Mirrors the 2D value-noise FBM cloud tint in AtmosphereSky.glsl.
        [[nodiscard]] f32 Hash12(const glm::vec2& p)
        {
            const f32 h = std::sin(glm::dot(p, glm::vec2(127.1f, 311.7f))) * 43758.5453f;
            return h - std::floor(h);
        }

        [[nodiscard]] f32 ValueNoise2D(const glm::vec2& p)
        {
            const glm::vec2 i = glm::floor(p);
            const glm::vec2 f = p - i;
            const glm::vec2 u = f * f * (3.0f - 2.0f * f);
            const f32 a = Hash12(i);
            const f32 b = Hash12(i + glm::vec2(1.0f, 0.0f));
            const f32 c = Hash12(i + glm::vec2(0.0f, 1.0f));
            const f32 d = Hash12(i + glm::vec2(1.0f, 1.0f));
            return glm::mix(glm::mix(a, b, u.x), glm::mix(c, d, u.x), u.y);
        }

        [[nodiscard]] f32 CloudFBM(const glm::vec2& p)
        {
            f32 sum = 0.0f;
            f32 amp = 0.5f;
            glm::vec2 q = p;
            for (int i = 0; i < 3; ++i)
            {
                sum += amp * ValueNoise2D(q);
                q *= 2.17f;
                amp *= 0.5f;
            }
            return sum;
        }
    } // namespace

    AtmosphereSkyUBO AtmosphereSky::ComputeUBO(const AtmosphereSkyParameters& params)
    {
        OLO_PROFILE_FUNCTION();

        AtmosphereSkyUBO ubo{};
        ubo.Day = ProceduralSky::ComputeCoefficients(params.Day);

        // Moon direction: normalize defensively; a zero/NaN vector points down
        // (moon absent) rather than poisoning the bake.
        glm::vec3 moonDir = params.MoonDirection;
        const bool moonFinite = std::isfinite(moonDir.x) && std::isfinite(moonDir.y) &&
                                std::isfinite(moonDir.z);
        const f32 moonLen = moonFinite ? glm::length(moonDir) : 0.0f;
        moonDir = (moonLen > 1e-4f) ? moonDir / moonLen : glm::vec3(0.0f, -1.0f, 0.0f);

        const f32 diskSize = SanitizeClamped(params.MoonDiskSize, 0.1f, 10.0f, 1.0f);
        ubo.MoonDirection = glm::vec4(moonDir, std::cos(MoonNominalAngularRadius * diskSize));

        ubo.NightParams = glm::vec4(SanitizeClamped(params.NightBlendFactor, 0.0f, 1.0f, 0.0f),
                                    SanitizeClamped(params.StarIntensity, 0.0f, 8.0f, 1.0f),
                                    SanitizeClamped(params.MoonIntensity, 0.0f, 10.0f, 1.0f),
                                    SanitizeClamped(params.NightBrightness, 0.0f, 10.0f, 0.35f));
        ubo.NightParams2 = glm::vec4(std::isfinite(params.StarRotationRadians) ? params.StarRotationRadians : 0.0f,
                                     SanitizeClamped(params.MoonIlluminatedFraction, 0.0f, 1.0f, 0.5f),
                                     SanitizeClamped(params.CloudCoverage, 0.0f, 1.0f, 0.0f),
                                     SanitizeClamped(params.CloudWetness, 0.0f, 1.0f, 0.0f));
        return ubo;
    }

    u64 AtmosphereSky::HashParameters(const AtmosphereSkyParameters& params, u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

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

        // Start from the day half's own hash so every Preetham field counts.
        u64 hash = ProceduralSky::HashParameters(params.Day, resolution);
        hash = mix(hash, bitsOf(params.MoonDirection.x));
        hash = mix(hash, bitsOf(params.MoonDirection.y));
        hash = mix(hash, bitsOf(params.MoonDirection.z));
        hash = mix(hash, bitsOf(params.MoonDiskSize));
        hash = mix(hash, bitsOf(params.MoonIlluminatedFraction));
        hash = mix(hash, bitsOf(params.MoonIntensity));
        hash = mix(hash, bitsOf(params.StarIntensity));
        hash = mix(hash, bitsOf(params.NightBlendFactor));
        hash = mix(hash, bitsOf(params.NightBrightness));
        hash = mix(hash, bitsOf(params.StarRotationRadians));
        hash = mix(hash, bitsOf(params.CloudCoverage));
        hash = mix(hash, bitsOf(params.CloudWetness));
        return hash;
    }

    glm::vec3 AtmosphereSky::EvaluateAtDirection(const AtmosphereSkyUBO& ubo, const glm::vec3& viewDir)
    {
        const glm::vec3 dir = glm::normalize(viewDir);
        const f32 nightBlend = std::clamp(ubo.NightParams.x, 0.0f, 1.0f);

        // Day half — the Preetham evaluator this mirrors byte-for-byte.
        glm::vec3 day(0.0f);
        if (nightBlend < 1.0f)
            day = ProceduralSky::EvaluateAtDirection(ubo.Day, dir);

        // Night half.
        glm::vec3 night(0.0f);
        if (nightBlend > 0.0f)
            night = NightLayer(dir, ubo);

        glm::vec3 sky = glm::mix(day, night, nightBlend);

        // Bake-time cloud tint (mirrors the shader's cloud block).
        const f32 coverage = ubo.NightParams2.z;
        if (coverage > 0.0f && dir.y > 0.02f)
        {
            const glm::vec2 uv = glm::vec2(dir.x, dir.z) / (dir.y + 0.18f) * 0.55f;
            const f32 fbm = CloudFBM(uv);
            // Damped to a TINT (mirrors AtmosphereSky.glsl — see the shader
            // comment; tuned live against the AtmosphereTest scene).
            const f32 cloud = SmoothStepF(1.05f - coverage, 1.25f - coverage, fbm) *
                              SmoothStepF(0.02f, 0.12f, dir.y) *
                              (0.25f + 0.45f * coverage);
            const f32 wetnessDarken = 1.0f - 0.55f * ubo.NightParams2.w;
            const glm::vec3 dayCloud = glm::vec3(0.82f, 0.83f, 0.86f) * wetnessDarken;
            const glm::vec3 nightCloud = glm::vec3(0.05f, 0.06f, 0.08f) * ubo.NightParams.w;
            sky = glm::mix(sky, glm::mix(dayCloud, nightCloud, nightBlend), cloud);
        }

        return glm::max(sky, glm::vec3(0.0f));
    }

    Ref<EnvironmentMap> AtmosphereSky::Generate(const AtmosphereSkyParameters& params, u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

        if (resolution < 8 || resolution > 4096)
        {
            OLO_CORE_WARN("AtmosphereSky::Generate: clamping resolution {} to [8, 4096]", resolution);
            resolution = glm::clamp(resolution, 8u, 4096u);
        }

        ShaderLibrary& shaderLibrary = Renderer3D::GetShaderLibrary();
        if (!shaderLibrary.Exists("AtmosphereSky"))
        {
            OLO_CORE_ERROR("AtmosphereSky::Generate: AtmosphereSky shader not found in shader library");
            return nullptr;
        }
        auto shader = shaderLibrary.Get("AtmosphereSky");

        CubemapSpecification cubeSpec;
        cubeSpec.Width = resolution;
        cubeSpec.Height = resolution;
        cubeSpec.Format = ImageFormat::RGBA32F;
        cubeSpec.GenerateMips = false;
        auto cubemap = TextureCubemap::Create(cubeSpec);
        if (!cubemap)
        {
            OLO_CORE_ERROR("AtmosphereSky::Generate: failed to allocate cubemap");
            return nullptr;
        }

        auto cameraUBO = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(),
                                               ShaderBindingLayout::UBO_CAMERA);
        if (!cameraUBO)
        {
            OLO_CORE_ERROR("AtmosphereSky::Generate: failed to allocate camera UBO");
            return nullptr;
        }

        const auto ubo = ComputeUBO(params);
        auto skyUBO = UniformBuffer::Create(sizeof(AtmosphereSkyUBO),
                                            ShaderBindingLayout::UBO_ATMOSPHERE_SKY);
        if (!skyUBO)
        {
            OLO_CORE_ERROR("AtmosphereSky::Generate: failed to allocate atmosphere sky UBO");
            return nullptr;
        }
        skyUBO->SetData(&ubo, sizeof(AtmosphereSkyUBO));

        if (!SkyBake::RenderSkyToCubemap(cubemap, shader, cameraUBO, skyUBO))
        {
            OLO_CORE_ERROR("AtmosphereSky::Generate: cubemap bake failed");
            return nullptr;
        }

        // Same IBL handoff as the other procedural skies; disk cache must
        // stay off (constant "Generated Cubemap" debug path — see
        // ProceduralSky::Generate for the full rationale).
        IBLConfiguration iblConfig;
        iblConfig.UseDiskCache = false;
        return EnvironmentMap::CreateFromCubemap(cubemap, iblConfig);
    }
} // namespace OloEngine
