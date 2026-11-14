// =============================================================================
// InstanceDataLayoutTest.cpp
//
// Pins the C++ <-> GLSL layout for the per-instance SSBO that powers GPU
// instancing. The struct lives at `OloEngine/Renderer/Instancing/InstanceData.h`
// and the matching GLSL block is emitted by
// `ShaderBindingLayout::GetInstanceSSBOLayout()`. Drift in either direction
// produces silent rendering bugs: wrong model matrices, scrambled normals,
// or stale entity IDs that break editor picking.
//
// What this test guards
// ---------------------
// 1. Total struct size is 256 bytes — divisible by 16 so std430 array stride
//    has no end padding.
// 2. Field offsets match the std430 layout assumed by shaders (see
//    ShaderBindingLayout::GetInstanceSSBOLayout()).
// 3. The SSBO binding constant has not been reassigned.
// 4. The GLSL layout string contains the field names shaders reference, so
//    a typo in the helper text fails a test instead of a shader.
//
// Classification: L1 / shaderpipe (pure CPU, no GL context needed).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    TEST(InstanceDataLayout, StructSizeMatchesStd430)
    {
        // 256 = 16 * 16, so a std430 array of InstanceData has stride 256.
        // A drift here means the shader's `instances[i]` reads from the wrong
        // memory offset — usually expressed as instances after the first
        // having garbage transforms.
        EXPECT_EQ(sizeof(InstanceData), 256u);
        EXPECT_EQ(sizeof(InstanceData) % 16u, 0u);
    }

    TEST(InstanceDataLayout, FieldOffsetsMatchGLSLBlock)
    {
        // Offsets must match the GLSL block emitted by
        // ShaderBindingLayout::GetInstanceSSBOLayout(). std430 layout:
        EXPECT_EQ(offsetof(InstanceData, Transform), 0u);
        EXPECT_EQ(offsetof(InstanceData, Normal), 64u);
        EXPECT_EQ(offsetof(InstanceData, PrevTransform), 128u);
        EXPECT_EQ(offsetof(InstanceData, Color), 192u);
        EXPECT_EQ(offsetof(InstanceData, EntityID), 208u);
        EXPECT_EQ(offsetof(InstanceData, Custom), 212u);
        EXPECT_EQ(offsetof(InstanceData, StableID), 216u);
        EXPECT_EQ(offsetof(InstanceData, LightmapScaleOffset), 224u);
        EXPECT_EQ(offsetof(InstanceData, GPUSceneRef), 240u);
    }

    TEST(InstanceDataLayout, DefaultsAreIdentityAndNeutral)
    {
        // Default-constructed instances must render as identity transforms
        // with no tint and no entity id. Anything else would be a footgun
        // for the auto-batching path that constructs InstanceData on the
        // fly from a MeshComponent + TransformComponent.
        InstanceData data{};
        EXPECT_EQ(data.EntityID, -1);
        EXPECT_EQ(data.Custom, 0.0f);
        EXPECT_EQ(data.StableID, 0u);
        // All-zero scale/offset is the "no lightmap" sentinel — a non-zero
        // default would make every non-lightmapped draw sample the atlas.
        EXPECT_EQ(data.LightmapScaleOffset.x, 0.0f);
        EXPECT_EQ(data.LightmapScaleOffset.y, 0.0f);
        EXPECT_EQ(data.LightmapScaleOffset.z, 0.0f);
        EXPECT_EQ(data.LightmapScaleOffset.w, 0.0f);
        // The canonical GPU Scene link defaults to "unlinked": generation zero
        // is the invalid generation, so a default-constructed instance can
        // never be mistaken for one that names a live record (issue #994).
        EXPECT_EQ(data.GPUSceneRef.y, GPUSceneDrawRefUnlinked);
        EXPECT_EQ(data.GPUSceneRef.w, GPUSceneDrawRefUnlinked);
        EXPECT_EQ(data.Color.x, 1.0f);
        EXPECT_EQ(data.Color.y, 1.0f);
        EXPECT_EQ(data.Color.z, 1.0f);
        EXPECT_EQ(data.Color.w, 1.0f);
        EXPECT_EQ(data.Transform[0][0], 1.0f);
        EXPECT_EQ(data.Transform[1][1], 1.0f);
        EXPECT_EQ(data.Transform[2][2], 1.0f);
        EXPECT_EQ(data.Transform[3][3], 1.0f);
    }

    TEST(InstanceDataLayout, BindingConstantIsStable)
    {
        // The binding is referenced from both C++ (InstanceBuffer ctor) and
        // GLSL (layout(std430, binding = 15) in GetInstanceSSBOLayout()).
        // A reassignment without updating the GLSL helper would silently
        // unbind the SSBO from the shader.
        EXPECT_EQ(ShaderBindingLayout::SSBO_INSTANCE_DATA, 15u);
    }

    TEST(InstancedMeshComponentDefaults, FieldsHaveSafeDefaults)
    {
        // Default-constructed component must be a safe no-op: empty instances
        // list (so the Scene loop skips it), flags consistent with the
        // single-mesh-many-transforms ergonomic, and culling on. A drift here
        // would silently change rendering for any entity that gets the
        // component via AddComponent<> without initialiser.
        InstancedMeshComponent imc{};
        EXPECT_TRUE(imc.Instances.IsEmpty());
        EXPECT_FALSE(static_cast<bool>(imc.MeshSource));
        EXPECT_FALSE(static_cast<bool>(imc.OverrideMaterial));
        EXPECT_TRUE(imc.FrustumCullPerInstance);
        EXPECT_TRUE(imc.CastShadows);
        EXPECT_EQ(imc.CullDistance, 0.0f);

        // Merge cache starts empty and matches no real input — first render
        // pass will populate it. Sentinel ensures InvalidateMergedCache works.
        EXPECT_EQ(imc._MergedCache.InlineSize, 0u);
        EXPECT_EQ(imc._MergedCache.InlineDataPtr, nullptr);
        EXPECT_EQ(imc._MergedCache.AssetSize, 0u);
        EXPECT_TRUE(imc._MergedCache.Data.IsEmpty());
        EXPECT_FALSE(imc._MergedCache.IgnoredAlphaReported);
        imc.InvalidateMergedCache();
        EXPECT_NE(imc._MergedCache.InlineSize, 0u) << "Invalidation should leave the size at a sentinel value distinct from any real inline size";
    }

    TEST(InstancedMeshComponentStableIDs, AssignmentRepairsLegacyAndDuplicateRecordsWithoutRenumberingLiveIDs)
    {
        std::vector<InstanceData> instances(3);
        instances[0].StableID = 9;
        instances[2].StableID = 9;

        EXPECT_TRUE(InstancedMeshComponent::EnsureStableIDs(instances));
        EXPECT_EQ(instances[0].StableID, 9u);
        EXPECT_NE(instances[1].StableID, 0u);
        EXPECT_NE(instances[2].StableID, 0u);
        EXPECT_NE(instances[1].StableID, instances[2].StableID);

        const u64 firstStableID = instances[0].StableID;
        const u64 lastStableID = instances[2].StableID;
        instances.insert(instances.begin() + 1, InstanceData{});

        EXPECT_TRUE(InstancedMeshComponent::EnsureStableIDs(instances));
        EXPECT_EQ(instances[0].StableID, firstStableID);
        EXPECT_EQ(instances[3].StableID, lastStableID);
        EXPECT_FALSE(InstancedMeshComponent::EnsureStableIDs(instances));
    }

    TEST(InstanceDataLayout, GLSLLayoutMentionsAllFieldsAndBinding)
    {
        const std::string_view glsl{ ShaderBindingLayout::GetInstanceSSBOLayout() };
        // Field names: a typo or rename in the helper string would otherwise
        // surface only when a migrated shader fails to compile.
        EXPECT_NE(glsl.find("Transform"), std::string_view::npos);
        EXPECT_NE(glsl.find("Normal"), std::string_view::npos);
        EXPECT_NE(glsl.find("PrevTransform"), std::string_view::npos);
        EXPECT_NE(glsl.find("Color"), std::string_view::npos);
        EXPECT_NE(glsl.find("EntityID"), std::string_view::npos);
        EXPECT_NE(glsl.find("Custom"), std::string_view::npos);
        EXPECT_NE(glsl.find("StableID"), std::string_view::npos);
        EXPECT_NE(glsl.find("LightmapScaleOffset"), std::string_view::npos);

        // Layout qualifier sanity: must be std430 + binding 15. Anything
        // else means the helper has drifted from the C++ constant or the
        // wrong layout qualifier was used (std140 would corrupt mat4 arrays).
        EXPECT_NE(glsl.find("std430"), std::string_view::npos);
        EXPECT_NE(glsl.find("binding = 15"), std::string_view::npos);
        EXPECT_NE(glsl.find("readonly buffer"), std::string_view::npos);
    }

    // An instance colour's alpha is not applied (issue #1691), so a producer
    // has to be able to say how many instances brought one. Opaque within a
    // tolerance is not reported; anything else is, a non-finite alpha included.
    TEST(InstanceDataLayout, AnAlphaOtherThanOneIsCountedAsIgnored)
    {
        std::vector<InstanceData> instances(6);
        EXPECT_EQ(CountInstancesWithIgnoredAlpha(instances), 0u) << "the default colour is opaque white";

        instances[1].Color.a = 0.25f;
        instances[2].Color.a = 0.0f;
        instances[3].Color.a = 1.5f;
        instances[4].Color.a = std::numeric_limits<f32>::quiet_NaN();
        instances[5].Color.a = 1.0f - 1.0e-6f; // float noise around 1 is not an authored alpha
        EXPECT_EQ(CountInstancesWithIgnoredAlpha(instances), 4u);

        // The colour itself is not what is counted.
        instances[0].Color = glm::vec4(0.2f, 0.4f, 0.6f, 1.0f);
        EXPECT_EQ(CountInstancesWithIgnoredAlpha(instances), 4u);
        EXPECT_EQ(CountInstancesWithIgnoredAlpha({}), 0u);
    }

    // A draw that cannot apply an instance colour reports it only when one was
    // authored: white, with any alpha, is not a tint.
    TEST(InstanceDataLayout, ATintIsAColourWhoseRgbIsNotWhite)
    {
        std::vector<InstanceData> instances(4);
        EXPECT_FALSE(AnyInstanceIsTinted(instances));
        EXPECT_FALSE(AnyInstanceIsTinted({}));

        instances[2].Color.a = 0.3f;
        EXPECT_FALSE(AnyInstanceIsTinted(instances)) << "alpha is not part of the tint";

        instances[3].Color.b = 0.5f;
        EXPECT_TRUE(AnyInstanceIsTinted(instances)) << "one tinted instance at the end of the batch is enough";

        instances[3].Color.b = 1.0f;
        instances[0].Color.r = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_TRUE(AnyInstanceIsTinted(instances)) << "a non-finite channel is not white";
    }

    // The tint a CPU consumer multiplies with (the GPU Scene record, the
    // lightmap bake) never carries a value a path tracer cannot take.
    TEST(InstanceDataLayout, TheTintACpuConsumerGetsIsFiniteAndNotNegative)
    {
        const glm::vec3 kept = InstanceTint(glm::vec4(0.25f, 0.5f, 2.0f, 0.1f));
        EXPECT_FLOAT_EQ(kept.r, 0.25f);
        EXPECT_FLOAT_EQ(kept.g, 0.5f);
        EXPECT_FLOAT_EQ(kept.b, 2.0f) << "a tint above 1 is a colour, not an error";

        const glm::vec3 clamped = InstanceTint(glm::vec4(-0.5f, 0.5f, 1.0f, 1.0f));
        EXPECT_FLOAT_EQ(clamped.r, 0.0f);
        EXPECT_FLOAT_EQ(clamped.g, 0.5f);

        for (const f32 bad : { std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity() })
        {
            const glm::vec3 white = InstanceTint(glm::vec4(0.2f, bad, 0.2f, 1.0f));
            EXPECT_FLOAT_EQ(white.r, 1.0f) << "one bad channel makes the whole colour white, as the loaders do";
            EXPECT_FLOAT_EQ(white.g, 1.0f);
            EXPECT_FLOAT_EQ(white.b, 1.0f);
        }
    }
} // namespace OloEngine::Tests
