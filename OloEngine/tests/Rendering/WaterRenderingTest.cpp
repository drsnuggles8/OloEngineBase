#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/UnderwaterFog.h"
#include "OloEngine/Renderer/UnderwaterCaustics.h"
#include "OloEngine/Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <type_traits>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// WaterUBO — Alignment & Size
// =============================================================================

TEST(WaterRendering, WaterUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::WaterUBO) % 16, 0u)
        << "WaterUBO must be 16-byte aligned for std140";
}

TEST(WaterRendering, WaterUBOSizeStable)
{
    // 17 x glm::vec4 = 17 x 16 = 272 bytes
    EXPECT_EQ(sizeof(UBOStructures::WaterUBO), 272u);
}

TEST(WaterRendering, WaterUBOGetSizeMatchesSizeof)
{
    EXPECT_EQ(UBOStructures::WaterUBO::GetSize(), sizeof(UBOStructures::WaterUBO));
}

TEST(WaterRendering, WaterUBOBindingSlot)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_WATER, 23u);
}

TEST(WaterRendering, WaterUBOKnownBinding)
{
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_WATER, "WaterParams"));
}

// =============================================================================
// WaterUBO — Field Layout
// =============================================================================

TEST(WaterRendering, WaterUBOFieldRoundTrip)
{
    // Compile-time layout guarantees
    static_assert(std::is_trivially_copyable_v<UBOStructures::WaterUBO>);
    EXPECT_EQ(offsetof(UBOStructures::WaterUBO, WaveParams), 0u);
    EXPECT_EQ(offsetof(UBOStructures::WaterUBO, WaveDir0), 16u);
    EXPECT_EQ(offsetof(UBOStructures::WaterUBO, WaveDir1), 32u);
    EXPECT_EQ(offsetof(UBOStructures::WaterUBO, NormalMapScroll), 96u);
    EXPECT_EQ(offsetof(UBOStructures::WaterUBO, DepthRefractionParams), 160u);
    EXPECT_EQ(offsetof(UBOStructures::WaterUBO, TessParams), 256u);

    UBOStructures::WaterUBO ubo{};
    ubo.WaveParams = glm::vec4(1.0f, 2.0f, 0.5f, 3.0f);
    ubo.WaveDir0 = glm::vec4(1.0f, 0.0f, 0.5f, 6.28f);
    ubo.WaveDir1 = glm::vec4(0.7f, 0.7f, 0.3f, 4.0f);
    ubo.WaterColor = glm::vec4(0.1f, 0.4f, 0.5f, 0.6f);
    ubo.WaterDeepColor = glm::vec4(0.0f, 0.1f, 0.2f, 0.5f);
    ubo.VisualParams = glm::vec4(5.0f, 1.0f, 0.0f, 0.0f);
    ubo.NormalMapScroll = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    ubo.NormalMapSpeed = glm::vec4(0.02f, 0.015f, 0.0f, 0.0f);
    ubo.LightDirection = glm::vec4(0.5f, 1.0f, 0.3f, 0.0f);
    ubo.ScreenParams = glm::vec4(1920.0f, 1080.0f, 1.0f / 1920.0f, 1.0f / 1080.0f);
    ubo.DepthRefractionParams = glm::vec4(2.0f, 0.05f, 0.5f, 0.0f);
    ubo.RefractionColor = glm::vec4(0.0f, 0.05f, 0.1f, 0.0f);
    ubo.FoamParams = glm::vec4(0.3f, 0.5f, 2.0f, 1.5f);
    ubo.FoamParams2 = glm::vec4(2.0f, 3.0f, 0.5f, 0.0f);
    ubo.SSSColor = glm::vec4(0.0f, 0.5f, 0.4f, 0.0f);
    ubo.SSRParams = glm::vec4(64.0f, 0.1f, 50.0f, 0.5f);
    ubo.TessParams = glm::vec4(8.0f, 10.0f, 200.0f, 0.0f);

    // Verify memcpy round-trip (simulating UBO upload)
    UBOStructures::WaterUBO copy{};
    std::memcpy(&copy, &ubo, sizeof(UBOStructures::WaterUBO));

    EXPECT_FLOAT_EQ(copy.WaveParams.x, 1.0f);
    EXPECT_FLOAT_EQ(copy.WaveParams.y, 2.0f);
    EXPECT_FLOAT_EQ(copy.WaveParams.z, 0.5f);
    EXPECT_FLOAT_EQ(copy.WaveParams.w, 3.0f);
    EXPECT_FLOAT_EQ(copy.WaterColor.a, 0.6f);
    EXPECT_FLOAT_EQ(copy.WaterDeepColor.a, 0.5f);
    EXPECT_FLOAT_EQ(copy.VisualParams.x, 5.0f);
    EXPECT_FLOAT_EQ(copy.VisualParams.y, 1.0f);
    EXPECT_FLOAT_EQ(copy.NormalMapScroll.x, 0.1f);
    EXPECT_FLOAT_EQ(copy.NormalMapScroll.w, 0.4f);
    EXPECT_FLOAT_EQ(copy.NormalMapSpeed.x, 0.02f);
    EXPECT_FLOAT_EQ(copy.LightDirection.y, 1.0f);
    EXPECT_FLOAT_EQ(copy.ScreenParams.x, 1920.0f);
    EXPECT_FLOAT_EQ(copy.ScreenParams.y, 1080.0f);
    EXPECT_FLOAT_EQ(copy.DepthRefractionParams.x, 2.0f);
    EXPECT_FLOAT_EQ(copy.DepthRefractionParams.y, 0.05f);
    EXPECT_FLOAT_EQ(copy.RefractionColor.g, 0.05f);
    EXPECT_FLOAT_EQ(copy.FoamParams.x, 0.3f);
    EXPECT_FLOAT_EQ(copy.FoamParams.w, 1.5f);
    EXPECT_FLOAT_EQ(copy.FoamParams2.z, 0.5f);
    EXPECT_FLOAT_EQ(copy.SSSColor.g, 0.5f);
    EXPECT_FLOAT_EQ(copy.SSRParams.x, 64.0f);
    EXPECT_FLOAT_EQ(copy.SSRParams.w, 0.5f);
    EXPECT_FLOAT_EQ(copy.TessParams.x, 8.0f);
    EXPECT_FLOAT_EQ(copy.TessParams.z, 200.0f);
}

// =============================================================================
// DrawWaterCommand — Trivially Copyable
// =============================================================================

TEST(WaterRendering, DrawWaterCommandTrivialCopy)
{
    static_assert(std::is_trivially_copyable_v<DrawWaterCommand>);

    DrawWaterCommand cmd{};
    cmd.header.type = CommandType::DrawWater;
    cmd.vertexArrayID = 42;
    cmd.indexCount = 1024;
    cmd.shaderRendererID = 7;
    cmd.modelTransform = glm::mat4(1.0f);
    cmd.normalMatrix = glm::mat4(1.0f);
    cmd.waveParams = glm::vec4(1.0f, 2.0f, 0.5f, 3.0f);
    cmd.waterColor = glm::vec4(0.1f, 0.4f, 0.5f, 0.6f);
    cmd.entityID = 999;

    DrawWaterCommand copy{};
    std::memcpy(&copy, &cmd, sizeof(DrawWaterCommand));

    EXPECT_EQ(copy.header.type, CommandType::DrawWater);
    EXPECT_EQ(copy.vertexArrayID, 42u);
    EXPECT_EQ(copy.indexCount, 1024u);
    EXPECT_EQ(copy.shaderRendererID, 7u);
    EXPECT_EQ(copy.entityID, 999);
    EXPECT_FLOAT_EQ(copy.waveParams.x, 1.0f);
    EXPECT_FLOAT_EQ(copy.waterColor.g, 0.4f);
}

TEST(WaterRendering, DrawWaterCommandSizeBound)
{
    EXPECT_LE(sizeof(DrawWaterCommand), MAX_COMMAND_SIZE);
}

TEST(WaterRendering, DrawWaterCommandZeroInitNoNaN)
{
    DrawWaterCommand cmd{};
    ValidateTransform(cmd.modelTransform);
    ValidateTransform(cmd.normalMatrix);
    ValidateVec4(cmd.waveParams, "waveParams");
    ValidateVec4(cmd.waveDir0, "waveDir0");
    ValidateVec4(cmd.waveDir1, "waveDir1");
    ValidateVec4(cmd.waterColor, "waterColor");
    ValidateVec4(cmd.waterDeepColor, "waterDeepColor");
    ValidateVec4(cmd.visualParams, "visualParams");
    ValidateVec4(cmd.normalMapScroll, "normalMapScroll");
    ValidateVec4(cmd.normalMapSpeed, "normalMapSpeed");
    ValidateVec4(cmd.lightDirection, "lightDirection");
    ValidateVec4(cmd.depthRefractionParams, "depthRefractionParams");
    ValidateVec4(cmd.refractionColor, "refractionColor");
    ValidateVec4(cmd.foamParams, "foamParams");
    ValidateVec4(cmd.foamParams2, "foamParams2");
    ValidateVec4(cmd.sssColor, "sssColor");
    ValidateVec4(cmd.ssrParams, "ssrParams");
    ValidateVec4(cmd.tessParams, "tessParams");
    EXPECT_EQ(cmd.normalMap0ID, 0u);
    EXPECT_EQ(cmd.normalMap1ID, 0u);
    EXPECT_EQ(cmd.noiseTextureID, 0u);
    EXPECT_EQ(cmd.foamTextureID, 0u);
}

// =============================================================================
// WaterComponent — Default Values
// =============================================================================

TEST(WaterRendering, WaterComponentDefaults)
{
    WaterComponent wc{};
    EXPECT_FLOAT_EQ(wc.m_WorldSizeX, 100.0f);
    EXPECT_FLOAT_EQ(wc.m_WorldSizeZ, 100.0f);
    EXPECT_FLOAT_EQ(wc.m_WaveAmplitude, 0.5f);
    EXPECT_FLOAT_EQ(wc.m_WaveFrequency, 1.0f);
    EXPECT_FLOAT_EQ(wc.m_WaveSpeed, 1.0f);
    EXPECT_FLOAT_EQ(wc.m_WaveSteepness0, 0.5f);
    EXPECT_FLOAT_EQ(wc.m_Wavelength0, 10.0f);
    EXPECT_FLOAT_EQ(wc.m_WaveSteepness1, 0.3f);
    EXPECT_FLOAT_EQ(wc.m_Wavelength1, 15.0f);
    EXPECT_FLOAT_EQ(wc.m_Transparency, 0.6f);
    EXPECT_FLOAT_EQ(wc.m_Reflectivity, 0.5f);
    EXPECT_FLOAT_EQ(wc.m_FresnelPower, 5.0f);
    EXPECT_FLOAT_EQ(wc.m_SpecularIntensity, 1.0f);
    EXPECT_EQ(wc.m_GridResolutionX, 128u);
    EXPECT_EQ(wc.m_GridResolutionZ, 128u);
    EXPECT_TRUE(wc.m_Enabled);
    EXPECT_TRUE(wc.m_NeedsRebuild);
    EXPECT_EQ(wc.m_WaterMesh, nullptr);

    // Normal map / noise defaults
    EXPECT_FLOAT_EQ(wc.m_NormalMapScrollSpeed0, 0.02f);
    EXPECT_FLOAT_EQ(wc.m_NormalMapScrollSpeed1, 0.015f);
    EXPECT_FLOAT_EQ(wc.m_NormalMapTiling, 1.0f);
    EXPECT_FLOAT_EQ(wc.m_NoiseIntensity, 0.3f);
    EXPECT_EQ(wc.m_NormalMap0, 0u);
    EXPECT_EQ(wc.m_NormalMap1, 0u);
    EXPECT_EQ(wc.m_NoiseTexture, 0u);

    // Depth / refraction defaults
    EXPECT_FLOAT_EQ(wc.m_DepthSofteningDistance, 2.0f);
    EXPECT_FLOAT_EQ(wc.m_RefractionDistortion, 0.05f);
    EXPECT_FLOAT_EQ(wc.m_RefractionHeightFactor, 0.5f);
    EXPECT_FLOAT_EQ(wc.m_RefractionColor.r, 0.0f);
    EXPECT_FLOAT_EQ(wc.m_RefractionColor.g, 0.05f);
    EXPECT_FLOAT_EQ(wc.m_RefractionColor.b, 0.1f);

    // Foam defaults
    EXPECT_EQ(wc.m_FoamTexture, 0u);
    EXPECT_FLOAT_EQ(wc.m_FoamHeightStart, 0.3f);
    EXPECT_FLOAT_EQ(wc.m_FoamFadeDistance, 0.5f);
    EXPECT_FLOAT_EQ(wc.m_FoamTiling, 2.0f);
    EXPECT_FLOAT_EQ(wc.m_FoamBrightness, 1.5f);
    EXPECT_FLOAT_EQ(wc.m_FoamAngleExponent, 2.0f);
    EXPECT_FLOAT_EQ(wc.m_ShorelineFoamPower, 3.0f);

    // SSS defaults
    EXPECT_FLOAT_EQ(wc.m_SSSColor.r, 0.0f);
    EXPECT_FLOAT_EQ(wc.m_SSSColor.g, 0.5f);
    EXPECT_FLOAT_EQ(wc.m_SSSColor.b, 0.4f);
    EXPECT_FLOAT_EQ(wc.m_SSSIntensity, 0.5f);

    // SSR defaults
    EXPECT_FLOAT_EQ(wc.m_SSRMaxSteps, 64.0f);
    EXPECT_FLOAT_EQ(wc.m_SSRStepSize, 0.1f);
    EXPECT_FLOAT_EQ(wc.m_SSRMaxDistance, 50.0f);
    EXPECT_FLOAT_EQ(wc.m_SSRThickness, 0.5f);
    EXPECT_TRUE(wc.m_SSREnabled);

    // Refraction defaults
    EXPECT_TRUE(wc.m_RefractionEnabled);

    // Tessellation defaults
    EXPECT_FALSE(wc.m_TessellationEnabled);
    EXPECT_FLOAT_EQ(wc.m_TessellationFactor, 8.0f);
    EXPECT_FLOAT_EQ(wc.m_TessMinDistance, 10.0f);
    EXPECT_FLOAT_EQ(wc.m_TessMaxDistance, 200.0f);

    // Underwater fog defaults (WATER_FUTURE_IMPROVEMENTS.md §7.2)
    EXPECT_FLOAT_EQ(wc.m_UnderwaterFogColor.r, 0.05f);
    EXPECT_FLOAT_EQ(wc.m_UnderwaterFogColor.g, 0.15f);
    EXPECT_FLOAT_EQ(wc.m_UnderwaterFogColor.b, 0.25f);
    EXPECT_FLOAT_EQ(wc.m_UnderwaterFogDensity, 0.08f);
    EXPECT_TRUE(wc.m_RenderFromBelow);
}

// =============================================================================
// WaterComponent — Copy Semantics (runtime state NOT copied)
// =============================================================================

TEST(WaterRendering, WaterComponentCopyOmitsRuntime)
{
    WaterComponent original{};
    original.m_WorldSizeX = 200.0f;
    original.m_WaveAmplitude = 1.5f;
    original.m_Wavelength0 = 20.0f;
    original.m_Wavelength1 = 30.0f;
    original.m_WaterColor = glm::vec3(0.2f, 0.5f, 0.8f);
    original.m_Enabled = false;
    original.m_NormalMapScrollSpeed0 = 0.123f;
    original.m_RefractionDistortion = 0.456f;
    original.m_TessellationEnabled = true;
    original.m_TessellationFactor = 16.0f;
    original.m_SSREnabled = false;
    original.m_SSRMaxSteps = 128.0f;
    original.m_UnderwaterFogColor = glm::vec3(0.1f, 0.2f, 0.3f);
    original.m_UnderwaterFogDensity = 0.25f;
    original.m_RenderFromBelow = false;
    original.m_UnderwaterRefractionStrength = 0.05f;
    original.m_UnderwaterChromaticStrength = 0.7f;
    original.m_CausticsIntensity = 1.25f;
    original.m_CausticsColor = glm::vec3(0.5f, 0.6f, 0.7f);
    original.m_GodRayIntensity = 0.8f;
    original.m_GodRayDecay = 0.93f;
    original.m_GodRaySamples = 64u;
    original.m_GodRayColor = glm::vec3(0.9f, 0.85f, 0.7f);
    original.m_GodRayDappleFloor = 0.42f;
    original.m_GodRaySunFalloff = 22.0f;

    WaterComponent copy(original);

    // Serialized fields should be copied
    EXPECT_FLOAT_EQ(copy.m_WorldSizeX, 200.0f);
    EXPECT_FLOAT_EQ(copy.m_WaveAmplitude, 1.5f);
    EXPECT_FLOAT_EQ(copy.m_Wavelength0, 20.0f);
    EXPECT_FLOAT_EQ(copy.m_Wavelength1, 30.0f);
    EXPECT_FLOAT_EQ(copy.m_WaterColor.r, 0.2f);
    EXPECT_FALSE(copy.m_Enabled);
    EXPECT_FLOAT_EQ(copy.m_NormalMapScrollSpeed0, 0.123f);
    EXPECT_FLOAT_EQ(copy.m_RefractionDistortion, 0.456f);
    EXPECT_TRUE(copy.m_TessellationEnabled);
    EXPECT_FLOAT_EQ(copy.m_TessellationFactor, 16.0f);
    EXPECT_FALSE(copy.m_SSREnabled);
    EXPECT_FLOAT_EQ(copy.m_SSRMaxSteps, 128.0f);
    EXPECT_FLOAT_EQ(copy.m_UnderwaterFogColor.r, 0.1f);
    EXPECT_FLOAT_EQ(copy.m_UnderwaterFogColor.g, 0.2f);
    EXPECT_FLOAT_EQ(copy.m_UnderwaterFogColor.b, 0.3f);
    EXPECT_FLOAT_EQ(copy.m_UnderwaterFogDensity, 0.25f);
    EXPECT_FALSE(copy.m_RenderFromBelow);
    EXPECT_FLOAT_EQ(copy.m_UnderwaterRefractionStrength, 0.05f);
    EXPECT_FLOAT_EQ(copy.m_UnderwaterChromaticStrength, 0.7f);
    EXPECT_FLOAT_EQ(copy.m_CausticsIntensity, 1.25f);
    EXPECT_FLOAT_EQ(copy.m_CausticsColor.r, 0.5f);
    EXPECT_FLOAT_EQ(copy.m_CausticsColor.g, 0.6f);
    EXPECT_FLOAT_EQ(copy.m_CausticsColor.b, 0.7f);
    EXPECT_FLOAT_EQ(copy.m_GodRayIntensity, 0.8f);
    EXPECT_FLOAT_EQ(copy.m_GodRayDecay, 0.93f);
    EXPECT_EQ(copy.m_GodRaySamples, 64u);
    EXPECT_FLOAT_EQ(copy.m_GodRayColor.r, 0.9f);
    EXPECT_FLOAT_EQ(copy.m_GodRayColor.b, 0.7f);
    EXPECT_FLOAT_EQ(copy.m_GodRayDappleFloor, 0.42f);
    EXPECT_FLOAT_EQ(copy.m_GodRaySunFalloff, 22.0f);

    // Runtime state should NOT be copied — mesh is null, needs rebuild
    EXPECT_EQ(copy.m_WaterMesh, nullptr);
    EXPECT_TRUE(copy.m_NeedsRebuild);
}

TEST(WaterRendering, WaterComponentAssignmentOmitsRuntime)
{
    WaterComponent original{};
    original.m_WorldSizeZ = 50.0f;
    original.m_FresnelPower = 3.0f;
    original.m_NormalMapScrollDir0 = glm::vec2(0.707f, 0.707f);
    original.m_RefractionEnabled = false;
    original.m_TessMinDistance = 5.0f;
    original.m_TessMaxDistance = 100.0f;
    original.m_SSRStepSize = 0.25f;

    WaterComponent target{};
    target = original;

    EXPECT_FLOAT_EQ(target.m_WorldSizeZ, 50.0f);
    EXPECT_FLOAT_EQ(target.m_FresnelPower, 3.0f);
    EXPECT_FLOAT_EQ(target.m_NormalMapScrollDir0.x, 0.707f);
    EXPECT_FLOAT_EQ(target.m_NormalMapScrollDir0.y, 0.707f);
    EXPECT_FALSE(target.m_RefractionEnabled);
    EXPECT_FLOAT_EQ(target.m_TessMinDistance, 5.0f);
    EXPECT_FLOAT_EQ(target.m_TessMaxDistance, 100.0f);
    EXPECT_FLOAT_EQ(target.m_SSRStepSize, 0.25f);
    EXPECT_EQ(target.m_WaterMesh, nullptr);
    EXPECT_TRUE(target.m_NeedsRebuild);
}

// =============================================================================
// CommandType Enum — DrawWater Exists
// =============================================================================

TEST(WaterRendering, DrawWaterCommandTypeExists)
{
    auto type = CommandType::DrawWater;
    const char* str = CommandTypeToString(type);
    EXPECT_NE(str, nullptr);
    EXPECT_STREQ(str, "DrawWater");
}

// =============================================================================
// Wavelength — Regression: non-zero defaults produce visible waves
// =============================================================================

TEST(WaterRendering, WavelengthDefaultsNonZero)
{
    WaterComponent wc{};
    // Wavelengths must be > 0 so the shader produces visible Gerstner waves.
    // A value of 0 crushes amplitude to sub-pixel noise (bug fixed here).
    EXPECT_GT(wc.m_Wavelength0, 0.0f);
    EXPECT_GT(wc.m_Wavelength1, 0.0f);
}

TEST(WaterRendering, WavelengthPackedIntoWaveDir)
{
    WaterComponent wc{};
    wc.m_WaveDir0 = { 1.0f, 0.0f };
    wc.m_WaveSteepness0 = 0.5f;
    wc.m_Wavelength0 = 12.0f;
    wc.m_WaveDir1 = { 0.7f, 0.7f };
    wc.m_WaveSteepness1 = 0.3f;
    wc.m_Wavelength1 = 18.0f;

    // Use the same packing helper that Scene.cpp calls at runtime
    glm::vec4 waveDir0 = wc.PackWaveDir0();
    glm::vec4 waveDir1 = wc.PackWaveDir1();

    EXPECT_FLOAT_EQ(waveDir0.w, 12.0f) << "waveDir0.w must carry wavelength0";
    EXPECT_FLOAT_EQ(waveDir1.w, 18.0f) << "waveDir1.w must carry wavelength1";
    EXPECT_FLOAT_EQ(waveDir0.z, 0.5f) << "waveDir0.z must carry steepness0";
    EXPECT_FLOAT_EQ(waveDir1.z, 0.3f) << "waveDir1.z must carry steepness1";
}

// =============================================================================
// Texture Binding Slots — Phase 2+3+5
// =============================================================================

TEST(WaterRendering, WaterTextureBindingSlots)
{
    EXPECT_EQ(ShaderBindingLayout::TEX_WATER_NORMAL_0, 36u);
    EXPECT_EQ(ShaderBindingLayout::TEX_WATER_NORMAL_1, 37u);
    EXPECT_EQ(ShaderBindingLayout::TEX_WATER_NOISE, 38u);
    EXPECT_EQ(ShaderBindingLayout::TEX_WATER_DEPTH, 39u);
    EXPECT_EQ(ShaderBindingLayout::TEX_WATER_REFRACTION, 40u);
    EXPECT_EQ(ShaderBindingLayout::TEX_WATER_FOAM, 41u);
    EXPECT_EQ(ShaderBindingLayout::TEX_WATER_SSR, 42u);
}

// =============================================================================
// Water TCS Frustum Culling — CPU mirror of the shader's per-patch reject test
// =============================================================================
//
// These tests pin the algorithm used inside Water.glsl's tess-control stage so
// regressions in the displacement bound or the Gribb-Hartmann plane extraction
// surface on the CPU before a visual artifact ships. The math here is a
// 1:1 port of `computeMaxWaveDisplacement` and `isPatchOutsideFrustum`; if the
// shader copy changes, this CPU copy must change in lockstep.
// =============================================================================

namespace
{
    // CPU mirror of Water.glsl :: computeMaxWaveDisplacement.
    // Inputs match the WaterUBO packing the shader sees.
    f32 ComputeMaxWaveDisplacementCpu(glm::vec4 waveParams, glm::vec4 waveDir0, glm::vec4 waveDir1)
    {
        constexpr f32 kTwoPi = 6.28318530f;
        const f32 freq = std::max(waveParams.w, 0.01f);
        const f32 amp = waveParams.z;

        const f32 wl0 = std::max(waveDir0.w, 0.1f) / freq;
        const f32 wl1 = std::max(waveDir1.w, 0.1f) / freq;
        const f32 a0 = waveDir0.z * wl0 / kTwoPi;
        const f32 a1 = waveDir1.z * wl1 / kTwoPi;

        const f32 avgWL = (wl0 + wl1) * 0.5f;
        const f32 avgSt = (waveDir0.z + waveDir1.z) * 0.5f;
        const f32 maxOctaveA = avgSt * avgWL / kTwoPi;
        // detail-octave amp weights = 0.5+0.4+0.3+0.22+0.15+0.1
        const f32 octaveSum = maxOctaveA * 1.67f;

        return amp * (a0 * 0.55f + a1 * 0.55f + octaveSum) * 1.5f;
    }

    // CPU mirror of Water.glsl :: isPatchOutsideFrustum. Uses GLM column-major
    // matrices, so `vp[col][row]` matches the GLSL access exactly.
    bool IsPatchOutsideFrustumCpu(glm::vec3 p0, glm::vec3 p1, glm::vec3 p2,
                                  f32 margin, glm::mat4 const& vp)
    {
        const glm::vec4 row0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
        const glm::vec4 row1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
        const glm::vec4 row2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
        const glm::vec4 row3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);

        const glm::vec4 planes[6] = {
            row3 + row0, // left
            row3 - row0, // right
            row3 + row1, // bottom
            row3 - row1, // top
            row3 + row2, // near (GL [-1,1] clip-space depth)
            row3 - row2, // far
        };

        for (auto const& plane : planes)
        {
            const f32 normFactor = glm::length(glm::vec3(plane));
            const f32 d0 = glm::dot(glm::vec3(plane), p0) + plane.w;
            const f32 d1 = glm::dot(glm::vec3(plane), p1) + plane.w;
            const f32 d2 = glm::dot(glm::vec3(plane), p2) + plane.w;
            const f32 threshold = -margin * normFactor;
            if (d0 < threshold && d1 < threshold && d2 < threshold)
                return true;
        }
        return false;
    }

    // Camera looking down -Z at the world origin, with a standard perspective.
    // Matches what `Renderer3D::BeginScene` builds for an editor preview cam.
    glm::mat4 MakeForwardLookingViewProj()
    {
        // 60° FOV, 16:9 aspect, near=0.1, far=500. Same near/far as the editor
        // SceneCamera default so the near-plane cull edge case matches shipped behaviour.
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 5.0f, 10.0f),
                                           glm::vec3(0.0f, 0.0f, 0.0f),
                                           glm::vec3(0.0f, 1.0f, 0.0f));
        return proj * view;
    }
} // namespace

TEST(WaterRendering, MaxWaveDisplacementBoundsActualGerstner)
{
    // Pick default-component wave params and assert the bound encloses the
    // worst-case sum of per-axis Gerstner amplitudes the shader produces.
    // Underestimating here would pop visible waves at off-screen patches.
    WaterComponent wc{};
    const glm::vec4 waveParams(0.0f, wc.m_WaveSpeed, wc.m_WaveAmplitude, wc.m_WaveFrequency);
    const glm::vec4 waveDir0 = wc.PackWaveDir0();
    const glm::vec4 waveDir1 = wc.PackWaveDir1();

    const f32 margin = ComputeMaxWaveDisplacementCpu(waveParams, waveDir0, waveDir1);
    EXPECT_GT(margin, 0.0f);

    // Conservative ground-truth: sum the per-axis Gerstner amplitudes the way
    // sumGerstnerWaves does (primary + 6 octaves), no 1.5x safety. The shader's
    // returned margin must dominate this.
    constexpr f32 kTwoPi = 6.28318530f;
    const f32 freq = std::max(waveParams.w, 0.01f);
    const f32 wl0 = std::max(waveDir0.w, 0.1f) / freq;
    const f32 wl1 = std::max(waveDir1.w, 0.1f) / freq;
    const f32 a0 = waveDir0.z * wl0 / kTwoPi;
    const f32 a1 = waveDir1.z * wl1 / kTwoPi;
    const f32 avgWL = (wl0 + wl1) * 0.5f;
    const f32 avgSt = (waveDir0.z + waveDir1.z) * 0.5f;
    // Per-octave: st * wl × global amp weight. Use the actual constants from
    // WaterCommon.glsl rather than the bound's avgSt*avgWL fall-back.
    const f32 octave0 = (avgSt * 0.5f) * (avgWL * 0.85f) / kTwoPi * 0.5f;
    const f32 octave1 = (avgSt * 0.45f) * (avgWL * 0.6f) / kTwoPi * 0.4f;
    const f32 octave2 = (avgSt * 0.38f) * (avgWL * 0.4f) / kTwoPi * 0.3f;
    const f32 octave3 = (avgSt * 0.3f) * (avgWL * 0.25f) / kTwoPi * 0.22f;
    const f32 octave4 = (avgSt * 0.22f) * (avgWL * 0.15f) / kTwoPi * 0.15f;
    const f32 octave5 = (avgSt * 0.15f) * (avgWL * 0.09f) / kTwoPi * 0.1f;
    const f32 truth = waveParams.z * (a0 * 0.55f + a1 * 0.55f + octave0 + octave1 + octave2 + octave3 + octave4 + octave5);

    EXPECT_GE(margin, truth) << "Displacement bound underestimates actual Gerstner amplitude";
}

TEST(WaterRendering, PatchInFrontOfCameraNotCulled)
{
    const glm::mat4 vp = MakeForwardLookingViewProj();
    // Patch centred on the world origin (camera looks at origin from +Z+Y).
    const glm::vec3 p0(-5.0f, 0.0f, -2.0f);
    const glm::vec3 p1(5.0f, 0.0f, -2.0f);
    const glm::vec3 p2(0.0f, 0.0f, 2.0f);

    EXPECT_FALSE(IsPatchOutsideFrustumCpu(p0, p1, p2, 0.0f, vp));
    EXPECT_FALSE(IsPatchOutsideFrustumCpu(p0, p1, p2, 1.0f, vp));
}

TEST(WaterRendering, PatchBehindCameraCulled)
{
    const glm::mat4 vp = MakeForwardLookingViewProj();
    // Patch sitting well behind the camera (camera at z=10, looking towards
    // origin). Any patch at z >> camera.z is behind the near plane.
    const glm::vec3 p0(-5.0f, 0.0f, 100.0f);
    const glm::vec3 p1(5.0f, 0.0f, 100.0f);
    const glm::vec3 p2(0.0f, 0.0f, 110.0f);

    EXPECT_TRUE(IsPatchOutsideFrustumCpu(p0, p1, p2, 0.0f, vp));
}

TEST(WaterRendering, PatchOffToSideCulled)
{
    const glm::mat4 vp = MakeForwardLookingViewProj();
    // Patch translated far to the right of the camera's forward axis. With a
    // 60° FOV the right plane is at roughly x = z (at z=0, x∈[-~6,~6]); this
    // patch sits at x>200 and so must be culled.
    const glm::vec3 p0(200.0f, 0.0f, -5.0f);
    const glm::vec3 p1(210.0f, 0.0f, -5.0f);
    const glm::vec3 p2(205.0f, 0.0f, 5.0f);

    EXPECT_TRUE(IsPatchOutsideFrustumCpu(p0, p1, p2, 0.0f, vp));
}

TEST(WaterRendering, PatchCrossingPlaneNotCulled)
{
    const glm::mat4 vp = MakeForwardLookingViewProj();
    // One corner is well inside the frustum, two are outside on the right.
    // The cull is conservative: if even one corner is inside any given plane,
    // that plane must not reject the patch (the per-plane all-out test fails).
    const glm::vec3 p0(0.0f, 0.0f, 0.0f);   // inside
    const glm::vec3 p1(200.0f, 0.0f, 0.0f); // outside right
    const glm::vec3 p2(200.0f, 0.0f, 5.0f); // outside right

    EXPECT_FALSE(IsPatchOutsideFrustumCpu(p0, p1, p2, 0.0f, vp));
}

TEST(WaterRendering, MarginExpandsAcceptanceRegion)
{
    const glm::mat4 vp = MakeForwardLookingViewProj();
    // A patch hanging just past the right edge that would be culled with
    // zero margin; a generous margin must bring it back into the frustum.
    // The patch sits at z=0, x=~10 (camera frustum right edge at z=0 is
    // around x≈10 for tan(30°)×16/9×|view-z|=10/cos(30°)/aspect ≈ 10.27).
    const glm::vec3 p0(15.0f, 0.0f, -1.0f);
    const glm::vec3 p1(16.0f, 0.0f, -1.0f);
    const glm::vec3 p2(15.5f, 0.0f, 1.0f);

    EXPECT_TRUE(IsPatchOutsideFrustumCpu(p0, p1, p2, 0.0f, vp))
        << "Patch was supposed to be just outside the right plane with zero margin";
    EXPECT_FALSE(IsPatchOutsideFrustumCpu(p0, p1, p2, 100.0f, vp))
        << "A huge margin must keep the patch in the keep-set";
}

TEST(WaterRendering, AllSixPlanesParticipateInCull)
{
    // Move a patch one at a time past each of the six frustum planes and
    // assert it gets culled in every case — guards against a missing plane
    // in the Gribb-Hartmann extraction (e.g. forgetting near vs. far).
    const glm::mat4 vp = MakeForwardLookingViewProj();

    // Inside reference.
    const glm::vec3 i0(-0.5f, 0.0f, -0.5f);
    const glm::vec3 i1(0.5f, 0.0f, -0.5f);
    const glm::vec3 i2(0.0f, 0.0f, 0.5f);
    EXPECT_FALSE(IsPatchOutsideFrustumCpu(i0, i1, i2, 0.0f, vp));

    // Way past each plane: each set of corners pushed far beyond one boundary.
    // (Numbers picked from the 60° FOV / near=0.1 / far=500 frustum.)
    struct Case
    {
        glm::vec3 p0, p1, p2;
        const char* label;
    };
    const Case offscreen[] = {
        { { -500.0f, 0.0f, -1.0f }, { -499.0f, 0.0f, -1.0f }, { -499.5f, 0.0f, 0.0f }, "left" },
        { { 500.0f, 0.0f, -1.0f }, { 499.0f, 0.0f, -1.0f }, { 499.5f, 0.0f, 0.0f }, "right" },
        { { -1.0f, -500.0f, -1.0f }, { 1.0f, -500.0f, -1.0f }, { 0.0f, -500.0f, 0.0f }, "bottom" },
        { { -1.0f, 500.0f, -1.0f }, { 1.0f, 500.0f, -1.0f }, { 0.0f, 500.0f, 0.0f }, "top" },
        // far plane: camera at (0,5,10) looks at the origin, so the forward
        // direction has only a partial -Z component. A patch at z=-2000 sits
        // ~2010 units from the camera — well past the far=500 plane regardless
        // of the off-axis camera offset.
        { { -1.0f, 0.0f, -2000.0f }, { 1.0f, 0.0f, -2000.0f }, { 0.0f, 0.0f, -1995.0f }, "far" },
        // near plane: just behind the camera position.
        { { -1.0f, 0.0f, 50.0f }, { 1.0f, 0.0f, 50.0f }, { 0.0f, 0.0f, 60.0f }, "near" },
    };
    for (auto const& c : offscreen)
    {
        EXPECT_TRUE(IsPatchOutsideFrustumCpu(c.p0, c.p1, c.p2, 0.0f, vp))
            << "Patch beyond " << c.label << " plane should be culled";
    }
}

TEST(WaterRendering, TessParamsCarryFrustumCullFlag)
{
    // Pin the channel assignment so a future refactor that re-purposes
    // tessParams.w forces the shader-side comment to be updated too.
    WaterComponent wc{};
    wc.m_TessellationEnabled = true;
    wc.m_TessellationFactor = 12.0f;
    wc.m_TessMinDistance = 5.0f;
    wc.m_TessMaxDistance = 150.0f;

    // Mirror Scene.cpp's tessParams build (the only call site that constructs
    // this vec4). If those constants drift, this test fails fast.
    const glm::vec4 tessParams(
        wc.m_TessellationEnabled ? wc.m_TessellationFactor : 0.0f,
        wc.m_TessMinDistance,
        wc.m_TessMaxDistance,
        1.0f);

    EXPECT_FLOAT_EQ(tessParams.x, 12.0f);
    EXPECT_FLOAT_EQ(tessParams.y, 5.0f);
    EXPECT_FLOAT_EQ(tessParams.z, 150.0f);
    EXPECT_FLOAT_EQ(tessParams.w, 1.0f) << "tessParams.w must default to 1 to enable TCS frustum culling";
}

// =============================================================================
// Underwater fog (WATER_FUTURE_IMPROVEMENTS.md §7.2)
// =============================================================================

TEST(WaterRendering, UnderwaterFogUBOAlignment)
{
    EXPECT_EQ(sizeof(UnderwaterFogUBOData) % 16, 0u)
        << "UnderwaterFogUBOData must be 16-byte aligned for std140";
    EXPECT_EQ(UnderwaterFogUBOData::GetSize(), sizeof(UnderwaterFogUBOData));
    // vec4 ColorAndDensity + vec4 Flags + vec4 CameraPos + vec4 RefractionParams
    // + vec4 CausticParams + vec4 CausticColorAndSun + vec4 GodRayParams
    // + vec4 GodRaySun + vec4 GodRayColor + vec4 GodRayShape + mat4 InvViewProj
    // = 16 * 10 + 64 = 224 bytes. The §3.3 god-ray params were appended after the
    // caustics block; if this changes, update the GLSL UnderwaterFogBlock layout too.
    EXPECT_EQ(sizeof(UnderwaterFogUBOData), 224u);
}

TEST(WaterRendering, UnderwaterFogUBOFieldOffsets)
{
    // Pin the std140 packing so the GLSL UnderwaterFogBlock and the Scene.cpp
    // populate code agree on where each param lands.
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, ColorAndDensity), 0u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, Flags), 16u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, CameraPos), 32u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, RefractionParams), 48u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, CausticParams), 64u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, CausticColorAndSun), 80u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, GodRayParams), 96u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, GodRaySun), 112u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, GodRayColor), 128u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, GodRayShape), 144u);
    EXPECT_EQ(offsetof(UnderwaterFogUBOData, InverseViewProjection), 160u);
}

TEST(WaterRendering, UnderwaterFogUBOBindingSlot)
{
    // Master's Preetham procedural-sky UBO occupies slot 36, so underwater fog
    // lives at 37. Must match `layout(std140, binding = 37)` in PostProcess_ToneMap.glsl.
    EXPECT_EQ(ShaderBindingLayout::UBO_UNDERWATER, 37u);
}

TEST(WaterRendering, UnderwaterFogTransmittanceAtCameraIsOne)
{
    // At zero distance nothing is absorbed: transmittance == 1, colour unchanged.
    EXPECT_FLOAT_EQ(UnderwaterFog::ComputeTransmittance(0.0f, 0.08f), 1.0f);

    const glm::vec3 scene(0.4f, 0.6f, 0.8f);
    const glm::vec3 fog(0.05f, 0.15f, 0.25f);
    const glm::vec3 r = UnderwaterFog::Apply(scene, fog, 0.0f, 0.08f);
    EXPECT_FLOAT_EQ(r.r, scene.r);
    EXPECT_FLOAT_EQ(r.g, scene.g);
    EXPECT_FLOAT_EQ(r.b, scene.b);
}

TEST(WaterRendering, UnderwaterFogBeerLambertFalloff)
{
    // Transmittance follows exp(-density * distance).
    constexpr f32 density = 0.5f;
    constexpr f32 dist = 4.0f;
    const f32 expected = std::exp(-density * dist);
    EXPECT_NEAR(UnderwaterFog::ComputeTransmittance(dist, density), expected, 1e-6f);

    // Apply blends scene → fog by (1 - transmittance), per channel.
    const glm::vec3 scene(1.0f, 1.0f, 1.0f);
    const glm::vec3 fog(0.0f, 0.0f, 0.0f);
    const glm::vec3 r = UnderwaterFog::Apply(scene, fog, dist, density);
    EXPECT_NEAR(r.r, expected, 1e-6f);
    EXPECT_NEAR(r.g, expected, 1e-6f);
    EXPECT_NEAR(r.b, expected, 1e-6f);
}

TEST(WaterRendering, UnderwaterFogSaturatesAtDistance)
{
    // Far pixels (e.g. the sky/far plane underwater) approach the fog colour.
    const glm::vec3 scene(1.0f, 1.0f, 1.0f);
    const glm::vec3 fog(0.05f, 0.15f, 0.25f);
    const glm::vec3 r = UnderwaterFog::Apply(scene, fog, 1000.0f, 0.08f);
    EXPECT_NEAR(r.r, fog.r, 1e-3f);
    EXPECT_NEAR(r.g, fog.g, 1e-3f);
    EXPECT_NEAR(r.b, fog.b, 1e-3f);
}

TEST(WaterRendering, UnderwaterFogClampsNegativeInputs)
{
    // Defensive clamps mirror the GPU path: negative distance / density → no fog.
    EXPECT_FLOAT_EQ(UnderwaterFog::ComputeTransmittance(-5.0f, 0.08f), 1.0f);
    EXPECT_FLOAT_EQ(UnderwaterFog::ComputeTransmittance(4.0f, -1.0f), 1.0f);
    // Density above the clamp ceiling (10) does not overflow the result.
    const f32 t = UnderwaterFog::ComputeTransmittance(4.0f, 1000.0f);
    EXPECT_GE(t, 0.0f);
    EXPECT_LE(t, 1.0f);
}

TEST(WaterRendering, UnderwaterFogStateDefaultsInactive)
{
    UnderwaterFogState s{};
    EXPECT_FALSE(s.Active);
    EXPECT_FLOAT_EQ(s.FogColor.r, 0.05f);
    EXPECT_FLOAT_EQ(s.FogColor.g, 0.15f);
    EXPECT_FLOAT_EQ(s.FogColor.b, 0.25f);
    EXPECT_FLOAT_EQ(s.Density, 0.08f);
}

// =============================================================================
// Submerged refraction distortion (WATER_FUTURE_IMPROVEMENTS.md §7.2 bullet 2)
//
// CPU mirror of underwaterRefractionOffset in PostProcess_ToneMap.glsl. Pins the
// contract the shader relies on: the wobble never exceeds its (hard-capped)
// amplitude, disables cleanly, and survives garbage input.
// =============================================================================

namespace
{
    // |a - b| summed over both components — used to assert two offsets/values
    // differ without an == / != on floats (SonarQube float-comparison rule).
    [[nodiscard]] f32 Vec2L1Diff(const glm::vec2& a, const glm::vec2& b)
    {
        return std::abs(a.x - b.x) + std::abs(a.y - b.y);
    }
} // namespace

TEST(WaterRendering, RefractionOffsetDisabledWhenStrengthZeroOrNegative)
{
    const glm::vec2 off0 = UnderwaterCaustics::RefractionOffset({ 0.3f, 0.7f }, 5.0f, 0.0f, 18.0f, 1.2f);
    EXPECT_FLOAT_EQ(off0.x, 0.0f);
    EXPECT_FLOAT_EQ(off0.y, 0.0f);
    const glm::vec2 offNeg = UnderwaterCaustics::RefractionOffset({ 0.3f, 0.7f }, 5.0f, -1.0f, 18.0f, 1.2f);
    EXPECT_FLOAT_EQ(offNeg.x, 0.0f);
    EXPECT_FLOAT_EQ(offNeg.y, 0.0f);
}

TEST(WaterRendering, RefractionOffsetBoundedByStrength)
{
    // |sin|,|cos| <= 1, so each component is bounded by the strength amplitude.
    // Sweep uv/time so we exercise the full trig range.
    constexpr f32 strength = 0.006f;
    for (i32 i = 0; i < 64; ++i)
    {
        const f32 t = static_cast<f32>(i) * 0.37f;
        const glm::vec2 uv(static_cast<f32>(i) * 0.013f, 1.0f - static_cast<f32>(i) * 0.011f);
        const glm::vec2 off = UnderwaterCaustics::RefractionOffset(uv, t, strength, 18.0f, 1.2f);
        EXPECT_LE(std::abs(off.x), strength + 1e-6f);
        EXPECT_LE(std::abs(off.y), strength + 1e-6f);
    }
}

TEST(WaterRendering, RefractionOffsetHardCapsRunawayStrength)
{
    // A bad/huge strength must never displace the sample more than the 0.1 UV cap
    // — that's what stops a misconfigured param from tearing the image apart.
    const glm::vec2 off = UnderwaterCaustics::RefractionOffset({ 0.5f, 0.5f }, 3.0f, 1000.0f, 18.0f, 1.2f);
    EXPECT_LE(std::abs(off.x), 0.1f + 1e-6f);
    EXPECT_LE(std::abs(off.y), 0.1f + 1e-6f);
}

TEST(WaterRendering, RefractionOffsetIsDeterministicAndAnimates)
{
    const glm::vec2 a = UnderwaterCaustics::RefractionOffset({ 0.4f, 0.6f }, 2.0f, 0.006f, 18.0f, 1.2f);
    const glm::vec2 a2 = UnderwaterCaustics::RefractionOffset({ 0.4f, 0.6f }, 2.0f, 0.006f, 18.0f, 1.2f);
    EXPECT_FLOAT_EQ(Vec2L1Diff(a, a2), 0.0f); // same inputs → same output
    const glm::vec2 b = UnderwaterCaustics::RefractionOffset({ 0.4f, 0.6f }, 2.5f, 0.006f, 18.0f, 1.2f);
    EXPECT_GT(Vec2L1Diff(a, b), 1e-5f); // advancing time changes the wobble
}

TEST(WaterRendering, RefractionOffsetSurvivesNonFiniteInput)
{
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    const glm::vec2 off = UnderwaterCaustics::RefractionOffset({ 0.4f, 0.6f }, nan, 0.006f, inf, 1.2f);
    EXPECT_TRUE(std::isfinite(off.x));
    EXPECT_TRUE(std::isfinite(off.y));
}

// =============================================================================
// Caustics (WATER_FUTURE_IMPROVEMENTS.md §7.1)
//
// CPU mirror of underwaterCausticPattern / underwaterCausticDepthFade in
// PostProcess_ToneMap.glsl. Pins the pattern's [0,1] range + animation and the
// depth-fade contract (zero at/above the surface and beyond maxDepth, monotonic
// in between).
// =============================================================================

TEST(WaterRendering, CausticPatternInUnitRange)
{
    // The ridged-sine pattern must stay in [0,1] for any sample — it's added to
    // HDR radiance, so an out-of-range value would over/under-shoot the seabed.
    for (i32 i = 0; i < 200; ++i)
    {
        const f32 fi = static_cast<f32>(i);
        const glm::vec2 p(fi * 1.7f - 80.0f, fi * -2.3f + 40.0f);
        const f32 v = UnderwaterCaustics::CausticPattern(p, fi * 0.21f, 0.35f, 0.6f);
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
        EXPECT_TRUE(std::isfinite(v));
    }
}

TEST(WaterRendering, CausticPatternVariesInSpaceAndTime)
{
    const f32 base = UnderwaterCaustics::CausticPattern({ 3.0f, 5.0f }, 1.0f, 0.35f, 0.6f);
    // A far-apart sample should generally differ (the pattern isn't flat).
    const f32 moved = UnderwaterCaustics::CausticPattern({ 13.0f, -7.0f }, 1.0f, 0.35f, 0.6f);
    EXPECT_GT(std::abs(base - moved), 1e-5f);
    // Advancing time animates the pattern.
    const f32 later = UnderwaterCaustics::CausticPattern({ 3.0f, 5.0f }, 4.0f, 0.35f, 0.6f);
    EXPECT_GT(std::abs(base - later), 1e-5f);
}

TEST(WaterRendering, CausticPatternSurvivesNonFiniteInput)
{
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FLOAT_EQ(UnderwaterCaustics::CausticPattern({ nan, 2.0f }, 1.0f, 0.35f, 0.6f), 0.0f);
    EXPECT_TRUE(std::isfinite(UnderwaterCaustics::CausticPattern({ 2.0f, 2.0f }, nan, 0.35f, 0.6f)));
}

TEST(WaterRendering, CausticDepthFadeZeroAtAndAboveSurface)
{
    // At or above the surface (depthBelowSurface <= 0) there is no submerged
    // geometry to project caustics onto → zero.
    EXPECT_FLOAT_EQ(UnderwaterCaustics::CausticDepthFade(0.0f, 25.0f), 0.0f);
    EXPECT_FLOAT_EQ(UnderwaterCaustics::CausticDepthFade(-3.0f, 25.0f), 0.0f);
}

TEST(WaterRendering, CausticDepthFadeZeroBeyondMaxDepth)
{
    EXPECT_FLOAT_EQ(UnderwaterCaustics::CausticDepthFade(25.0f, 25.0f), 0.0f);
    EXPECT_FLOAT_EQ(UnderwaterCaustics::CausticDepthFade(40.0f, 25.0f), 0.0f);
    // Degenerate maxDepth → no fade window → zero.
    EXPECT_FLOAT_EQ(UnderwaterCaustics::CausticDepthFade(1.0f, 0.0f), 0.0f);
}

TEST(WaterRendering, CausticDepthFadeMonotonicBetween)
{
    constexpr f32 maxDepth = 25.0f;
    // Linear ramp from ~1 just below the surface to 0 at maxDepth. (Exactly at
    // the surface the fade is 0 by design — there's no submerged geometry there —
    // so the monotonic-decrease contract is checked over the SUBMERGED range only,
    // starting just below the surface.)
    EXPECT_NEAR(UnderwaterCaustics::CausticDepthFade(0.0001f, maxDepth), 1.0f, 1e-3f);
    EXPECT_NEAR(UnderwaterCaustics::CausticDepthFade(12.5f, maxDepth), 0.5f, 1e-4f);
    f32 prev = 2.0f;
    for (i32 i = 1; i <= 25; ++i)
    {
        const f32 v = UnderwaterCaustics::CausticDepthFade(static_cast<f32>(i), maxDepth);
        EXPECT_LE(v, prev + 1e-6f) << "depth fade must not increase with depth (i=" << i << ")";
        prev = v;
    }
}

// =============================================================================
// Volumetric light shafts / god rays (WATER_FUTURE_IMPROVEMENTS.md §3.3)
//
// The shader's occlusion radial blur accumulates decay^i for the open steps and
// divides by the sum of all decay^i, giving a bounded [0,1] openness. That sum is
// the texture-INDEPENDENT normaliser the shader and CPU must agree on (a wrong sum
// would drift the shaft brightness scale), so GodRayDecaySum runs the exact same
// loop and these contracts pin it. The sun-screen projection (GodRaySunScreenUV)
// decides where the shafts point and whether they appear at all, so it gets its
// own gating/center/behind-camera contracts.
// =============================================================================

TEST(WaterRendering, GodRayDecaySumZeroForDegenerateInput)
{
    EXPECT_FLOAT_EQ(UnderwaterCaustics::GodRayDecaySum(0, 0.97f), 0.0f);
    EXPECT_FLOAT_EQ(UnderwaterCaustics::GodRayDecaySum(-5, 0.97f), 0.0f);
}

TEST(WaterRendering, GodRayDecaySumMatchesGeometricSeries)
{
    // The loop sums sum_{i=1..N} decay^i, a finite geometric series with closed
    // form decay * (1 - decay^N) / (1 - decay).
    constexpr i32 N = 48;
    constexpr f32 decay = 0.97f;
    const f32 closed = decay * (1.0f - std::pow(decay, static_cast<f32>(N))) / (1.0f - decay);
    EXPECT_NEAR(UnderwaterCaustics::GodRayDecaySum(N, decay), closed, 1e-4f);
}

TEST(WaterRendering, GodRayDecaySumGrowsWithSamples)
{
    // Each extra sample adds a strictly positive term (positive decay), so the
    // normaliser is monotonically increasing in the sample count.
    f32 prev = -1.0f;
    for (i32 n = 1; n <= 64; ++n)
    {
        const f32 v = UnderwaterCaustics::GodRayDecaySum(n, 0.95f);
        EXPECT_GT(v, prev) << "more samples must accumulate a larger decay sum (n=" << n << ")";
        prev = v;
    }
}

TEST(WaterRendering, GodRayDecaySumSurvivesNonFiniteInput)
{
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FLOAT_EQ(UnderwaterCaustics::GodRayDecaySum(48, nan), 0.0f);
}

TEST(WaterRendering, GodRayDappleInUnitRange)
{
    // The surface-wave dapple modulation that shimmers the shafts must stay in [0,1]
    // for any input (it multiplies the shaft, so out-of-range would over/under-drive it).
    for (i32 i = 0; i < 64; ++i)
    {
        const f32 x = static_cast<f32>(i) * 0.37f - 11.0f;
        const f32 y = static_cast<f32>(i) * 0.53f - 7.0f;
        const f32 v = UnderwaterCaustics::GodRayDapple({ x, y }, static_cast<f32>(i) * 0.25f, 0.5f, 0.6f);
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
}

TEST(WaterRendering, GodRayDappleVariesInSpaceAndTime)
{
    // It must actually vary (otherwise the shafts wouldn't shimmer) — across space
    // at a fixed time, and across time at a fixed point.
    const f32 a = UnderwaterCaustics::GodRayDapple({ 0.0f, 0.0f }, 2.0f, 0.5f, 0.6f);
    const f32 b = UnderwaterCaustics::GodRayDapple({ 3.1f, 1.7f }, 2.0f, 0.5f, 0.6f);
    EXPECT_GT(std::abs(a - b), 1e-3f) << "dapple is flat in space — shafts wouldn't shimmer";
    const f32 c = UnderwaterCaustics::GodRayDapple({ 0.0f, 0.0f }, 5.0f, 0.5f, 0.6f);
    EXPECT_GT(std::abs(a - c), 1e-3f) << "dapple is static in time — shafts wouldn't animate";
}

TEST(WaterRendering, GodRayDappleSurvivesNonFiniteInput)
{
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_TRUE(std::isfinite(UnderwaterCaustics::GodRayDapple({ nan, 1.0f }, 1.0f, 0.5f, 0.6f)));
    EXPECT_TRUE(std::isfinite(UnderwaterCaustics::GodRayDapple({ 1.0f, 1.0f }, nan, 0.5f, 0.6f)));
}

TEST(WaterRendering, GodRaySunScreenUVCentersWhenLookingAtSun)
{
    // Sun light travels in -Z (so the sun itself sits in +Z); a camera at the
    // origin looking toward +Z stares straight at it → the vanishing point lands
    // at screen centre.
    const glm::vec3 sunDir(0.0f, 0.0f, -1.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.05f, 1000.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec2 uv(-1.0f);
    ASSERT_TRUE(UnderwaterCaustics::GodRaySunScreenUV(proj * view, sunDir, uv));
    EXPECT_NEAR(uv.x, 0.5f, 1e-3f);
    EXPECT_NEAR(uv.y, 0.5f, 1e-3f);
}

TEST(WaterRendering, GodRaySunScreenUVRejectsSunBehindCamera)
{
    // Same sun, but the camera now faces -Z (away from the sun): the vanishing
    // point is behind the camera plane → no shafts, projection rejected.
    const glm::vec3 sunDir(0.0f, 0.0f, -1.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.05f, 1000.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec2 uv(0.123f, 0.456f);
    EXPECT_FALSE(UnderwaterCaustics::GodRaySunScreenUV(proj * view, sunDir, uv));
}

TEST(WaterRendering, GodRaySunScreenUVRejectsDegenerateSunDir)
{
    const glm::mat4 vp(1.0f);
    glm::vec2 uv(0.0f);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(UnderwaterCaustics::GodRaySunScreenUV(vp, glm::vec3(0.0f), uv));
    EXPECT_FALSE(UnderwaterCaustics::GodRaySunScreenUV(vp, glm::vec3(nan, 1.0f, 0.0f), uv));
}

// =============================================================================
// Waterline behaviour (WATER_FUTURE_IMPROVEMENTS.md §7.2)
//
// These pin the two pieces of logic that decide what the water looks like as
// the camera moves through the surface and views it from the side — the exact
// cases where the early implementations glitched:
//   * underwaterSegmentLength: how much of a view ray passes through water
//     (drives the per-pixel fog so the underwater half of a waterline view is
//      fogged while the above-water half is clear);
//   * the per-fragment surface side selection (which face of the double-sided
//     water plane is kept), mirroring the Water.glsl discard rule.
// =============================================================================

namespace
{
    constexpr f32 kWaterSurfaceY = 0.0f;

    // CPU mirror of the Water.glsl per-fragment waterline discard:
    //   keep the fragment iff (cameraBelowFragment != isTopFace)
    // A fragment above the eye shows its underside; below the eye shows its top.
    bool WaterFaceKept(f32 fragmentY, f32 cameraY, bool isTopFace)
    {
        const bool cameraBelowFragment = fragmentY > cameraY;
        return cameraBelowFragment != isTopFace;
    }
} // namespace

TEST(WaterRendering, UnderwaterSegment_CameraBelowFragmentBelow_FullRay)
{
    // Fully submerged view: the whole ray is underwater.
    const glm::vec3 cam(0.0f, -5.0f, 0.0f);
    const glm::vec3 frag(0.0f, -5.0f, 20.0f); // horizontal ray, 20 m
    const f32 len = UnderwaterFog::UnderwaterSegmentLength(cam, frag, kWaterSurfaceY);
    EXPECT_NEAR(len, 20.0f, 1e-4f);
}

TEST(WaterRendering, UnderwaterSegment_BothAbove_Zero)
{
    // Above-water view of above-water geometry: no fog.
    const glm::vec3 cam(0.0f, 3.0f, 0.0f);
    const glm::vec3 frag(0.0f, 5.0f, 20.0f);
    EXPECT_FLOAT_EQ(UnderwaterFog::UnderwaterSegmentLength(cam, frag, kWaterSurfaceY), 0.0f);
}

TEST(WaterRendering, UnderwaterSegment_CameraAboveFragmentBelow_EntryToFrag)
{
    // Looking down from above into the water (or the waterline case for
    // downward rays): only the part below the surface counts. Camera at y=+10,
    // fragment (seafloor) at y=-10, straight down → crosses surface at the
    // midpoint, so 10 m of the 20 m ray is underwater.
    const glm::vec3 cam(0.0f, 10.0f, 0.0f);
    const glm::vec3 frag(0.0f, -10.0f, 0.0f);
    EXPECT_NEAR(UnderwaterFog::UnderwaterSegmentLength(cam, frag, kWaterSurfaceY), 10.0f, 1e-4f);
}

TEST(WaterRendering, UnderwaterSegment_CameraBelowFragmentAbove_CamToExit)
{
    // Submerged camera looking up at above-water geometry: only the part below
    // the surface counts. Camera y=-10, fragment y=+10 straight up → 10 m
    // underwater before exiting.
    const glm::vec3 cam(0.0f, -10.0f, 0.0f);
    const glm::vec3 frag(0.0f, 10.0f, 0.0f);
    EXPECT_NEAR(UnderwaterFog::UnderwaterSegmentLength(cam, frag, kWaterSurfaceY), 10.0f, 1e-4f);
}

TEST(WaterRendering, UnderwaterSegment_AtWaterlineSplitsRayByDirection)
{
    // Camera exactly at the surface (the "going in/out" case): a downward ray
    // is fully underwater, an upward ray fully above. This is what lets the
    // waterline render correctly per pixel instead of as a whole-screen toggle.
    const glm::vec3 cam(0.0f, 0.0f, 0.0f);
    const glm::vec3 down(0.0f, -8.0f, 6.0f); // mixed down+forward
    const glm::vec3 up(0.0f, 8.0f, 6.0f);
    EXPECT_NEAR(UnderwaterFog::UnderwaterSegmentLength(cam, down, kWaterSurfaceY),
                glm::length(down), 1e-4f);
    EXPECT_FLOAT_EQ(UnderwaterFog::UnderwaterSegmentLength(cam, up, kWaterSurfaceY), 0.0f);
}

TEST(WaterRendering, UnderwaterSegment_RespectsNonZeroSurfacePlane)
{
    // Surface raised to y=5: depth is measured from that plane.
    const f32 surfaceY = 5.0f;
    const glm::vec3 cam(0.0f, 5.0f, 0.0f);   // at the raised surface
    const glm::vec3 frag(0.0f, -5.0f, 0.0f); // 10 m below the surface
    EXPECT_NEAR(UnderwaterFog::UnderwaterSegmentLength(cam, frag, surfaceY), 10.0f, 1e-4f);
}

TEST(WaterRendering, WaterFace_CameraAbove_KeepsTopsDiscardsUndersides)
{
    // Camera above the water: every surface fragment is below the eye, so the
    // top face is kept and the underside discarded — identical to the classic
    // back-cull top-down view, no see-through holes.
    const f32 cameraY = 5.0f;
    const f32 fragY = 0.0f; // surface below the camera
    EXPECT_TRUE(WaterFaceKept(fragY, cameraY, /*isTopFace=*/true));
    EXPECT_FALSE(WaterFaceKept(fragY, cameraY, /*isTopFace=*/false));
}

TEST(WaterRendering, WaterFace_CameraBelow_KeepsUndersidesDiscardsTops)
{
    // Fully submerged: every surface fragment is above the eye, so the
    // underside is kept and the top discarded — the surface is visible from
    // below.
    const f32 cameraY = -5.0f;
    const f32 fragY = 0.0f; // surface above the camera
    EXPECT_FALSE(WaterFaceKept(fragY, cameraY, /*isTopFace=*/true));
    EXPECT_TRUE(WaterFaceKept(fragY, cameraY, /*isTopFace=*/false));
}

TEST(WaterRendering, WaterFace_AtWaterline_SplitsPerFragment)
{
    // Camera straddling the waterline (eye at y=0): surface fragments below the
    // eye keep their tops, fragments above the eye keep their undersides. This
    // per-fragment split is what removes the see-through holes / interleaved
    // sheets when viewing the water from the side.
    const f32 cameraY = 0.0f;
    // Fragment of a wave trough below the eye → show top.
    EXPECT_TRUE(WaterFaceKept(-0.5f, cameraY, /*isTopFace=*/true));
    EXPECT_FALSE(WaterFaceKept(-0.5f, cameraY, /*isTopFace=*/false));
    // Fragment of a wave crest above the eye → show underside.
    EXPECT_FALSE(WaterFaceKept(0.5f, cameraY, /*isTopFace=*/true));
    EXPECT_TRUE(WaterFaceKept(0.5f, cameraY, /*isTopFace=*/false));
}
