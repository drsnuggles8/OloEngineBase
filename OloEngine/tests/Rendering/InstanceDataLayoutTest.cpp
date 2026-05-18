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
// 1. Total struct size is 224 bytes — divisible by 16 so std430 array stride
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
#include <string_view>

namespace OloEngine::Tests
{
    TEST(InstanceDataLayout, StructSizeMatchesStd430)
    {
        // 224 = 14 * 16, so a std430 array of InstanceData has stride 224.
        // A drift here means the shader's `instances[i]` reads from the wrong
        // memory offset — usually expressed as instances after the first
        // having garbage transforms.
        EXPECT_EQ(sizeof(InstanceData), 224u);
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
        EXPECT_TRUE(imc.Instances.empty());
        EXPECT_FALSE(static_cast<bool>(imc.MeshSource));
        EXPECT_FALSE(static_cast<bool>(imc.OverrideMaterial));
        EXPECT_TRUE(imc.FrustumCullPerInstance);
        EXPECT_TRUE(imc.CastShadows);
        EXPECT_EQ(imc.CullDistance, 0.0f);
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

        // Layout qualifier sanity: must be std430 + binding 15. Anything
        // else means the helper has drifted from the C++ constant or the
        // wrong layout qualifier was used (std140 would corrupt mat4 arrays).
        EXPECT_NE(glsl.find("std430"), std::string_view::npos);
        EXPECT_NE(glsl.find("binding = 15"), std::string_view::npos);
        EXPECT_NE(glsl.find("readonly buffer"), std::string_view::npos);
    }
} // namespace OloEngine::Tests
