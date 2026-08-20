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
    // 144 before issue #715 appended the three virtual-texture vec4s. The GLSL
    // side is include/TerrainParamsBlock.glsl — ONE declaration shared by all
    // eight terrain shaders, so bumping this number and forgetting the shader is
    // a single-file fix rather than the thirteen-copy hunt it used to be.
    EXPECT_EQ(sizeof(UBOStructures::TerrainUBO), 192u);
}

TEST(ShaderBindingLayout, BrushPreviewUBOSizeStable)
{
    EXPECT_EQ(sizeof(UBOStructures::BrushPreviewUBO), 32u);
}

TEST(ShaderBindingLayout, FoliageUBOSizeStable)
{
    // 48 base bytes + 2 vec4 octahedral impostor params (issue #433) = 80.
    EXPECT_EQ(sizeof(UBOStructures::FoliageUBO), 80u);
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
    // Binding 1 (formerly UBO_LIGHTS / single-light LightProperties) is now free.
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
    checkSlot(ShaderBindingLayout::UBO_VIRTUAL_DRAW, "UBO_VIRTUAL_DRAW");
    checkSlot(ShaderBindingLayout::UBO_DEBUG_DRAW, "UBO_DEBUG_DRAW");               // #725
    checkSlot(ShaderBindingLayout::UBO_REFLECTION_PROBES, "UBO_REFLECTION_PROBES"); // #705
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
    // Distance-impostor reflection probe arrays (issue #705).
    checkSlot(ShaderBindingLayout::TEX_REFLECTION_PROBE_RADIANCE, "TEX_REFLECTION_PROBE_RADIANCE");
    checkSlot(ShaderBindingLayout::TEX_REFLECTION_PROBE_DISTANCE, "TEX_REFLECTION_PROBE_DISTANCE");
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
    checkSlot(ShaderBindingLayout::SSBO_FLUID_POSITIONS, "SSBO_FLUID_POSITIONS");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_VELOCITIES, "SSBO_FLUID_VELOCITIES");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_PREDICTED_A, "SSBO_FLUID_PREDICTED_A");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_PREDICTED_B, "SSBO_FLUID_PREDICTED_B");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_AUX, "SSBO_FLUID_AUX");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_GRID_HEAD, "SSBO_FLUID_GRID_HEAD");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_GRID_NEXT, "SSBO_FLUID_GRID_NEXT");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_COUNTERS, "SSBO_FLUID_COUNTERS");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_EMIT_STAGING, "SSBO_FLUID_EMIT_STAGING");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_BODY_PROXIES, "SSBO_FLUID_BODY_PROXIES");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_BODY_IMPULSES, "SSBO_FLUID_BODY_IMPULSES");
    checkSlot(ShaderBindingLayout::SSBO_FLUID_VELOCITIES_ALT, "SSBO_FLUID_VELOCITIES_ALT");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_CLUSTERS, "SSBO_VIRTUAL_CLUSTERS");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_GROUPS, "SSBO_VIRTUAL_GROUPS");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_INSTANCES, "SSBO_VIRTUAL_INSTANCES");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_DRAW_COMMANDS, "SSBO_VIRTUAL_DRAW_COMMANDS");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_DRAW_ARGS, "SSBO_VIRTUAL_DRAW_ARGS");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_VISIBLE, "SSBO_VIRTUAL_VISIBLE");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_VERTICES, "SSBO_VIRTUAL_VERTICES");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_SW_LIST, "SSBO_VIRTUAL_SW_LIST");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_VISBUFFER, "SSBO_VIRTUAL_VISBUFFER");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_INDICES, "SSBO_VIRTUAL_INDICES");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_GROUP_STATES, "SSBO_VIRTUAL_GROUP_STATES");
    checkSlot(ShaderBindingLayout::SSBO_VIRTUAL_REJECTED, "SSBO_VIRTUAL_REJECTED");
    checkSlot(ShaderBindingLayout::SSBO_RESOURCE_HEAP, "SSBO_RESOURCE_HEAP");
    // GPU-pushable shader debug draws (issue #725). Seven consecutive channels;
    // ShaderDebugDraw derives each one's binding as SSBO_DEBUG_DRAW_FIRST +
    // enumerator, so a collision here would silently point two channels at one
    // buffer — the entries of one primitive reinterpreted as another's.
    checkSlot(ShaderBindingLayout::SSBO_DEBUG_DRAW_LINE, "SSBO_DEBUG_DRAW_LINE");
    checkSlot(ShaderBindingLayout::SSBO_DEBUG_DRAW_CIRCLE, "SSBO_DEBUG_DRAW_CIRCLE");
    checkSlot(ShaderBindingLayout::SSBO_DEBUG_DRAW_RECTANGLE, "SSBO_DEBUG_DRAW_RECTANGLE");
    checkSlot(ShaderBindingLayout::SSBO_DEBUG_DRAW_AABB, "SSBO_DEBUG_DRAW_AABB");
    checkSlot(ShaderBindingLayout::SSBO_DEBUG_DRAW_BOX, "SSBO_DEBUG_DRAW_BOX");
    checkSlot(ShaderBindingLayout::SSBO_DEBUG_DRAW_CONE, "SSBO_DEBUG_DRAW_CONE");
    checkSlot(ShaderBindingLayout::SSBO_DEBUG_DRAW_SPHERE, "SSBO_DEBUG_DRAW_SPHERE");
    // Per-cluster reflection-probe bitmask (issue #705).
    checkSlot(ShaderBindingLayout::SSBO_REFLECTION_PROBE_GRID, "SSBO_REFLECTION_PROBE_GRID");

    // Terrain virtual texture (#715) and DDGI v2 (#707), added together because
    // they COLLIDED: both branches independently claimed 79/80 while in review,
    // and #715 merged first. Nothing caught it. The two blocks live in different
    // parts of ShaderBindingLayout.h, so git auto-merged them into a header with
    // two constants per slot and no textual conflict; this list is curated by
    // hand and neither family was in it; and the GLSL literals only disagree
    // with C++ once someone renumbers one side. The result would have been two
    // unrelated buffers bound to one slot -- terrain page-feedback writes
    // landing in the DDGI probe-aux buffer, which reads as "GI is subtly wrong"
    // and "the VT loop never converges", neither of them near the cause.
    //
    // The other SSBO families (VSM, prefix-sum, fluid alternates) are still
    // absent from this list. Adding them is worth doing, but is an audit rather
    // than a merge fix -- these two are here because this PR is where the
    // collision actually happened.
    checkSlot(ShaderBindingLayout::SSBO_TERRAIN_VT_FEEDBACK, "SSBO_TERRAIN_VT_FEEDBACK");
    checkSlot(ShaderBindingLayout::SSBO_TERRAIN_VT_BAKE, "SSBO_TERRAIN_VT_BAKE");
    checkSlot(ShaderBindingLayout::SSBO_TERRAIN_VT_INDIRECTION, "SSBO_TERRAIN_VT_INDIRECTION");
    checkSlot(ShaderBindingLayout::SSBO_DDGI_PROBE_AUX, "SSBO_DDGI_PROBE_AUX");
    checkSlot(ShaderBindingLayout::SSBO_DDGI_STATS, "SSBO_DDGI_STATS");
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
