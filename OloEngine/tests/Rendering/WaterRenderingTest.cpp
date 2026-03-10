#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Scene/Components.h"

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
    // 6 x glm::vec4 = 6 x 16 = 96 bytes
    EXPECT_EQ(sizeof(UBOStructures::WaterUBO), 96u);
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
    UBOStructures::WaterUBO ubo{};
    ubo.WaveParams = glm::vec4(1.0f, 2.0f, 0.5f, 3.0f);
    ubo.WaveDir0 = glm::vec4(1.0f, 0.0f, 0.5f, 6.28f);
    ubo.WaveDir1 = glm::vec4(0.7f, 0.7f, 0.3f, 4.0f);
    ubo.WaterColor = glm::vec4(0.1f, 0.4f, 0.5f, 0.6f);
    ubo.WaterDeepColor = glm::vec4(0.0f, 0.1f, 0.2f, 0.5f);
    ubo.VisualParams = glm::vec4(5.0f, 1.0f, 0.0f, 0.0f);

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

    WaterComponent copy(original);

    // Serialized fields should be copied
    EXPECT_FLOAT_EQ(copy.m_WorldSizeX, 200.0f);
    EXPECT_FLOAT_EQ(copy.m_WaveAmplitude, 1.5f);
    EXPECT_FLOAT_EQ(copy.m_Wavelength0, 20.0f);
    EXPECT_FLOAT_EQ(copy.m_Wavelength1, 30.0f);
    EXPECT_FLOAT_EQ(copy.m_WaterColor.r, 0.2f);
    EXPECT_FALSE(copy.m_Enabled);

    // Runtime state should NOT be copied — mesh is null, needs rebuild
    EXPECT_EQ(copy.m_WaterMesh, nullptr);
    EXPECT_TRUE(copy.m_NeedsRebuild);
}

TEST(WaterRendering, WaterComponentAssignmentOmitsRuntime)
{
    WaterComponent original{};
    original.m_WorldSizeZ = 50.0f;
    original.m_FresnelPower = 3.0f;

    WaterComponent target{};
    target = original;

    EXPECT_FLOAT_EQ(target.m_WorldSizeZ, 50.0f);
    EXPECT_FLOAT_EQ(target.m_FresnelPower, 3.0f);
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

    // Simulate the packing that Scene.cpp performs for the shader UBO
    glm::vec4 waveDir0 = glm::vec4(
        wc.m_WaveDir0.x, wc.m_WaveDir0.y,
        wc.m_WaveSteepness0, wc.m_Wavelength0);
    glm::vec4 waveDir1 = glm::vec4(
        wc.m_WaveDir1.x, wc.m_WaveDir1.y,
        wc.m_WaveSteepness1, wc.m_Wavelength1);

    EXPECT_FLOAT_EQ(waveDir0.w, 12.0f) << "waveDir0.w must carry wavelength0";
    EXPECT_FLOAT_EQ(waveDir1.w, 18.0f) << "waveDir1.w must carry wavelength1";
    EXPECT_FLOAT_EQ(waveDir0.z, 0.5f) << "waveDir0.z must carry steepness0";
    EXPECT_FLOAT_EQ(waveDir1.z, 0.3f) << "waveDir1.z must carry steepness1";
}
