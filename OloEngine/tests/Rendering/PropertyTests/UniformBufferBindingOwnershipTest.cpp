// OLO_TEST_LAYER: L4
// =============================================================================
// UniformBufferBindingOwnershipTest.cpp — who owns a UBO binding point, and
// what `Unbind()` is allowed to do about it.
//
// THE RULE. A `UniformBuffer` claims its binding point when it is CONSTRUCTED
// and nothing rebinds it afterwards, so two buffers on one binding point is
// last-created-wins and `SetData` on the loser lands somewhere no shader reads.
// That is written up in notes-renderer.md; it cost a wrong frame on #1040
// before an end-to-end comparison caught it.
//
// This file pins the half of that rule which is easy to get wrong in the other
// direction: `Unbind()` must clear the slot ONLY when the caller still owns it.
// An unconditional clear lets a stale buffer evict the buffer that legitimately
// owns the binding now, and the symptom is a later pass reading zeroes — a
// frame that renders, with a degenerate camera, rather than an error.
//
// Both halves are asserted through the public facade only, so the test is a
// statement about the RHI contract rather than about OpenGL.
//
// Classification: L4 (GPU state validation — UBO binding leaks across passes).
// SKIPs cleanly with no GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/UniformBuffer.h"
#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <array>

namespace OloEngine::Tests
{
    namespace
    {
        // The binding this test plays with. UBO_USER_1 is a pass-local slot, so
        // borrowing it cannot disturb a camera or material block that some
        // other test in this process left bound.
        constexpr u32 kScratchBinding = 8;

        [[nodiscard]] u32 CurrentlyBoundAt(u32 binding)
        {
            GLint bound = 0;
            ::glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, binding, &bound);
            return static_cast<u32>(bound);
        }
    } // namespace

    TEST(UniformBufferBindingOwnership, ConstructionClaimsTheBindingPoint)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const u32 before = CurrentlyBoundAt(kScratchBinding);
        {
            Ref<UniformBuffer> buffer = UniformBuffer::Create(64, kScratchBinding);
            ASSERT_TRUE(buffer);
            // This is the surprising half of the rule, stated as a test so it
            // stops being folklore: creating a UBO binds it, no explicit Bind
            // required.
            EXPECT_EQ(CurrentlyBoundAt(kScratchBinding), buffer->GetRendererID());
            buffer->Unbind();
        }
        EXPECT_EQ(CurrentlyBoundAt(kScratchBinding), 0u) << "the scratch binding was left occupied";
        (void)before;
    }

    TEST(UniformBufferBindingOwnership, UnbindingAStaleBufferDoesNotEvictTheCurrentOwner)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Ref<UniformBuffer> older = UniformBuffer::Create(64, kScratchBinding);
        ASSERT_TRUE(older);
        Ref<UniformBuffer> newer = UniformBuffer::Create(64, kScratchBinding);
        ASSERT_TRUE(newer);

        // Last created wins.
        ASSERT_EQ(CurrentlyBoundAt(kScratchBinding), newer->GetRendererID())
            << "construction did not claim the binding point from the older buffer";

        // The older buffer tidying up after itself must not take the slot away
        // from the newer one. An unconditional glBindBufferBase(..., 0) here is
        // the bug: every later draw on this binding would read zeroes.
        older->Unbind();
        EXPECT_EQ(CurrentlyBoundAt(kScratchBinding), newer->GetRendererID())
            << "a stale buffer's Unbind evicted the binding point's real owner";

        // And the owner can still release its own slot.
        newer->Unbind();
        EXPECT_EQ(CurrentlyBoundAt(kScratchBinding), 0u);
    }

    TEST(UniformBufferBindingOwnership, RebindingRestoresOwnershipAfterADisplacement)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Ref<UniformBuffer> first = UniformBuffer::Create(64, kScratchBinding);
        Ref<UniformBuffer> second = UniformBuffer::Create(64, kScratchBinding);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);

        // The documented remedy for the displacement: bind explicitly before
        // the draw that reads you. This is what GpuViewOrdering and the splat
        // evidence test both do, and why they stopped disagreeing about the
        // camera.
        first->Bind();
        EXPECT_EQ(CurrentlyBoundAt(kScratchBinding), first->GetRendererID());

        second->Bind();
        EXPECT_EQ(CurrentlyBoundAt(kScratchBinding), second->GetRendererID());

        second->Unbind();
        EXPECT_EQ(CurrentlyBoundAt(kScratchBinding), 0u);
        // `first` never owned the slot at the end, so its Unbind is a no-op
        // rather than a second clear.
        first->Unbind();
        EXPECT_EQ(CurrentlyBoundAt(kScratchBinding), 0u);
    }
} // namespace OloEngine::Tests
