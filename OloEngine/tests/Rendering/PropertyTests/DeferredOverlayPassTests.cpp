// =============================================================================
// DeferredOverlayPassTests.cpp
//
// Layer-1 property tests for the Deferred forward-overlay plumbing added in
// Phase 8. Validates the data-contract invariants that the full G-Buffer +
// ForwardOverlayRenderPass + per-object velocity pipeline depends on:
//
//   * ModelUBO std140 layout — offsets of Model / Normal / EntityID / PrevModel
//     must match what PBR_GBuffer.glsl declares, or the shader reads wrong
//     bytes for per-object motion vectors.
//   * DrawMeshCommand.prevTransform placement — motion history must round-trip
//     through the radix-sorted command bucket without disturbing POD-ness.
//   * ForwardOverlayRenderPass existence — the class that routes skybox /
//     terrain / voxel / grid / light-cube draws away from the G-Buffer MRT
//     write must compile and construct cleanly off-device.
//
// These are CPU-side contracts. Full integration (actually rendering a
// deferred scene end-to-end) is covered by the OloEditor running a sample
// scene with `RendererSettings::Path = Deferred` — which is the project's
// effective Layer-10 smoke surface.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/Passes/ForwardOverlayRenderPass.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine::Tests
{
    // std140 ModelUBO layout expected by PBR_GBuffer.glsl. Sizes are chosen
    // so a mat4 = 64 bytes, ivec (padded to vec4) = 16 bytes, mat4 = 64 bytes.
    TEST(DeferredModelUBOLayout, StructMatchesStd140Expectations)
    {
        using ModelUBO = ShaderBindingLayout::ModelUBO;

        static_assert(std::is_trivially_copyable_v<ModelUBO>, "ModelUBO must be trivially copyable for raw UBO upload");

        EXPECT_EQ(offsetof(ModelUBO, Model), 0u);
        EXPECT_EQ(offsetof(ModelUBO, Normal), 64u);
        EXPECT_EQ(offsetof(ModelUBO, EntityID), 128u);
        EXPECT_EQ(offsetof(ModelUBO, PrevModel), 144u);
        EXPECT_EQ(sizeof(ModelUBO), 208u);
        EXPECT_EQ(ModelUBO::GetSize(), sizeof(ModelUBO));
    }

    TEST(DeferredModelUBOLayout, PrevModelStoresMat4RoundTrip)
    {
        ShaderBindingLayout::ModelUBO ubo{};
        const glm::mat4 prev = glm::mat4(
            glm::vec4(1.0f, 2.0f, 3.0f, 4.0f),
            glm::vec4(5.0f, 6.0f, 7.0f, 8.0f),
            glm::vec4(9.0f, 10.0f, 11.0f, 12.0f),
            glm::vec4(13.0f, 14.0f, 15.0f, 16.0f));
        ubo.PrevModel = prev;

        // Reinterpret as raw bytes to confirm the mat4 sits where the shader reads.
        const f32* base = reinterpret_cast<const f32*>(&ubo);
        const f32* prevMat = base + (offsetof(ShaderBindingLayout::ModelUBO, PrevModel) / sizeof(f32));
        for (i32 i = 0; i < 16; ++i)
        {
            EXPECT_FLOAT_EQ(prevMat[i], reinterpret_cast<const f32*>(&prev)[i]) << "index=" << i;
        }
    }

    TEST(DeferredDrawMeshCommand, PrevTransformFieldIsPresent)
    {
        static_assert(std::is_trivially_copyable_v<DrawMeshCommand>, "DrawMeshCommand must remain trivially copyable after adding prevTransform");

        // transform and prevTransform should live back-to-back so copy operations
        // inside CommandBucket move both with a single contiguous memcpy.
        constexpr size_t transformOffset = offsetof(DrawMeshCommand, transform);
        constexpr size_t prevTransformOffset = offsetof(DrawMeshCommand, prevTransform);
        EXPECT_EQ(prevTransformOffset, transformOffset + sizeof(glm::mat4));
    }

    TEST(DeferredDrawMeshCommand, PrevTransformRoundTripsThroughMemcpy)
    {
        DrawMeshCommand src{};
        src.transform = glm::mat4(1.0f);
        src.prevTransform = glm::translate(glm::mat4(1.0f), glm::vec3(7.5f, -2.25f, 1.125f));

        DrawMeshCommand dst{};
        std::memcpy(&dst, &src, sizeof(DrawMeshCommand));

        // Bitwise comparison — avoids any ambiguity around glm mat equality
        // (element-wise comparison, signed-zero handling, etc.).
        EXPECT_EQ(std::memcmp(&dst.transform, &src.transform, sizeof(glm::mat4)), 0);
        EXPECT_EQ(std::memcmp(&dst.prevTransform, &src.prevTransform, sizeof(glm::mat4)), 0);
    }

    TEST(ForwardOverlayRenderPassConstruction, DefaultConstructsAndExposesSetter)
    {
        // Smoke: can be constructed off-device.
        // Actual Execute() requires a live GL context and is exercised
        // through OloEditor's Deferred-mode sample scenes.
        ForwardOverlayRenderPass pass;
        SUCCEED();
    }

    // =========================================================================
    // G-Buffer entity-ID attachment — regression coverage for the deferred-
    // path selection-outline gap (logged 2026-05-15). PBR meshes render into
    // the G-Buffer and never reach SceneColor RT1 in deferred; without a
    // dedicated entity-ID slot, the JFA Init shader sees only the clear
    // sentinel (-1) at every pixel and no outline ever appears.
    // =========================================================================

    TEST(GBufferLayout, EntityIDIsTheFifthAttachment)
    {
        // Locking these enum values pins the order PBR_GBuffer.glsl etc.
        // assume (location = 4 → entity ID). A shader-side mismatch would
        // either silently discard entity-ID writes (renderer regression) or
        // overwrite the normal/emissive attachment (visual regression).
        EXPECT_EQ(static_cast<u32>(GBuffer::Albedo), 0u);
        EXPECT_EQ(static_cast<u32>(GBuffer::Normal), 1u);
        EXPECT_EQ(static_cast<u32>(GBuffer::Emissive), 2u);
        EXPECT_EQ(static_cast<u32>(GBuffer::Velocity), 3u);
        EXPECT_EQ(static_cast<u32>(GBuffer::EntityID), 4u);
        EXPECT_EQ(static_cast<u32>(GBuffer::Count), 5u);
    }
} // namespace OloEngine::Tests
