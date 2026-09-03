#pragma once

// Shared, editor-side logic behind `olo_postprocess_settings_get` (read-only) and
// `olo_postprocess_settings_set` (consented write) — issue #607's first open
// capability gap.
//
// The gap it closes: `olo_renderer_settings_set` reaches exactly five session-
// global knobs (upscale / tonemap / renderpath / depthprepass / softshadows).
// Everything else in the Post Processing panel was unreachable from an agent
// session — AO technique (GTAO vs SSAO vs none), the AO enables, GTAO
// radius/power/falloff/denoise, SSAO radius/bias/intensity/samples, every
// `*DebugView` flag, bloom, DOF, TAA, SSR/SSGI/contact shadows, and the whole
// fog block. While root-causing the GTAO grazing-angle collapse the A/B that
// mattered — *same scene, same pose, GTAO vs SSAO* — could not be driven at all;
// the workarounds were hand-editing the scene `.olo` and relaunching (slow,
// mutates project data, and could not switch `ActiveAOTechnique` live), or
// giving up and reasoning analytically. The read side was
// equally missing: parameter values had to be read off a screenshot of the panel.
//
// Scope — the two live settings PODs the renderer evaluates every frame:
//   * `PostProcessSettings` (Renderer3D::GetPostProcessSettings())
//   * `FogSettings`         (Renderer3D::GetFogSettings()), token-prefixed `Fog`
// `Tonemap` and `Upscale` are deliberately NOT here: `olo_renderer_settings_set`
// already owns them, and two write paths onto one field is exactly the kind of
// divergence that goes unnoticed. The tool description points at the sibling.
//
// Token = the C++ field name (`GTAORadius`, `FogDensity`), matched case- and
// separator-insensitively via Normalize. Naming them after the struct means an
// agent reading PostProcessSettings.h can guess every token, and a renamed field
// is a compile error here rather than a silently dead token.
//
// Restore semantics — restore-PRIOR-VALUE, NOT CommandHistory, identical to
// McpRendererSettings.h: these are session-global renderer settings, not scene /
// ECS data, so an undo-stack entry would be wrong. Every write reports
// `previousValue` + `restoreWith`; a scene reload also restores them.
//
// Range handling — every numeric field carries a Min/Max and the write CLAMPS to
// it (reporting `clamped: true` when it bit). The bounds mirror the engine's own
// `SanitizeSSR` / `SanitizeSSGI` / `SanitizeContactShadow` caps where those exist
// (`kSSRMaxSteps` et al. are referenced directly, so a cap change here is a
// compile-time follow), so an MCP write can never push the settings somewhere a
// scene load would reject — the same "a bounded write beats an unvalidated one"
// rule the generated MCP field registry follows.
//
// Everything here is httplib/editor/McpServer-free — it mutates the plain POD
// settings structs by reference, plus the schema-builder DSL and nlohmann::json —
// so the MCP test binary (which deliberately does NOT link McpTools.cpp)
// exercises the real schema + parse + apply code. The one renderer-bound side
// effect, `ActiveAOTechnique` needing `Renderer3D::ApplyRendererSettings()` to
// re-register the AO pass in the render graph (issue #533), is signalled out via
// `ApplyResult::RequiresRendererApply` instead of being called here.

#include "MCP/McpSchemaBuilder.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/SphereProxyAO.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace OloEngine::MCP::PostProcess
{
    using Json = nlohmann::json;

    // What kind of value a field holds — drives JSON coercion, the reported
    // `value` shape, and whether Min/Max or the enum table applies.
    enum class FieldType : u8
    {
        Bool,
        Float,
        Int,
        Enum, // integer-backed engine enum, exchanged as a named token
        Vec3, // 3-number array (fog colours)
    };

    // One allowed value of an enum-valued field: the stable token the agent
    // passes, the engine enum integer, and a human description.
    struct EnumValue
    {
        std::string_view Token;
        i32 Value;
        std::string_view Description;
    };

    inline constexpr std::array<EnumValue, 3> kAOTechniqueValues = { {
        { "none", static_cast<i32>(AOTechnique::None), "No ambient occlusion pass registered." },
        { "ssao", static_cast<i32>(AOTechnique::SSAO), "Classic hemisphere-kernel SSAO." },
        { "gtao", static_cast<i32>(AOTechnique::GTAO), "Ground-Truth Ambient Occlusion (horizon-based, HZB-accelerated)." },
    } };

    inline constexpr std::array<EnumValue, 3> kFogModeValues = { {
        { "linear", static_cast<i32>(FogMode::Linear), "Linear ramp between FogStart and FogEnd." },
        { "exponential", static_cast<i32>(FogMode::Exponential), "exp(-density * d)." },
        { "exponentialsquared", static_cast<i32>(FogMode::ExponentialSquared), "exp(-(density * d)^2) — the default." },
    } };

    // ---- accessor plumbing ---------------------------------------------------
    // Every scalar field is reached through a captureless get/set function-pointer
    // pair over the two live PODs, so the table below stays one line per field
    // without a per-type member-pointer union. Scalars (bool / int / float / enum)
    // all travel as f64; a Vec3 uses the separate glm pair.

    template<auto Mem>
    [[nodiscard]] f64 GetPp(const PostProcessSettings& pp, const FogSettings&)
    {
        using T = std::remove_cvref_t<decltype(pp.*Mem)>;
        if constexpr (std::is_enum_v<T>)
            return static_cast<f64>(static_cast<std::underlying_type_t<T>>(pp.*Mem));
        else
            return static_cast<f64>(pp.*Mem);
    }

    template<auto Mem>
    void SetPp(PostProcessSettings& pp, FogSettings&, f64 v)
    {
        using T = std::remove_cvref_t<decltype(pp.*Mem)>;
        if constexpr (std::is_enum_v<T>)
            pp.*Mem = static_cast<T>(static_cast<std::underlying_type_t<T>>(v));
        else
            pp.*Mem = static_cast<T>(v);
    }

    template<auto Mem>
    [[nodiscard]] f64 GetFog(const PostProcessSettings&, const FogSettings& fog)
    {
        using T = std::remove_cvref_t<decltype(fog.*Mem)>;
        if constexpr (std::is_enum_v<T>)
            return static_cast<f64>(static_cast<std::underlying_type_t<T>>(fog.*Mem));
        else
            return static_cast<f64>(fog.*Mem);
    }

    template<auto Mem>
    void SetFog(PostProcessSettings&, FogSettings& fog, f64 v)
    {
        using T = std::remove_cvref_t<decltype(fog.*Mem)>;
        if constexpr (std::is_enum_v<T>)
            fog.*Mem = static_cast<T>(static_cast<std::underlying_type_t<T>>(v));
        else
            fog.*Mem = static_cast<T>(v);
    }

    template<auto Mem>
    [[nodiscard]] glm::vec3 GetFogVec3(const PostProcessSettings&, const FogSettings& fog)
    {
        return fog.*Mem;
    }

    template<auto Mem>
    void SetFogVec3(PostProcessSettings&, FogSettings& fog, const glm::vec3& v)
    {
        fog.*Mem = v;
    }

    // One settable field. `Get`/`Set` are non-null for every scalar type;
    // `GetV3`/`SetV3` replace them for FieldType::Vec3. `Values` is non-empty only
    // for FieldType::Enum. `RequiresRendererApply` marks the fields whose effect
    // needs the render graph rebuilt (the AO technique swaps which AO pass is
    // registered — issue #533); the handler calls Renderer3D::ApplyRendererSettings.
    struct FieldInfo
    {
        std::string_view Token;
        std::string_view Group; // panel-ish grouping, for the introspection reply
        FieldType Type = FieldType::Float;
        f64 Min = 0.0;
        f64 Max = 0.0;
        std::span<const EnumValue> Values;
        bool RequiresRendererApply = false;
        std::string_view Description;
        f64 (*Get)(const PostProcessSettings&, const FogSettings&) = nullptr;
        void (*Set)(PostProcessSettings&, FogSettings&, f64) = nullptr;
        glm::vec3 (*GetV3)(const PostProcessSettings&, const FogSettings&) = nullptr;
        void (*SetV3)(PostProcessSettings&, FogSettings&, const glm::vec3&) = nullptr;
    };

// Table shorthands. #undef'd immediately after the table so they never leak.
#define OLO_PP_BOOL(name, group, desc)                                                                                                                     \
    FieldInfo                                                                                                                                              \
    {                                                                                                                                                      \
        #name, group, FieldType::Bool, 0.0, 1.0, {}, false, desc, &GetPp<&PostProcessSettings::name>, &SetPp<&PostProcessSettings::name>, nullptr, nullptr \
    }
#define OLO_PP_NUM(name, group, type, lo, hi, desc)                                                                                           \
    FieldInfo                                                                                                                                 \
    {                                                                                                                                         \
        #name, group, type, lo, hi, {}, false, desc, &GetPp<&PostProcessSettings::name>, &SetPp<&PostProcessSettings::name>, nullptr, nullptr \
    }
#define OLO_PP_ENUM(name, group, values, apply, desc)                                                                                                          \
    FieldInfo                                                                                                                                                  \
    {                                                                                                                                                          \
        #name, group, FieldType::Enum, 0.0, 0.0, values, apply, desc, &GetPp<&PostProcessSettings::name>, &SetPp<&PostProcessSettings::name>, nullptr, nullptr \
    }
#define OLO_FOG_BOOL(token, name, desc)                                                                                                      \
    FieldInfo                                                                                                                                \
    {                                                                                                                                        \
        token, "fog", FieldType::Bool, 0.0, 1.0, {}, false, desc, &GetFog<&FogSettings::name>, &SetFog<&FogSettings::name>, nullptr, nullptr \
    }
#define OLO_FOG_NUM(token, name, type, lo, hi, desc)                                                                            \
    FieldInfo                                                                                                                   \
    {                                                                                                                           \
        token, "fog", type, lo, hi, {}, false, desc, &GetFog<&FogSettings::name>, &SetFog<&FogSettings::name>, nullptr, nullptr \
    }
#define OLO_FOG_ENUM(token, name, values, desc)                                                                                                  \
    FieldInfo                                                                                                                                    \
    {                                                                                                                                            \
        token, "fog", FieldType::Enum, 0.0, 0.0, values, false, desc, &GetFog<&FogSettings::name>, &SetFog<&FogSettings::name>, nullptr, nullptr \
    }
#define OLO_FOG_VEC3(token, name, desc)                                                                                                              \
    FieldInfo                                                                                                                                        \
    {                                                                                                                                                \
        token, "fog", FieldType::Vec3, 0.0, 1.0, {}, false, desc, nullptr, nullptr, &GetFogVec3<&FogSettings::name>, &SetFogVec3<&FogSettings::name> \
    }

    // The full settable surface. Ranges mirror the engine's own sanitizers where
    // one exists (kSSRMaxSteps / kSSGIMaxRays / kContactShadowMaxSteps are
    // referenced directly, so a cap change there fails to compile here rather
    // than silently letting an MCP write exceed the runtime UBO clamp).
    inline const std::array kFields = {
        // ---- tone mapping / exposure ----------------------------------------
        OLO_PP_NUM(Exposure, "exposure", FieldType::Float, 0.0, 64.0,
                   "Manual exposure multiplier applied to the HDR scene colour (ignored while AutoExposureEnabled)."),
        OLO_PP_NUM(Gamma, "exposure", FieldType::Float, 1.0, 4.0, "Display gamma applied after tone mapping."),
        OLO_PP_BOOL(AutoExposureEnabled, "exposure",
                    "Histogram eye-adaptation drives the exposure multiplier instead of the manual Exposure above."),
        OLO_PP_NUM(AutoExposureMinLogLuminance, "exposure", FieldType::Float, -16.0, 16.0, "Histogram lower bound, log2 luminance."),
        OLO_PP_NUM(AutoExposureMaxLogLuminance, "exposure", FieldType::Float, -16.0, 16.0, "Histogram upper bound, log2 luminance."),
        OLO_PP_NUM(AutoExposureSpeedUp, "exposure", FieldType::Float, 0.0, 64.0, "Adaptation rate when brightening (per second)."),
        OLO_PP_NUM(AutoExposureSpeedDown, "exposure", FieldType::Float, 0.0, 64.0, "Adaptation rate when darkening (per second)."),
        OLO_PP_NUM(AutoExposureCompensation, "exposure", FieldType::Float, -8.0, 8.0, "EV bias; +1 doubles the resulting brightness."),
        OLO_PP_NUM(AutoExposureMinExposure, "exposure", FieldType::Float, 0.0001, 64.0, "Hard lower clamp on the metered exposure multiplier."),
        OLO_PP_NUM(AutoExposureMaxExposure, "exposure", FieldType::Float, 0.0001, 1024.0, "Hard upper clamp on the metered exposure multiplier."),
        OLO_PP_NUM(AutoExposureLowPercentile, "exposure", FieldType::Float, 0.0, 1.0, "Lower percentile of the metered non-black population."),
        OLO_PP_NUM(AutoExposureHighPercentile, "exposure", FieldType::Float, 0.0, 1.0, "Upper percentile of the metered non-black population."),

        // ---- ambient occlusion ----------------------------------------------
        OLO_PP_ENUM(ActiveAOTechnique, "ao", kAOTechniqueValues, /*RequiresRendererApply*/ true,
                    "Which AO pass is registered in the render graph. Switching rebuilds the render-graph topology "
                    "(issue #533) — this is the GTAO-vs-SSAO A/B lever. This live write is reset by scene reload."),
        OLO_PP_BOOL(SSAOEnabled, "ao", "Run the SSAO pass (also needs ActiveAOTechnique = ssao)."),
        OLO_PP_NUM(SSAORadius, "ao", FieldType::Float, 0.01, 32.0, "SSAO world-space sampling radius."),
        OLO_PP_NUM(SSAOBias, "ao", FieldType::Float, 0.0, 1.0, "SSAO depth bias (self-occlusion guard)."),
        OLO_PP_NUM(SSAOIntensity, "ao", FieldType::Float, 0.0, 16.0, "SSAO occlusion strength multiplier."),
        OLO_PP_NUM(SSAOSamples, "ao", FieldType::Int, 1.0, 128.0, "SSAO hemisphere kernel sample count."),
        OLO_PP_BOOL(SSAODebugView, "ao", "Show the raw SSAO buffer instead of the composite."),
        OLO_PP_BOOL(GTAOEnabled, "ao", "Run the GTAO pass (also needs ActiveAOTechnique = gtao)."),
        OLO_PP_NUM(GTAORadius, "ao", FieldType::Float, 0.01, 32.0, "GTAO world-space AO radius."),
        OLO_PP_NUM(GTAOPower, "ao", FieldType::Float, 0.1, 8.0, "GTAO contrast curve exponent."),
        OLO_PP_NUM(GTAOFalloffRange, "ao", FieldType::Float, 0.0, 1.0, "GTAO relative falloff distance."),
        OLO_PP_NUM(GTAOSampleDistribution, "ao", FieldType::Float, 0.1, 8.0, "GTAO sample-distance distribution power."),
        OLO_PP_NUM(GTAOThinCompensation, "ao", FieldType::Float, 0.0, 1.0, "GTAO thin-occluder compensation."),
        OLO_PP_NUM(GTAODepthMipOffset, "ao", FieldType::Float, 0.0, 8.0, "GTAO HZB mip-selection offset."),
        OLO_PP_BOOL(GTAODenoiseEnabled, "ao", "Run the GTAO bilateral denoise passes."),
        OLO_PP_NUM(GTAODenoisePasses, "ao", FieldType::Int, 0.0, 16.0, "GTAO bilateral blur pass count."),
        OLO_PP_NUM(GTAODenoiseBeta, "ao", FieldType::Float, 0.0, 16.0, "GTAO denoise edge sensitivity."),
        OLO_PP_BOOL(GTAODebugView, "ao", "Show the raw GTAO buffer instead of the composite."),

        // ---- analytic sphere-proxy AO (issue #710) ----------------------------
        // The off-screen-occluder term. Needs ActiveAOTechnique = gtao AND
        // GTAOEnabled: it multiplies into GTAO's full-resolution AO buffer.
        OLO_PP_BOOL(SphereProxyAOEnabled, "ao",
                    "Run the analytic sphere-proxy AO pass (also needs ActiveAOTechnique = gtao). "
                    "Occludes with proxies that are off-screen or behind the camera."),
        OLO_PP_NUM(SphereProxyAOStrength, "ao", FieldType::Float, 0.0, 1.0,
                   "How much of the sphere-proxy term reaches the AO buffer."),
        OLO_PP_NUM(SphereProxyAOMaxProxies, "ao", FieldType::Int, 0.0,
                   static_cast<f64>(SphereProxyAO::kMaxProxies),
                   "Per-frame proxy budget; the UBO array length is a hard ceiling."),
        OLO_PP_NUM(SphereProxyAOMaxRadius, "ao", FieldType::Float, 0.0, 1000.0,
                   "Drop occluders whose fitted proxy sphere is larger than this (the ground/terrain filter)."),
        OLO_PP_NUM(SphereProxyAOInfluenceScale, "ao", FieldType::Float, 1.0, 64.0,
                   "Tile-binning cutoff, in proxy radii."),
        OLO_PP_NUM(SphereProxyAOMaxOcclusion, "ao", FieldType::Float, 0.0, 1.0,
                   "Ceiling on the occlusion the whole proxy set may produce at one pixel; bounds "
                   "double counting where proxies overlap. 1.0 = the raw product."),
        OLO_PP_BOOL(SphereProxyAODebugView, "ao",
                    "Write the sphere-proxy term alone into the AO buffer instead of the product with GTAO."),

        // ---- variable rate compute shading (issue #683) ------------------------
        // Two gates: VRCSEnabled is the global kill switch (off = the classifier
        // never dispatches and every consumer runs full rate), VRCSGTAO is the
        // per-pass opt-in. Both rebuild the render graph when flipped.
        OLO_PP_BOOL(VRCSEnabled, "ao",
                    "Global VRCS kill switch: classify 8x8 tiles into 1x1/2x2/4x4 shading footprints."),
        OLO_PP_BOOL(VRCSGTAO, "ao", "Let the GTAO compute pass consume the VRCS shading rates."),
        OLO_PP_BOOL(VRCSAllow4x4, "ao", "Permit the 4x4 (16:1) footprint, not just 2x2."),
        OLO_PP_NUM(VRCSDepthThreshold, "ao", FieldType::Float, 0.0, 1.0,
                   "Max tile depth range, relative to its nearest surface, that may still coarsen. Lower = more full-rate tiles."),
        OLO_PP_NUM(VRCSNormalThreshold, "ao", FieldType::Float, 0.0, 1.0,
                   "Max tile normal spread (1 - |mean normal|) that may still coarsen. Lower = more full-rate tiles."),
        OLO_PP_NUM(VRCSLumaThreshold, "ao", FieldType::Float, 0.0, 16.0,
                   "Max previous-frame luminance range, relative to the tile's darkest pixel, that may still coarsen."),
        OLO_PP_NUM(VRCS4x4ToleranceScale, "ao", FieldType::Float, 0.0, 1.0,
                   "Multiplier the 4x4 test applies to all three thresholds, so 4x4 is a strict subset of 2x2."),
        OLO_PP_BOOL(VRCSDebugOverlay, "ao",
                    "Paint the per-invocation shading footprint into the AO term (1.0/0.5/0.25). Needs GTAODebugView to be visible."),

        // ---- bloom ------------------------------------------------------------
        OLO_PP_BOOL(BloomEnabled, "bloom", "Run the bloom down/upsample pyramid."),
        OLO_PP_NUM(BloomThreshold, "bloom", FieldType::Float, 0.0, 64.0, "Luminance above which a texel contributes to bloom."),
        OLO_PP_NUM(BloomIntensity, "bloom", FieldType::Float, 0.0, 16.0, "Bloom additive strength."),
        OLO_PP_NUM(BloomIterations, "bloom", FieldType::Int, 1.0, 12.0, "Bloom pyramid mip count."),

        // ---- vignette / chromatic aberration / FXAA / colour grading ---------
        OLO_PP_BOOL(VignetteEnabled, "vignette", "Apply the vignette darkening."),
        OLO_PP_NUM(VignetteIntensity, "vignette", FieldType::Float, 0.0, 1.0, "Vignette darkening strength."),
        OLO_PP_NUM(VignetteSmoothness, "vignette", FieldType::Float, 0.0, 1.0, "Vignette edge softness."),
        OLO_PP_BOOL(ChromaticAberrationEnabled, "chromaticaberration", "Apply the chromatic-aberration split."),
        OLO_PP_NUM(ChromaticAberrationIntensity, "chromaticaberration", FieldType::Float, 0.0, 0.1, "Chromatic-aberration UV offset scale."),
        OLO_PP_BOOL(FXAAEnabled, "antialiasing", "Run the FXAA post-AA pass."),
        OLO_PP_BOOL(ColorGradingEnabled, "colorgrading", "Apply the colour-grading LUT."),

        // ---- depth of field / motion blur -------------------------------------
        OLO_PP_BOOL(DOFEnabled, "dof", "Run the depth-of-field pass."),
        OLO_PP_NUM(DOFFocusDistance, "dof", FieldType::Float, 0.0, 10000.0, "Distance to the focal plane, world units."),
        OLO_PP_NUM(DOFFocusRange, "dof", FieldType::Float, 0.0, 10000.0, "Depth range that stays in focus."),
        OLO_PP_NUM(DOFBokehRadius, "dof", FieldType::Float, 0.0, 64.0, "Maximum circle-of-confusion radius in pixels."),
        OLO_PP_BOOL(MotionBlurEnabled, "motionblur", "Run the motion-blur pass."),
        OLO_PP_NUM(MotionBlurStrength, "motionblur", FieldType::Float, 0.0, 4.0, "Velocity scale applied to the blur."),
        OLO_PP_NUM(MotionBlurSamples, "motionblur", FieldType::Int, 1.0, 64.0, "Motion-blur tap count."),

        // ---- TAA / CAS / RCAS ---------------------------------------------------
        OLO_PP_BOOL(TAAEnabled, "antialiasing", "Run temporal anti-aliasing (velocity-reprojected accumulation)."),
        OLO_PP_NUM(TAAFeedback, "antialiasing", FieldType::Float, 0.0, 1.0, "TAA history blend weight; higher = smoother, slower response."),
        OLO_PP_NUM(TAASharpness, "antialiasing", FieldType::Float, 0.0, 1.0, "Post-TAA sharpen amount (0 = off)."),
        OLO_PP_BOOL(CASEnabled, "sharpen", "Run Contrast Adaptive Sharpening (native-resolution path)."),
        OLO_PP_NUM(CASSharpness, "sharpen", FieldType::Float, 0.0, 1.0, "CAS sharpening amount."),
        OLO_PP_NUM(RCASSharpness, "sharpen", FieldType::Float, 0.0, 1.0,
                   "FSR1 RCAS sharpening amount (the late sharpen on the upscale path; select the preset with "
                   "olo_renderer_settings_set { setting: 'upscale' })."),

        // ---- screen-space reflections -----------------------------------------
        OLO_PP_BOOL(SSREnabled, "ssr", "Run screen-space reflections (Deferred path only)."),
        OLO_PP_NUM(SSRMaxDistance, "ssr", FieldType::Float, 0.1, 10000.0, "Max ray length before the ray is abandoned."),
        OLO_PP_NUM(SSRThickness, "ssr", FieldType::Float, 0.001, 1000.0, "View-space depth tolerance for accepting a hit."),
        OLO_PP_NUM(SSRStride, "ssr", FieldType::Float, 0.001, 100.0, "Initial view-space marching step length."),
        OLO_PP_NUM(SSRMaxSteps, "ssr", FieldType::Int, 1.0, static_cast<f64>(kSSRMaxSteps), "Maximum linear march iterations."),
        OLO_PP_NUM(SSRBinarySearchSteps, "ssr", FieldType::Int, 0.0, static_cast<f64>(kSSRMaxBinarySearchSteps),
                   "Binary-search refinement iterations after a crossing is found."),
        OLO_PP_NUM(SSRIntensity, "ssr", FieldType::Float, 0.0, 16.0, "Overall reflection strength multiplier."),
        OLO_PP_NUM(SSRMaxRoughness, "ssr", FieldType::Float, 0.0, 1.0, "Surfaces rougher than this receive no SSR."),
        OLO_PP_NUM(SSREdgeFade, "ssr", FieldType::Float, 0.0, 0.5, "Screen-border fade width in UV."),
        OLO_PP_BOOL(SSRDebugView, "ssr", "Show the resolved reflection delta instead of the composite."),
        OLO_PP_BOOL(SSRTemporalResolve, "ssr", "Accumulate the reflection signal over frames (issue #902)."),
        OLO_PP_NUM(SSRTemporalFeedback, "ssr", FieldType::Float, 0.0, 0.98, "History weight per frame for the SSR temporal resolve."),

        // ---- screen-space global illumination ----------------------------------
        OLO_PP_BOOL(SSGIEnabled, "ssgi", "Run screen-space indirect diffuse (Deferred path only)."),
        OLO_PP_NUM(SSGIIntensity, "ssgi", FieldType::Float, 0.0, 16.0, "Indirect-diffuse strength multiplier."),
        OLO_PP_NUM(SSGIMaxDistance, "ssgi", FieldType::Float, 0.1, 10000.0, "Max ray length (GI is local — keep short)."),
        OLO_PP_NUM(SSGIThickness, "ssgi", FieldType::Float, 0.001, 1000.0, "View-space depth tolerance for accepting a hit."),
        OLO_PP_NUM(SSGIStride, "ssgi", FieldType::Float, 0.001, 100.0, "Initial view-space marching step length."),
        OLO_PP_NUM(SSGIMaxSteps, "ssgi", FieldType::Int, 1.0, static_cast<f64>(kSSGIMaxSteps), "Maximum linear march iterations per ray."),
        OLO_PP_NUM(SSGIRayCount, "ssgi", FieldType::Int, 1.0, static_cast<f64>(kSSGIMaxRays), "Cosine-weighted hemisphere rays per pixel."),
        OLO_PP_NUM(SSGIEdgeFade, "ssgi", FieldType::Float, 0.0, 0.5, "Screen-border fade width in UV."),
        OLO_PP_BOOL(SSGIDebugView, "ssgi", "Show the indirect-diffuse buffer in isolation."),
        OLO_PP_BOOL(SSGITemporalResolve, "ssgi", "Accumulate the indirect-diffuse signal over frames (issue #902)."),
        OLO_PP_NUM(SSGITemporalFeedback, "ssgi", FieldType::Float, 0.0, 0.98, "History weight per frame for the SSGI temporal resolve."),

        // ---- screen-space contact shadows --------------------------------------
        OLO_PP_BOOL(ContactShadowEnabled, "contactshadow", "Run screen-space contact shadows (Deferred path only)."),
        OLO_PP_NUM(ContactShadowIntensity, "contactshadow", FieldType::Float, 0.0, 1.0, "Darkening at full occlusion (1 = black)."),
        OLO_PP_NUM(ContactShadowMaxDistance, "contactshadow", FieldType::Float, 0.01, 1000.0, "Max ray length toward the light."),
        OLO_PP_NUM(ContactShadowThickness, "contactshadow", FieldType::Float, 0.001, 1000.0, "View-space depth tolerance for an occluder."),
        OLO_PP_NUM(ContactShadowStride, "contactshadow", FieldType::Float, 0.001, 100.0, "View-space marching step length."),
        OLO_PP_NUM(ContactShadowMaxSteps, "contactshadow", FieldType::Int, 1.0, static_cast<f64>(kContactShadowMaxSteps),
                   "Maximum linear march iterations along the ray."),
        OLO_PP_NUM(ContactShadowBias, "contactshadow", FieldType::Float, 0.0, 10.0, "Depth-proportional start offset along the normal."),
        OLO_PP_NUM(ContactShadowEdgeFade, "contactshadow", FieldType::Float, 0.0, 0.5, "Screen-border fade width in UV."),
        OLO_PP_BOOL(ContactShadowDebugView, "contactshadow", "Show the shadow factor as greyscale."),

        // ---- overdraw ------------------------------------------------------------
        OLO_PP_BOOL(OverdrawDebugView, "debug", "Replace the viewport with the per-pixel overdraw heat map."),

        // ---- fog (FogSettings, Renderer3D::GetFogSettings) -----------------------
        OLO_FOG_BOOL("FogEnabled", Enabled, "Master switch for distance/height fog."),
        OLO_FOG_ENUM("FogMode", Mode, kFogModeValues, "Distance falloff curve."),
        OLO_FOG_VEC3("FogColor", Color, "Base fog colour, linear RGB in [0,1]."),
        OLO_FOG_NUM("FogDensity", Density, FieldType::Float, 0.0, 10.0, "Density for the exponential modes."),
        OLO_FOG_NUM("FogStart", Start, FieldType::Float, 0.0, 100000.0, "Linear-mode near distance."),
        OLO_FOG_NUM("FogEnd", End, FieldType::Float, 0.0, 100000.0, "Linear-mode far distance."),
        OLO_FOG_NUM("FogHeightFalloff", HeightFalloff, FieldType::Float, 0.0, 10.0, "Density decay rate above the reference height."),
        OLO_FOG_NUM("FogHeightOffset", HeightOffset, FieldType::Float, -100000.0, 100000.0, "Sea-level reference for height fog."),
        OLO_FOG_NUM("FogMaxOpacity", MaxOpacity, FieldType::Float, 0.0, 1.0, "Clamp on the fog factor at distance."),
        OLO_FOG_BOOL("FogEnableScattering", EnableScattering, "Rayleigh/Mie atmospheric scattering."),
        OLO_FOG_NUM("FogRayleighStrength", RayleighStrength, FieldType::Float, 0.0, 64.0, "Rayleigh in-scatter strength."),
        OLO_FOG_NUM("FogMieStrength", MieStrength, FieldType::Float, 0.0, 64.0, "Mie in-scatter strength."),
        OLO_FOG_NUM("FogMieDirectionality", MieDirectionality, FieldType::Float, -0.99, 0.99,
                    "Henyey-Greenstein g (0 = isotropic, ~1 = full forward scatter)."),
        OLO_FOG_VEC3("FogRayleighColor", RayleighColor, "Rayleigh scattering tint, linear RGB in [0,1]."),
        OLO_FOG_NUM("FogSunIntensity", SunIntensity, FieldType::Float, 0.0, 1000.0, "Sun radiance feeding the scattering terms."),
        OLO_FOG_BOOL("FogEnableVolumetric", EnableVolumetric, "Ray-marched volumetric fog."),
        OLO_FOG_NUM("FogVolumetricSamples", VolumetricSamples, FieldType::Int, 1.0, 256.0, "Volumetric ray-march step count."),
        OLO_FOG_NUM("FogAbsorptionCoefficient", AbsorptionCoefficient, FieldType::Float, 0.0, 10.0, "Beer-Lambert absorption."),
        OLO_FOG_BOOL("FogEnableNoise", EnableNoise, "Modulate fog density with animated 3D noise."),
        OLO_FOG_NUM("FogNoiseScale", NoiseScale, FieldType::Float, 0.0, 10.0, "Noise spatial frequency (world-space)."),
        OLO_FOG_NUM("FogNoiseSpeed", NoiseSpeed, FieldType::Float, 0.0, 100.0, "Noise animation speed (units/sec)."),
        OLO_FOG_NUM("FogNoiseIntensity", NoiseIntensity, FieldType::Float, 0.0, 1.0, "Noise modulation strength."),
        OLO_FOG_BOOL("FogEnableLightShafts", EnableLightShafts, "Volumetric light shafts through the shadow map."),
        OLO_FOG_NUM("FogLightShaftIntensity", LightShaftIntensity, FieldType::Float, 0.0, 64.0, "In-scattering boost for lit volume samples."),
    };

#undef OLO_PP_BOOL
#undef OLO_PP_NUM
#undef OLO_PP_ENUM
#undef OLO_FOG_BOOL
#undef OLO_FOG_NUM
#undef OLO_FOG_ENUM
#undef OLO_FOG_VEC3

    // Lowercase + drop every non-alphanumeric character, so "GTAORadius",
    // "gtao_radius" and "gtao.radius" all collapse to one key. Mirrors
    // RendererSettings::Normalize / RenderOverrides::Normalize.
    [[nodiscard]] inline std::string Normalize(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
        {
            const auto uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) != 0)
                out.push_back(static_cast<char>(std::tolower(uc)));
        }
        return out;
    }

    // Resolve a field token (case / separator insensitive). Returns nullptr for an
    // unknown token.
    [[nodiscard]] inline const FieldInfo* FindField(std::string_view token)
    {
        const std::string key = Normalize(token);
        if (key.empty())
            return nullptr;
        for (const auto& field : kFields)
        {
            if (Normalize(field.Token) == key)
                return &field;
        }
        return nullptr;
    }

    // ", "-joined token list for error text. Long (100+ fields), so callers that
    // want a short message pass a group filter.
    [[nodiscard]] inline std::string JoinFieldTokens(std::string_view group = {})
    {
        std::string out;
        for (const auto& field : kFields)
        {
            if (!group.empty() && field.Group != group)
                continue;
            if (!out.empty())
                out += ", ";
            out += std::string(field.Token);
        }
        return out;
    }

    // The group catalogue, in first-appearance order (the table is authored in
    // panel order, so this reads like the Post Processing panel). Dedup is an
    // exact compare against the groups already emitted, NOT a substring search of
    // the joined string — the latter silently swallows any group that happens to
    // be a substring of an earlier one.
    [[nodiscard]] inline std::vector<std::string_view> GroupTokens()
    {
        std::vector<std::string_view> groups;
        for (const auto& field : kFields)
        {
            if (std::find(groups.begin(), groups.end(), field.Group) == groups.end())
                groups.push_back(field.Group);
        }
        return groups;
    }

    [[nodiscard]] inline std::string JoinGroupTokens()
    {
        std::string out;
        for (const std::string_view group : GroupTokens())
        {
            if (!out.empty())
                out += ", ";
            out += std::string(group);
        }
        return out;
    }

    [[nodiscard]] inline std::string JoinEnumTokens(const FieldInfo& field)
    {
        std::string out;
        for (const auto& v : field.Values)
        {
            if (!out.empty())
                out += ", ";
            out += std::string(v.Token);
        }
        return out;
    }

    // Up to `limit` known tokens whose normalized form contains (or is contained
    // by) the normalized input — a "did you mean" list, since the full catalogue is
    // far too long to dump in an error string.
    [[nodiscard]] inline std::vector<std::string> SuggestFields(std::string_view token, sizet limit = 8)
    {
        std::vector<std::string> out;
        const std::string key = Normalize(token);
        if (key.empty())
            return out;
        for (const auto& field : kFields)
        {
            const std::string candidate = Normalize(field.Token);
            if (candidate.find(key) != std::string::npos || key.find(candidate) != std::string::npos)
            {
                out.emplace_back(field.Token);
                if (out.size() >= limit)
                    break;
            }
        }
        return out;
    }

    // The canonical JSON representation of a field's current value: a bool for
    // Bool, a token string for Enum, a 3-array for Vec3, an integer for Int, a
    // number for Float.
    [[nodiscard]] inline Json ValueJson(const FieldInfo& field, const PostProcessSettings& pp, const FogSettings& fog)
    {
        switch (field.Type)
        {
            case FieldType::Bool:
                return field.Get(pp, fog) != 0.0;
            case FieldType::Int:
                return static_cast<i64>(std::llround(field.Get(pp, fog)));
            case FieldType::Enum:
            {
                const auto raw = static_cast<i32>(std::llround(field.Get(pp, fog)));
                for (const auto& v : field.Values)
                {
                    if (v.Value == raw)
                        return std::string(v.Token);
                }
                return std::string("unknown");
            }
            case FieldType::Vec3:
            {
                const glm::vec3 v = field.GetV3(pp, fog);
                return Json::array({ v.x, v.y, v.z });
            }
            case FieldType::Float:
                break;
        }
        return field.Get(pp, fog);
    }

    // Outcome of a settings write. `RequiresRendererApply` is true only when the
    // written field needs the render-graph topology rebuilt
    // (Renderer3D::ApplyRendererSettings) — that renderer-bound call stays in the
    // McpTools.cpp handler, exactly like RendererSettings::ApplyResult::PathChanged.
    struct ApplyResult
    {
        bool Ok = false;
        std::string Error;
        Json Data;
        bool RequiresRendererApply = false;
        bool RequiresRenderGraphRebuild = false;
    };

    // These fields change the resources or pass declarations produced by the
    // already-registered graph nodes. The live renderer fingerprints them too,
    // but MCP writes explicitly invalidate both caches so a successful setter
    // can never be reported while an old graph remains in use.
    [[nodiscard]] inline bool IsRenderGraphTopologyField(std::string_view token)
    {
        constexpr std::array kFields = {
            std::string_view{ "ActiveAOTechnique" },
            std::string_view{ "SSAOEnabled" },
            std::string_view{ "GTAOEnabled" },
            std::string_view{ "VRCSEnabled" },
            std::string_view{ "VRCSGTAO" },
            std::string_view{ "BloomEnabled" },
            std::string_view{ "VignetteEnabled" },
            std::string_view{ "ChromaticAberrationEnabled" },
            std::string_view{ "FXAAEnabled" },
            std::string_view{ "ColorGradingEnabled" },
            std::string_view{ "DOFEnabled" },
            std::string_view{ "MotionBlurEnabled" },
            std::string_view{ "TAAEnabled" },
            std::string_view{ "CASEnabled" },
            std::string_view{ "SSREnabled" },
            std::string_view{ "SSGIEnabled" },
            std::string_view{ "ContactShadowEnabled" },
            std::string_view{ "OverdrawDebugView" },
            std::string_view{ "FogEnabled" },
            std::string_view{ "FogEnableVolumetric" },
        };
        return std::ranges::find(kFields, token) != kFields.end();
    }

    // Coerce and apply one `value` onto `field`, mutating the live PODs and
    // reporting the prior value so the change can be restored by setting it back.
    // Numeric values CLAMP to the field's declared range (reported via `clamped`);
    // a non-finite float, a wrong JSON type, or an unknown enum token is an error
    // that leaves both structs untouched.
    [[nodiscard]] inline ApplyResult Apply(const FieldInfo& field, const Json& value,
                                           PostProcessSettings& pp, FogSettings& fog)
    {
        ApplyResult result;
        const Json previous = ValueJson(field, pp, fog);
        bool clamped = false;

        switch (field.Type)
        {
            case FieldType::Bool:
            {
                if (!value.is_boolean())
                {
                    result.Error = "Field '" + std::string(field.Token) + "' is a boolean; got " + std::string(value.type_name()) + ".";
                    return result;
                }
                field.Set(pp, fog, value.get<bool>() ? 1.0 : 0.0);
                break;
            }
            case FieldType::Enum:
            {
                if (!value.is_string())
                {
                    result.Error = "Field '" + std::string(field.Token) + "' takes a value token (" + JoinEnumTokens(field) +
                                   "); got " + std::string(value.type_name()) + ".";
                    return result;
                }
                const std::string token = value.get<std::string>();
                const std::string key = Normalize(token);
                const EnumValue* match = nullptr;
                for (const auto& v : field.Values)
                {
                    if (Normalize(v.Token) == key)
                    {
                        match = &v;
                        break;
                    }
                }
                if (match == nullptr)
                {
                    result.Error = "Invalid value '" + token + "' for '" + std::string(field.Token) +
                                   "'. Valid values: " + JoinEnumTokens(field) + ".";
                    return result;
                }
                field.Set(pp, fog, static_cast<f64>(match->Value));
                break;
            }
            case FieldType::Vec3:
            {
                if (!value.is_array() || value.size() != 3)
                {
                    result.Error = "Field '" + std::string(field.Token) + "' is a 3-number array [r, g, b]; got " +
                                   std::string(value.type_name()) + ".";
                    return result;
                }
                glm::vec3 v{};
                for (int i = 0; i < 3; ++i)
                {
                    if (!value[static_cast<sizet>(i)].is_number())
                    {
                        result.Error = "Field '" + std::string(field.Token) + "': element " + std::to_string(i) + " is not a number.";
                        return result;
                    }
                    const f64 raw = value[static_cast<sizet>(i)].get<f64>();
                    if (!std::isfinite(raw))
                    {
                        result.Error = "Field '" + std::string(field.Token) + "': element " + std::to_string(i) + " must be finite.";
                        return result;
                    }
                    const f64 ranged = std::clamp(raw, field.Min, field.Max);
                    clamped = clamped || ranged != raw;
                    v[i] = static_cast<f32>(ranged);
                }
                field.SetV3(pp, fog, v);
                break;
            }
            case FieldType::Int:
            case FieldType::Float:
            {
                if (!value.is_number() || value.is_boolean())
                {
                    result.Error = "Field '" + std::string(field.Token) + "' is a number; got " + std::string(value.type_name()) + ".";
                    return result;
                }
                const f64 raw = value.get<f64>();
                if (!std::isfinite(raw))
                {
                    result.Error = "Field '" + std::string(field.Token) + "' must be finite (got NaN/Inf).";
                    return result;
                }
                const f64 rounded = field.Type == FieldType::Int ? std::round(raw) : raw;
                const f64 ranged = std::clamp(rounded, field.Min, field.Max);
                // Compare against `rounded`, not `raw`: rounding 4.6 to 5 on an
                // Int field is not a range clamp, and reporting it as one (plus
                // the `range` block) tells the caller their value was rejected
                // when it was honoured.
                clamped = ranged != rounded;
                field.Set(pp, fog, ranged);
                break;
            }
        }

        const Json applied = ValueJson(field, pp, fog);
        result.Ok = true;
        result.RequiresRendererApply = field.RequiresRendererApply && applied != previous;
        result.RequiresRenderGraphRebuild = IsRenderGraphTopologyField(field.Token) && applied != previous;
        result.Data = Json{
            { "field", std::string(field.Token) },
            { "group", std::string(field.Group) },
            { "previousValue", previous },
            { "value", applied },
            { "changed", applied != previous },
            { "clamped", clamped },
            // Restore hint: session-global settings, so a revert is this same tool
            // with `value` = previousValue (no CommandHistory / Ctrl-Z entry).
            { "restoreWith", previous },
        };
        if (clamped)
            result.Data["range"] = Json{ { "min", field.Min }, { "max", field.Max } };
        return result;
    }

    // One field's introspection record: token, group, type, range / enum values,
    // live current value, description.
    [[nodiscard]] inline Json DescribeField(const FieldInfo& field, const PostProcessSettings& pp, const FogSettings& fog)
    {
        const char* typeName = "number";
        switch (field.Type)
        {
            case FieldType::Bool:
                typeName = "boolean";
                break;
            case FieldType::Int:
                typeName = "integer";
                break;
            case FieldType::Enum:
                typeName = "enum";
                break;
            case FieldType::Vec3:
                typeName = "vec3";
                break;
            case FieldType::Float:
                break;
        }

        Json j{
            { "field", std::string(field.Token) },
            { "group", std::string(field.Group) },
            { "type", typeName },
            { "value", ValueJson(field, pp, fog) },
            { "description", std::string(field.Description) },
        };
        if (field.Type == FieldType::Enum)
        {
            Json values = Json::array();
            for (const auto& v : field.Values)
                values.push_back(Json{ { "token", std::string(v.Token) }, { "description", std::string(v.Description) } });
            j["values"] = std::move(values);
        }
        else if (field.Type != FieldType::Bool)
        {
            j["min"] = field.Min;
            j["max"] = field.Max;
        }
        if (IsRenderGraphTopologyField(field.Token))
            j["rebuildsRenderGraph"] = true;
        return j;
    }

    // Introspection payload. `group` (normalized) filters to one group; an empty
    // group lists everything. `unknownGroup` is set when a non-empty filter matched
    // nothing, so the caller can turn it into an error listing the real groups.
    [[nodiscard]] inline Json Describe(const PostProcessSettings& pp, const FogSettings& fog,
                                       std::string_view group, bool& unknownGroup)
    {
        const std::string key = Normalize(group);
        unknownGroup = false;

        Json fields = Json::array();
        for (const auto& field : kFields)
        {
            if (!key.empty() && Normalize(field.Group) != key)
                continue;
            fields.push_back(DescribeField(field, pp, fog));
        }
        if (!key.empty() && fields.empty())
            unknownGroup = true;

        Json groups = Json::array();
        for (const std::string_view groupToken : GroupTokens())
            groups.push_back(std::string(groupToken));

        return Json{
            { "fields", std::move(fields) },
            { "groups", std::move(groups) },
            { "note", "Tone-map operator, FSR1 upscale preset, rendering path, depth prepass and soft-shadow "
                      "filtering live on olo_renderer_settings_set, not here — one write path per field." },
        };
    }

    // ---- tool schemas ---------------------------------------------------------

    [[nodiscard]] inline Json GetInputSchema()
    {
        return Schema::Object()
            .Prop("field", Schema::String().Desc("One field token (the C++ field name, e.g. 'GTAORadius', 'ActiveAOTechnique', 'FogDensity'); "
                                                 "case- and separator-insensitive. Omit to list every field."))
            .Prop("group", Schema::String().Desc("Restrict the listing to one group (ao, bloom, ssr, ssgi, fog, exposure, dof, ...). "
                                                 "Ignored when 'field' is given."))
            .NoAdditional();
    }

    [[nodiscard]] inline Json SetInputSchema()
    {
        return Schema::Object()
            .Prop("field", Schema::String().Desc("Field token to write (the C++ field name, e.g. 'ActiveAOTechnique', 'GTAORadius', "
                                                 "'FogDensity'); case- and separator-insensitive. Omit to list every field with its "
                                                 "current value (the same payload olo_postprocess_settings_get returns)."))
            .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } })
                               .Desc("New value: boolean for a flag, number for a scalar (clamped to the field's declared range), "
                                     "a value token for an enum field (ActiveAOTechnique: none|ssao|gtao; FogMode: "
                                     "linear|exponential|exponentialsquared), or a 3-number array for a colour."))
            .NoAdditional();
    }

    // Parse the set-tool arguments. `isIntrospect` true means "no field given —
    // list everything". The server validates against SetInputSchema() first, but
    // this stays defensive (tests reach it directly). Returns an error, or nullopt.
    [[nodiscard]] inline std::optional<std::string> ParseSetArgs(const Json& args, bool& isIntrospect,
                                                                 const FieldInfo*& field, Json& value)
    {
        const bool hasField = args.contains("field") && !args["field"].is_null();
        if (!hasField)
        {
            isIntrospect = true;
            if (args.contains("value") && !args["value"].is_null())
                return "Provide 'field' when providing 'value' (omit both to list every field).";
            return std::nullopt;
        }

        isIntrospect = false;
        if (!args["field"].is_string())
            return "Invalid 'field': expected a string.";
        const std::string token = args["field"].get<std::string>();
        field = FindField(token);
        if (field == nullptr)
        {
            std::string message = "Unknown post-process field '" + token + "'.";
            if (const std::vector<std::string> suggestions = SuggestFields(token); !suggestions.empty())
            {
                message += " Did you mean: ";
                for (sizet i = 0; i < suggestions.size(); ++i)
                    message += (i == 0 ? "" : ", ") + suggestions[i];
                message += "?";
            }
            message += " Call with no arguments (or olo_postprocess_settings_get) to list every field; groups: " +
                       JoinGroupTokens() + ".";
            return message;
        }

        if (!args.contains("value") || args["value"].is_null())
        {
            // Name the shape the caller must actually send. A Vec3 falling through
            // to "number" tells them to send the one thing Apply will reject.
            std::string expected;
            switch (field->Type)
            {
                case FieldType::Enum:
                    expected = "one of: " + JoinEnumTokens(*field);
                    break;
                case FieldType::Bool:
                    expected = "type boolean";
                    break;
                case FieldType::Vec3:
                    expected = "a 3-number array [x, y, z]";
                    break;
                case FieldType::Int:
                case FieldType::Float:
                    expected = "type number";
                    break;
            }
            return "Missing required argument 'value' for field '" + std::string(field->Token) + "' (" + expected + ").";
        }
        value = args["value"];
        return std::nullopt;
    }
} // namespace OloEngine::MCP::PostProcess
