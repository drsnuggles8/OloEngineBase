#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ProceduralSky.h"

#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    namespace
    {
        // Preetham table 2 — Perez F-function coefficients are linear in turbidity.
        // Row order matches the original paper: (A,B,C,D,E) for x, y, then Y.
        // Each row is (slope, intercept) so the coefficient = slope * T + intercept.
        struct PerezRow
        {
            f32 Slope;
            f32 Intercept;
        };

        // Chromaticity x
        constexpr PerezRow kAx{ -0.0193f, -0.2592f };
        constexpr PerezRow kBx{ -0.0665f, 0.0008f };
        constexpr PerezRow kCx{ -0.0004f, 0.2125f };
        constexpr PerezRow kDx{ -0.0641f, -0.8989f };
        constexpr PerezRow kEx{ -0.0033f, 0.0452f };
        // Chromaticity y
        constexpr PerezRow kAy{ -0.0167f, -0.2608f };
        constexpr PerezRow kBy{ -0.0950f, 0.0092f };
        constexpr PerezRow kCy{ -0.0079f, 0.2102f };
        constexpr PerezRow kDy{ -0.0441f, -1.6537f };
        constexpr PerezRow kEy{ -0.0109f, 0.0529f };
        // Luminance Y (kcd/m^2 scale)
        constexpr PerezRow kAY{ 0.1787f, -1.4630f };
        constexpr PerezRow kBY{ -0.3554f, 0.4275f };
        constexpr PerezRow kCY{ -0.0227f, 5.3251f };
        constexpr PerezRow kDY{ 0.1206f, -2.5771f };
        constexpr PerezRow kEY{ -0.0670f, 0.3703f };

        [[nodiscard]] inline f32 EvalRow(const PerezRow& r, f32 T) noexcept
        {
            return r.Slope * T + r.Intercept;
        }

        // Preetham zenith luminance Yz in kcd/m^2 (paper eq. A.2). thetaS is
        // the solar zenith angle in radians.
        [[nodiscard]] f32 ComputeZenithLuminance(f32 turbidity, f32 thetaS) noexcept
        {
            const f32 chi = (4.0f / 9.0f - turbidity / 120.0f) *
                            (glm::pi<f32>() - 2.0f * thetaS);
            return (4.0453f * turbidity - 4.9710f) * std::tan(chi) -
                   0.2155f * turbidity + 2.4192f;
        }

        // Preetham zenith chromaticity x as a polynomial of solar zenith
        // angle in radians, with quadratic turbidity dependence (table 1
        // in the paper, equation A.1).
        [[nodiscard]] f32 ComputeZenithChromaticityX(f32 turbidity, f32 thetaS) noexcept
        {
            const f32 T2 = turbidity * turbidity;
            const f32 t3 = thetaS * thetaS * thetaS;
            const f32 t2 = thetaS * thetaS;
            const f32 t1 = thetaS;

            return T2 * (0.00166f * t3 - 0.00375f * t2 + 0.00209f * t1 + 0.0f) +
                   turbidity * (-0.02903f * t3 + 0.06377f * t2 - 0.03202f * t1 + 0.00394f) +
                   (0.11693f * t3 - 0.21196f * t2 + 0.06052f * t1 + 0.25886f);
        }

        // Preetham zenith chromaticity y, same structure as X.
        [[nodiscard]] f32 ComputeZenithChromaticityY(f32 turbidity, f32 thetaS) noexcept
        {
            const f32 T2 = turbidity * turbidity;
            const f32 t3 = thetaS * thetaS * thetaS;
            const f32 t2 = thetaS * thetaS;
            const f32 t1 = thetaS;

            return T2 * (0.00275f * t3 - 0.00610f * t2 + 0.00317f * t1 + 0.0f) +
                   turbidity * (-0.04214f * t3 + 0.08970f * t2 - 0.04153f * t1 + 0.00516f) +
                   (0.15346f * t3 - 0.26756f * t2 + 0.06670f * t1 + 0.26688f);
        }

        // CIE xyY -> linear sRGB (D65). Uses the standard XYZ-to-linear-sRGB
        // matrix from Bruce Lindbloom / sRGB specification.
        [[nodiscard]] glm::vec3 XYYToLinearRGB(f32 cx, f32 cy, f32 Y) noexcept
        {
            // Guard against division by zero / sub-horizon nonsense.
            const f32 safeY = std::max(cy, 1e-6f);
            const f32 X = (cx / safeY) * Y;
            const f32 Z = ((1.0f - cx - cy) / safeY) * Y;

            // sRGB D65 from CIE 1931 XYZ
            glm::vec3 rgb{
                3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
                -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
                0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z
            };
            return rgb;
        }

        // Perez F distribution function (equation 3 of Preetham 1999).
        //   F(theta, gamma) = (1 + A * exp(B / cos(theta))) *
        //                     (1 + C * exp(D * gamma) + E * cos^2(gamma))
        // theta is the angle from zenith of the view direction, gamma is the
        // angle from the sun direction.
        [[nodiscard]] f32 PerezF(f32 A, f32 B, f32 C, f32 D, f32 E,
                                 f32 cosTheta, f32 cosGamma, f32 gamma) noexcept
        {
            // Guard cos(theta) away from 0 — exp(B/0) explodes near the horizon.
            const f32 safeCosTheta = std::max(cosTheta, 1e-3f);
            const f32 term1 = 1.0f + A * std::exp(B / safeCosTheta);
            const f32 term2 = 1.0f + C * std::exp(D * gamma) + E * cosGamma * cosGamma;
            return term1 * term2;
        }
    } // namespace

    PreethamCoefficientsUBO ProceduralSky::ComputeCoefficients(const PreethamParameters& params)
    {
        OLO_PROFILE_FUNCTION();

        // Sanitise inputs — extreme turbidity / sub-horizon sun otherwise
        // produces NaN-tinted skies.
        const f32 T = glm::clamp(params.Turbidity, 1.7f, 10.0f);

        // Sun direction must be unit; tolerate slightly off-unit inputs by
        // normalising defensively.  Sub-horizon sun is clamped a few
        // degrees above the horizon to keep Preetham bounded — once the
        // sun drops below the horizon the analytic model is undefined
        // (chi grows past pi/2 and tan() flips sign), so use a small
        // positive elevation as the lower bound. Callers that want
        // nighttime sky should swap to a different model anyway.
        glm::vec3 sunDir = params.SunDirection;
        const f32 sunLen = glm::length(sunDir);
        if (sunLen < 1e-6f)
            sunDir = glm::vec3(0.0f, 1.0f, 0.0f);
        else
            sunDir /= sunLen;

        constexpr f32 kMinSunElevationDeg = 2.0f;
        const f32 minSinElev = std::sin(glm::radians(kMinSunElevationDeg));
        if (sunDir.y < minSinElev)
        {
            // Lift the sun up to the floor while keeping its azimuth.
            const f32 horizLen = std::sqrt(std::max(0.0f, 1.0f - minSinElev * minSinElev));
            const glm::vec2 horizDir{ sunDir.x, sunDir.z };
            const f32 horizMag = glm::length(horizDir);
            const glm::vec2 horizUnit = horizMag > 1e-6f ? horizDir / horizMag : glm::vec2(0.0f, 1.0f);
            sunDir = glm::vec3(horizUnit.x * horizLen, minSinElev, horizUnit.y * horizLen);
        }

        // Solar zenith angle (theta_s).  sunDir.y = cos(theta_s) when y is up.
        const f32 cosThetaS = glm::clamp(sunDir.y, 0.0f, 1.0f);
        const f32 thetaS = std::acos(cosThetaS);

        // Zenith luminance Yz is in kcd/m^2 (Preetham uses kilo-candela). We
        // keep it in kcd and let `Exposure` (default ~0.1) map it into the
        // linear-HDR range the skybox + IBL pipeline expects (~0.5-2). The
        // previous code multiplied by 1000 to reach cd/m^2, which combined
        // with exposure produced values in the hundreds — the scene
        // tonemapper then crushed sky AND IBL ambient to pure white.
        const f32 Yz = std::max(ComputeZenithLuminance(T, thetaS), 0.0f);
        const f32 xz = ComputeZenithChromaticityX(T, thetaS);
        const f32 yz = ComputeZenithChromaticityY(T, thetaS);

        // Pack F coefficients per channel: (Fx, Fy, FY, _).
        PreethamCoefficientsUBO ubo{};
        ubo.A = glm::vec4(EvalRow(kAx, T), EvalRow(kAy, T), EvalRow(kAY, T), 0.0f);
        ubo.B = glm::vec4(EvalRow(kBx, T), EvalRow(kBy, T), EvalRow(kBY, T), 0.0f);
        ubo.C = glm::vec4(EvalRow(kCx, T), EvalRow(kCy, T), EvalRow(kCY, T), 0.0f);
        ubo.D = glm::vec4(EvalRow(kDx, T), EvalRow(kDy, T), EvalRow(kDY, T), 0.0f);
        ubo.E = glm::vec4(EvalRow(kEx, T), EvalRow(kEy, T), EvalRow(kEY, T), 0.0f);

        ubo.ZenithXYY = glm::vec4(xz, yz, Yz, 0.0f);

        // Sun disk: pre-compute cos(angular radius * size multiplier) so the
        // shader can test cos(gamma) >= ubo.SunDirection.w for the disk.
        const f32 angularRadius = SunNominalAngularRadius *
                                  std::max(params.SunDiskSize, 1e-3f);
        const f32 cosDisk = std::cos(angularRadius);
        ubo.SunDirection = glm::vec4(sunDir, cosDisk);

        ubo.Params = glm::vec4(
            std::max(params.Exposure, 0.0f),
            std::max(params.SunIntensity, 0.0f),
            params.ShowSunDisk ? 1.0f : 0.0f,
            0.0f);

        return ubo;
    }

    glm::vec3 ProceduralSky::EvaluateAtDirection(const PreethamCoefficientsUBO& coeffs,
                                                 const glm::vec3& viewDir)
    {
        const glm::vec3 sunDir{ coeffs.SunDirection };

        // View direction is assumed normalized; cosTheta = dot(viewDir, up).
        const f32 cosTheta = glm::clamp(viewDir.y, 0.0f, 1.0f);
        const f32 cosGamma = glm::clamp(glm::dot(viewDir, sunDir), -1.0f, 1.0f);
        const f32 gamma = std::acos(cosGamma);

        // Reference values at zenith for normalisation: F(0, thetaS).
        const f32 cosThetaS = glm::clamp(sunDir.y, 0.0f, 1.0f);
        const f32 thetaS = std::acos(cosThetaS);

        // Sample Perez at zenith reference (theta=0 so cos(theta)=1).
        const f32 FzX = PerezF(coeffs.A.x, coeffs.B.x, coeffs.C.x, coeffs.D.x, coeffs.E.x,
                               1.0f, cosThetaS, thetaS);
        const f32 FzY = PerezF(coeffs.A.y, coeffs.B.y, coeffs.C.y, coeffs.D.y, coeffs.E.y,
                               1.0f, cosThetaS, thetaS);
        const f32 FzL = PerezF(coeffs.A.z, coeffs.B.z, coeffs.C.z, coeffs.D.z, coeffs.E.z,
                               1.0f, cosThetaS, thetaS);

        // Sample at view direction.
        const f32 FX = PerezF(coeffs.A.x, coeffs.B.x, coeffs.C.x, coeffs.D.x, coeffs.E.x,
                              cosTheta, cosGamma, gamma);
        const f32 FY = PerezF(coeffs.A.y, coeffs.B.y, coeffs.C.y, coeffs.D.y, coeffs.E.y,
                              cosTheta, cosGamma, gamma);
        const f32 FL = PerezF(coeffs.A.z, coeffs.B.z, coeffs.C.z, coeffs.D.z, coeffs.E.z,
                              cosTheta, cosGamma, gamma);

        // Q(theta, gamma) = Q_z * F(theta,gamma) / F(0, thetaS)
        const f32 cx = coeffs.ZenithXYY.x * (FX / std::max(FzX, 1e-6f));
        const f32 cy = coeffs.ZenithXYY.y * (FY / std::max(FzY, 1e-6f));
        const f32 cY = coeffs.ZenithXYY.z * (FL / std::max(FzL, 1e-6f));

        // Luminance tonemap (mirrors ProceduralSky.glsl): compress the
        // zenith->horizon dynamic range into [0,1) while preserving the
        // chromaticity, so the blue survives the downstream scene tonemapper
        // instead of clipping to white.
        const f32 Yt = 1.0f - std::exp(-coeffs.Params.x * cY);
        glm::vec3 rgb = XYYToLinearRGB(cx, cy, Yt);

        return glm::max(rgb, glm::vec3(0.0f));
    }

    u64 ProceduralSky::HashParameters(const PreethamParameters& params, u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

        // FNV-1a over the parameter bytes plus resolution. Cheap and adequate
        // for "did anything change since last bake" checks.
        constexpr u64 kFnvOffset = 1469598103934665603ull;
        constexpr u64 kFnvPrime = 1099511628211ull;

        auto mix = [](u64 h, u32 v) noexcept
        {
            h ^= static_cast<u64>(v);
            h *= kFnvPrime;
            return h;
        };

        u64 hash = kFnvOffset;
        const auto bitsOf = [](f32 v) noexcept
        {
            u32 bits;
            std::memcpy(&bits, &v, sizeof(bits));
            return bits;
        };
        hash = mix(hash, bitsOf(params.SunDirection.x));
        hash = mix(hash, bitsOf(params.SunDirection.y));
        hash = mix(hash, bitsOf(params.SunDirection.z));
        hash = mix(hash, bitsOf(params.Turbidity));
        hash = mix(hash, bitsOf(params.Exposure));
        hash = mix(hash, bitsOf(params.SunIntensity));
        hash = mix(hash, bitsOf(params.SunDiskSize));
        hash = mix(hash, params.ShowSunDisk ? 1u : 0u);
        hash = mix(hash, resolution);
        return hash;
    }

    namespace
    {
        // Update the IBL camera UBO for face N: identity-like view with
        // a 90-degree FOV projection, mirrored to the same convention as
        // IBLPrecompute::RenderToCubemap.
        struct CaptureMatrices
        {
            glm::mat4 Views[6];
            glm::mat4 Projection;
        };

        const CaptureMatrices& GetCaptureMatrices()
        {
            static const CaptureMatrices kMatrices = []
            {
                CaptureMatrices m{};
                m.Views[0] = glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Views[1] = glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Views[2] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
                m.Views[3] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
                m.Views[4] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Views[5] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
                return m;
            }();
            return kMatrices;
        }

        // Bake the procedural sky into all 6 cubemap faces. Mirrors the
        // single-mip path of IBLPrecompute::RenderToCubemap but does not
        // bind a source cubemap — the shader generates radiance from the
        // PreethamCoefficients UBO directly.
        bool BakeSkyToCubemap(Ref<TextureCubemap> cubemap,
                              Ref<Shader> shader,
                              Ref<UniformBuffer> cameraUBO,
                              Ref<UniformBuffer> skyUBO)
        {
            OLO_PROFILE_FUNCTION();

            const u32 face = cubemap->GetWidth();
            const auto& mats = GetCaptureMatrices();

            const bool wasStencilEnabled = RenderCommand::IsStencilTestEnabled();
            if (wasStencilEnabled)
                RenderCommand::DisableStencilTest();

            shader->Bind();
            skyUBO->Bind();

            FramebufferSpecification fbSpec;
            fbSpec.Width = face;
            fbSpec.Height = face;
            fbSpec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::Depth };
            auto framebuffer = Framebuffer::Create(fbSpec);

            auto cubeMesh = MeshPrimitives::CreateSkyboxCube();
            if (!cubeMesh)
            {
                if (wasStencilEnabled)
                    RenderCommand::EnableStencilTest();
                return false;
            }

            for (u32 i = 0; i < 6; ++i)
            {
                OLO_PROFILE_SCOPE("ProceduralSky::BakeFace");

                // Update CameraUBO with face matrices. We use the engine's
                // standard UBO_CAMERA slot so the shader vertex stage can
                // pick up u_ViewProjection just like every other skybox-style
                // shader in the codebase.
                ShaderBindingLayout::CameraUBO data;
                data.ViewProjection = mats.Projection * mats.Views[i];
                data.View = mats.Views[i];
                data.Projection = mats.Projection;
                data.Position = glm::vec3(0.0f);
                data._padding0 = 0.0f;
                cameraUBO->SetData(&data, ShaderBindingLayout::CameraUBO::GetSize());
                cameraUBO->Bind();

                framebuffer->Bind();
                RenderCommand::SetViewport(0, 0, face, face);
                RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
                RenderCommand::ClearColorAndDepth();

                auto vao = cubeMesh->GetVertexArray();
                vao->Bind();
                RenderCommand::DrawIndexed(vao);

                const u32 fbColor = framebuffer->GetColorAttachmentRendererID(0);
                RenderCommand::CopyImageSubDataFull(
                    fbColor, RendererAPI::TextureTargetType::Texture2D, 0, 0,
                    cubemap->GetRendererID(), RendererAPI::TextureTargetType::TextureCubeMap, 0, static_cast<i32>(i),
                    face, face);
            }

            framebuffer->Unbind();

            if (wasStencilEnabled)
                RenderCommand::EnableStencilTest();

            return true;
        }
    } // namespace

    Ref<EnvironmentMap> ProceduralSky::Generate(const PreethamParameters& params, u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

        if (resolution < 8 || resolution > 4096)
        {
            OLO_CORE_WARN("ProceduralSky::Generate: clamping resolution {} to [8, 4096]", resolution);
            resolution = glm::clamp(resolution, 8u, 4096u);
        }

        // Render command access requires the renderer system to be live.
        ShaderLibrary& shaderLibrary = Renderer3D::GetShaderLibrary();
        if (!shaderLibrary.Exists("ProceduralSky"))
        {
            OLO_CORE_ERROR("ProceduralSky::Generate: ProceduralSky shader not found in shader library");
            return nullptr;
        }

        auto shader = shaderLibrary.Get("ProceduralSky");

        // Sky cubemap target.
        CubemapSpecification cubeSpec;
        cubeSpec.Width = resolution;
        cubeSpec.Height = resolution;
        cubeSpec.Format = ImageFormat::RGBA32F;
        cubeSpec.GenerateMips = false;
        auto cubemap = TextureCubemap::Create(cubeSpec);
        if (!cubemap)
        {
            OLO_CORE_ERROR("ProceduralSky::Generate: failed to allocate cubemap");
            return nullptr;
        }

        // Camera UBO is shared with the rest of the IBL pipeline (binding
        // UBO_CAMERA); allocate locally rather than reaching into static
        // state inside IBLPrecompute.
        auto cameraUBO = UniformBuffer::Create(
            ShaderBindingLayout::CameraUBO::GetSize(),
            ShaderBindingLayout::UBO_CAMERA);

        // Procedural sky coefficient UBO at binding UBO_PROCEDURAL_SKY.
        const auto ubo = ComputeCoefficients(params);
        auto skyUBO = UniformBuffer::Create(
            sizeof(PreethamCoefficientsUBO),
            ShaderBindingLayout::UBO_PROCEDURAL_SKY);
        skyUBO->SetData(&ubo, sizeof(PreethamCoefficientsUBO));

        if (!BakeSkyToCubemap(cubemap, shader, cameraUBO, skyUBO))
        {
            OLO_CORE_ERROR("ProceduralSky::Generate: cubemap bake failed");
            return nullptr;
        }

        // Hand the cubemap off to the EnvironmentMap factory so the existing
        // IBL pipeline (irradiance, prefilter, BRDF LUT) does the rest for
        // free. Disable the IBL disk cache: the generated cubemap's path is a
        // constant debug name ("Generated Cubemap"), so the cache key can't
        // tell two different skies apart — leaving it on would serve stale
        // over-bright IBL when the sun / turbidity / exposure changes.
        IBLConfiguration iblConfig;
        iblConfig.UseDiskCache = false;
        auto envMap = EnvironmentMap::CreateFromCubemap(cubemap, iblConfig);
        return envMap;
    }
} // namespace OloEngine
