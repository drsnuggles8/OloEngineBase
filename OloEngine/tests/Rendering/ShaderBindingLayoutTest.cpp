#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <set>
#include <string>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// UBO Size Alignment — std140 requires 16-byte alignment
// =============================================================================

TEST(ShaderBindingLayout, CameraUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::CameraUBO) % 16, 0u)
        << "CameraUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, LightUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::LightUBO) % 16, 0u)
        << "LightUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, MaterialUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::MaterialUBO) % 16, 0u)
        << "MaterialUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, PBRMaterialUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::PBRMaterialUBO) % 16, 0u)
        << "PBRMaterialUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, ModelUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::ModelUBO) % 16, 0u)
        << "ModelUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, AnimationUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::AnimationUBO) % 16, 0u)
        << "AnimationUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, MultiLightUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::MultiLightUBO) % 16, 0u)
        << "MultiLightUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, ShadowUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::ShadowUBO) % 16, 0u)
        << "ShadowUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, TerrainUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::TerrainUBO) % 16, 0u)
        << "TerrainUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, BrushPreviewUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::BrushPreviewUBO) % 16, 0u)
        << "BrushPreviewUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, FoliageUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::FoliageUBO) % 16, 0u)
        << "FoliageUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, DecalUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::DecalUBO) % 16, 0u)
        << "DecalUBO must be 16-byte aligned for std140";
}

TEST(ShaderBindingLayout, IBLParametersUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::IBLParametersUBO) % 16, 0u)
        << "IBLParametersUBO must be 16-byte aligned for std140";
}

// =============================================================================
// UBO Size Stability — Known expected sizes (if they change, shader layouts break)
// =============================================================================

TEST(ShaderBindingLayout, TerrainUBOSizeStable)
{
    EXPECT_EQ(sizeof(UBOStructures::TerrainUBO), 144u);
}

TEST(ShaderBindingLayout, BrushPreviewUBOSizeStable)
{
    EXPECT_EQ(sizeof(UBOStructures::BrushPreviewUBO), 32u);
}

TEST(ShaderBindingLayout, FoliageUBOSizeStable)
{
    EXPECT_EQ(sizeof(UBOStructures::FoliageUBO), 48u);
}

TEST(ShaderBindingLayout, DecalUBOSizeStable)
{
    EXPECT_EQ(sizeof(UBOStructures::DecalUBO), 160u);
}

// =============================================================================
// Binding Slot Uniqueness — No Two UBO Types Share the Same Slot
// =============================================================================

TEST(ShaderBindingLayout, UBOBindingSlotUniqueness)
{
    std::set<u32> usedSlots;

    auto checkSlot = [&usedSlots](u32 slot, const char* name)
    {
        EXPECT_EQ(usedSlots.count(slot), 0u)
            << "UBO binding slot " << slot << " (" << name << ") is already in use!";
        usedSlots.insert(slot);
    };

    checkSlot(ShaderBindingLayout::UBO_CAMERA, "UBO_CAMERA");
    checkSlot(ShaderBindingLayout::UBO_LIGHTS, "UBO_LIGHTS");
    checkSlot(ShaderBindingLayout::UBO_MATERIAL, "UBO_MATERIAL");
    checkSlot(ShaderBindingLayout::UBO_MODEL, "UBO_MODEL");
    checkSlot(ShaderBindingLayout::UBO_ANIMATION, "UBO_ANIMATION");
    checkSlot(ShaderBindingLayout::UBO_MULTI_LIGHTS, "UBO_MULTI_LIGHTS");
    checkSlot(ShaderBindingLayout::UBO_SHADOW, "UBO_SHADOW");
    checkSlot(ShaderBindingLayout::UBO_USER_0, "UBO_USER_0");
    checkSlot(ShaderBindingLayout::UBO_USER_1, "UBO_USER_1");
    checkSlot(ShaderBindingLayout::UBO_SSAO, "UBO_SSAO");
    checkSlot(ShaderBindingLayout::UBO_TERRAIN, "UBO_TERRAIN");
    checkSlot(ShaderBindingLayout::UBO_BRUSH_PREVIEW, "UBO_BRUSH_PREVIEW");
    checkSlot(ShaderBindingLayout::UBO_FOLIAGE, "UBO_FOLIAGE");
    checkSlot(ShaderBindingLayout::UBO_SNOW, "UBO_SNOW");
    checkSlot(ShaderBindingLayout::UBO_SSS, "UBO_SSS");
    checkSlot(ShaderBindingLayout::UBO_WIND, "UBO_WIND");
    checkSlot(ShaderBindingLayout::UBO_SNOW_ACCUMULATION, "UBO_SNOW_ACCUMULATION");
    checkSlot(ShaderBindingLayout::UBO_FOG, "UBO_FOG");
    checkSlot(ShaderBindingLayout::UBO_PRECIPITATION, "UBO_PRECIPITATION");
    checkSlot(ShaderBindingLayout::UBO_PRECIPITATION_SCREEN, "UBO_PRECIPITATION_SCREEN");
    checkSlot(ShaderBindingLayout::UBO_FOG_VOLUMES, "UBO_FOG_VOLUMES");
    checkSlot(ShaderBindingLayout::UBO_DECAL, "UBO_DECAL");
}

// =============================================================================
// Texture Slot Uniqueness (engine-defined standard slots)
// =============================================================================

TEST(ShaderBindingLayout, TextureSlotUniqueness)
{
    std::set<u32> usedSlots;

    auto checkSlot = [&usedSlots](u32 slot, const char* name)
    {
        EXPECT_EQ(usedSlots.count(slot), 0u)
            << "Texture slot " << slot << " (" << name << ") is already in use!";
        usedSlots.insert(slot);
    };

    checkSlot(ShaderBindingLayout::TEX_DIFFUSE, "TEX_DIFFUSE");
    checkSlot(ShaderBindingLayout::TEX_SPECULAR, "TEX_SPECULAR");
    checkSlot(ShaderBindingLayout::TEX_NORMAL, "TEX_NORMAL");
    checkSlot(ShaderBindingLayout::TEX_HEIGHT, "TEX_HEIGHT");
    checkSlot(ShaderBindingLayout::TEX_AMBIENT, "TEX_AMBIENT");
    checkSlot(ShaderBindingLayout::TEX_EMISSIVE, "TEX_EMISSIVE");
    checkSlot(ShaderBindingLayout::TEX_ROUGHNESS, "TEX_ROUGHNESS");
    checkSlot(ShaderBindingLayout::TEX_METALLIC, "TEX_METALLIC");
    checkSlot(ShaderBindingLayout::TEX_SHADOW, "TEX_SHADOW");
    checkSlot(ShaderBindingLayout::TEX_ENVIRONMENT, "TEX_ENVIRONMENT");
}

// =============================================================================
// SSBO Slot Uniqueness
// =============================================================================

TEST(ShaderBindingLayout, SSBOSlotUniqueness)
{
    std::set<u32> usedSlots;

    auto checkSlot = [&usedSlots](u32 slot, const char* name)
    {
        EXPECT_EQ(usedSlots.count(slot), 0u)
            << "SSBO slot " << slot << " (" << name << ") is already in use!";
        usedSlots.insert(slot);
    };

    checkSlot(ShaderBindingLayout::SSBO_GPU_PARTICLES, "SSBO_GPU_PARTICLES");
    checkSlot(ShaderBindingLayout::SSBO_ALIVE_INDICES, "SSBO_ALIVE_INDICES");
    checkSlot(ShaderBindingLayout::SSBO_COUNTERS, "SSBO_COUNTERS");
    checkSlot(ShaderBindingLayout::SSBO_FREE_LIST, "SSBO_FREE_LIST");
    checkSlot(ShaderBindingLayout::SSBO_INDIRECT_DRAW, "SSBO_INDIRECT_DRAW");
    checkSlot(ShaderBindingLayout::SSBO_EMIT_STAGING, "SSBO_EMIT_STAGING");
    checkSlot(ShaderBindingLayout::SSBO_FOLIAGE_INSTANCES, "SSBO_FOLIAGE_INSTANCES");
    checkSlot(ShaderBindingLayout::SSBO_SNOW_DEFORMERS, "SSBO_SNOW_DEFORMERS");
}

// =============================================================================
// Animation Constants Consistency
// =============================================================================

TEST(ShaderBindingLayout, AnimationConstantsConsistency)
{
    EXPECT_EQ(UBOStructures::AnimationConstants::MAX_BONES,
              UBOStructures::AnimationUBO::MAX_BONES)
        << "MAX_BONES constants must be synchronized";
}

// =============================================================================
// ShaderConstantGenerator Output
// =============================================================================

TEST(ShaderBindingLayout, ShaderConstantGeneratorRoundTrip)
{
    std::string animDefines = UBOStructures::ShaderConstantGenerator::GetAnimationDefines();
    std::string expected = "#define MAX_BONES " +
                           std::to_string(UBOStructures::AnimationConstants::MAX_BONES) + "\n";
    EXPECT_EQ(animDefines, expected);

    std::string lightDefines = UBOStructures::ShaderConstantGenerator::GetLightingDefines();
    std::string expectedLight = "#define MAX_LIGHTS " +
                                std::to_string(UBOStructures::MultiLightUBO::MAX_LIGHTS) + "\n";
    EXPECT_EQ(lightDefines, expectedLight);
}

// =============================================================================
// UBO Binding Name Validation
// =============================================================================

TEST(ShaderBindingLayout, KnownUBOBindingRecognized)
{
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_CAMERA, "CameraMatrices"));
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_LIGHTS, "LightProperties"));
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_MATERIAL, "MaterialProperties"));
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_MODEL, "ModelMatrices"));
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_ANIMATION, "AnimationMatrices"));
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_SHADOW, "ShadowData"));
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_TERRAIN, "TerrainData"));
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_FOLIAGE, "FoliageParams"));
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_DECAL, "DecalData"));
}

TEST(ShaderBindingLayout, UnknownUBOBindingRejected)
{
    EXPECT_FALSE(ShaderBindingLayout::IsKnownUBOBinding(100, "SomethingRandom"));
    EXPECT_FALSE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_CAMERA, "SomethingRandom"));
}

// =============================================================================
// Texture Slot Fits GL Minimum
// =============================================================================

TEST(ShaderBindingLayout, AllTextureSlotsWithinGLMinimum)
{
    // GL 4.6 guarantees at least 80 combined texture image units
    EXPECT_LT(ShaderBindingLayout::TEX_PRECIPITATION_NOISE, 80u);
}

// =============================================================================
// UBO GetSize() Matches sizeof()
// =============================================================================

TEST(ShaderBindingLayout, UBOGetSizeMatchesSizeof)
{
    EXPECT_EQ(UBOStructures::CameraUBO::GetSize(), sizeof(UBOStructures::CameraUBO));
    EXPECT_EQ(UBOStructures::LightUBO::GetSize(), sizeof(UBOStructures::LightUBO));
    EXPECT_EQ(UBOStructures::MaterialUBO::GetSize(), sizeof(UBOStructures::MaterialUBO));
    EXPECT_EQ(UBOStructures::PBRMaterialUBO::GetSize(), sizeof(UBOStructures::PBRMaterialUBO));
    EXPECT_EQ(UBOStructures::ModelUBO::GetSize(), sizeof(UBOStructures::ModelUBO));
    EXPECT_EQ(UBOStructures::AnimationUBO::GetSize(), sizeof(UBOStructures::AnimationUBO));
    EXPECT_EQ(UBOStructures::MultiLightUBO::GetSize(), sizeof(UBOStructures::MultiLightUBO));
    EXPECT_EQ(UBOStructures::ShadowUBO::GetSize(), sizeof(UBOStructures::ShadowUBO));
    EXPECT_EQ(UBOStructures::TerrainUBO::GetSize(), sizeof(UBOStructures::TerrainUBO));
    EXPECT_EQ(UBOStructures::BrushPreviewUBO::GetSize(), sizeof(UBOStructures::BrushPreviewUBO));
    EXPECT_EQ(UBOStructures::FoliageUBO::GetSize(), sizeof(UBOStructures::FoliageUBO));
    EXPECT_EQ(UBOStructures::DecalUBO::GetSize(), sizeof(UBOStructures::DecalUBO));
    EXPECT_EQ(UBOStructures::IBLParametersUBO::GetSize(), sizeof(UBOStructures::IBLParametersUBO));
}
