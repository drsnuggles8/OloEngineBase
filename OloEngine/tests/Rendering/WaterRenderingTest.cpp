#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Scene/Components.h"

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
    ValidateVec4(cmd.screenParams, "screenParams");
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
