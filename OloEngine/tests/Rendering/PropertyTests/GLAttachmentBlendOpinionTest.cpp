// OLO_TEST_LAYER: plumbing
// =============================================================================
// GLAttachmentBlendOpinionTest — the GL arm of the per-attachment blend
// tri-state (issue #896).
//
// The engine's contract is that a per-attachment blend opinion OUTRANKS the
// global enable in both directions and is withdrawn only by an explicit
// ResetBlendStateForAttachment. Raw GL does not work that way: glEnablei /
// glDisablei is the only way to speak to a draw buffer, there is no "unset",
// and glEnable(GL_BLEND) is DEFINED as glEnablei for every buffer — so on GL
// the contract exists only because OpenGLRendererAPI::SetBlendState re-asserts
// the standing opinions after the global call
// (ReassertAttachmentBlendOpinions).
//
// Vulkan's arm of the same contract is pinned by
// VulkanDrawPath.PerAttachmentBlendOpinionOutranksTheGlobalEnable, which reads
// the composed result out of rendered pixels. This one asks the GL state
// machine directly with glIsEnabledi, because on GL the driver's per-buffer
// enable IS the state under test — there is no recorded array to lower, and a
// pixel probe would only re-test blending.
//
// A withdrawal has to put the draw buffer back on the GLOBAL enable, and the
// backend reads that from its own mirror because GL will not answer the
// question. `glIsEnabled(GL_BLEND)` is documented as the index-0 value and does
// not behave as one: measured here on NVIDIA (RTX 4090, driver 98.352.0), after
// glDisable(GL_BLEND) + glEnablei(GL_BLEND, 1) it returns TRUE while
// glIsEnabledi(GL_BLEND, 0) returns FALSE — i.e. "some index is on", not the
// global. An earlier revision of this change queried it and this test is what
// caught that, which is the reason the assertions below read the driver with
// the INDEXED query throughout.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Commands/RenderCommand.h" // MAX_MASKED_COLOR_ATTACHMENTS
#include "OloEngine/Renderer/RenderCommand.h"

#include <gtest/gtest.h>

#include <glad/gl.h>

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] bool BlendEnabledFor(const u32 drawBuffer)
        {
            return ::glIsEnabledi(GL_BLEND, drawBuffer) == GL_TRUE;
        }
    } // namespace

    // The fixture exists only to guarantee Renderer::Init has run, so the GL
    // backend knows GL_MAX_DRAW_BUFFERS — the per-attachment entry points
    // reject every index until it does. No scene is needed: this test drives
    // the facade directly and reads the driver back.
    class GLAttachmentBlendOpinion : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // Intentionally empty — see the class comment.
        }
    };

    TEST_F(GLAttachmentBlendOpinion, OpinionOutranksTheGlobalEnableAndWithdrawalRestoresIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RendererAPI& api = RenderCommand::GetRendererAPI();

        // Opinions outlive the pass that stated them and this backend is
        // process-global, so the baseline below cannot assume it starts clean —
        // an earlier test in this binary leaving a ForceOff on 0 or 1 would
        // fail the control before the real scenario was ever reached. Clear
        // first, then establish the baseline. (The cleanup at the end of this
        // test protects the NEXT test; this protects THIS one.)
        for (u32 attachment = 0; attachment < MAX_MASKED_COLOR_ATTACHMENTS; ++attachment)
            api.ResetBlendStateForAttachment(attachment);

        // Baseline: nothing has an opinion, so every draw buffer follows the
        // global call. This is the control for everything below — if it fails,
        // no later assertion means anything.
        api.SetBlendState(true);
        EXPECT_TRUE(BlendEnabledFor(0)) << "with no opinion standing, draw buffer 0 must follow SetBlendState(true)";
        EXPECT_TRUE(BlendEnabledFor(1)) << "with no opinion standing, draw buffer 1 must follow SetBlendState(true)";

        // ForceOff survives a global ENABLE — the #896 arm.
        // OITResolveRenderPass asks for exactly this on its entity-ID and
        // view-normal targets.
        api.SetBlendStateForAttachment(1, false);
        EXPECT_FALSE(BlendEnabledFor(1)) << "the indexed disable did not reach the driver at all";
        api.SetBlendState(true); // the flatten that used to erase it
        EXPECT_TRUE(BlendEnabledFor(0)) << "draw buffer 0 has no opinion and must still follow the global enable";
        EXPECT_FALSE(BlendEnabledFor(1))
            << "a per-attachment DISABLE must outrank a later global SetBlendState(true) — glEnable(GL_BLEND) is "
               "glEnablei for every buffer, so SetBlendState has to re-assert the standing opinions (issue #896)";

        // ForceOn survives a global DISABLE — the #823 arm, which
        // DecalRenderPass's Emissive additive accumulation rides on.
        api.SetBlendStateForAttachment(1, true);
        api.SetBlendState(false);
        EXPECT_FALSE(BlendEnabledFor(0)) << "draw buffer 0 has no opinion and must follow the global disable";
        EXPECT_TRUE(BlendEnabledFor(1))
            << "a per-attachment ENABLE must outrank a later global SetBlendState(false) — this is the enable "
               "DecalRenderPass installs for an Emissive decal whose PODRenderState carries blendEnabled=false";

        // Withdrawal is the only way out of either state, and puts the buffer
        // back on the global flag — in both directions.
        api.ResetBlendStateForAttachment(1);
        EXPECT_FALSE(BlendEnabledFor(1)) << "after withdrawal, draw buffer 1 must follow the global disable";
        api.SetBlendState(true);
        EXPECT_TRUE(BlendEnabledFor(1))
            << "after withdrawal, draw buffer 1 must follow the global enable too — a withdrawal that only "
               "half-worked would leave the old opinion standing here";

        // Withdrawing an attachment that never had an opinion is a no-op, and
        // withdrawing one does not disturb another's. Both are load-bearing:
        // RGCommandContext::ResetGraphicsStateToDefault withdraws all eight
        // attachments unconditionally, most of which no pass ever touched, and
        // the pass restore blocks withdraw their own indices one at a time
        // while a sibling pass's opinion may still stand.
        api.SetBlendState(false);
        api.SetBlendStateForAttachment(1, true);
        api.ResetBlendStateForAttachment(2); // never given an opinion
        EXPECT_FALSE(BlendEnabledFor(2)) << "withdrawing an untouched attachment must leave it on the global";
        EXPECT_TRUE(BlendEnabledFor(1))
            << "withdrawing attachment 2 disturbed attachment 1's standing opinion — withdrawals are per-index";

        // Leave nothing standing for the next test in this process — the leak
        // the tri-state makes possible, and the reason every pass withdraws.
        for (u32 attachment = 0; attachment < MAX_MASKED_COLOR_ATTACHMENTS; ++attachment)
            api.ResetBlendStateForAttachment(attachment);
        api.SetBlendState(false);
    }
} // namespace OloEngine::Tests
