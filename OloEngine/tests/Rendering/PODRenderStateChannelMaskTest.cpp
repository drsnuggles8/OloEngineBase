#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: plumbing

#include "RenderingTestUtils.h"
#include "MockRendererAPI.h"
#include "FrameDataBufferFixture.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"

#include <optional>

using namespace OloEngine;          // NOLINT(google-build-using-namespace)
using namespace OloEngine::Testing; // NOLINT(google-build-using-namespace)

// ApplyPODRenderState reads the render-state table, so the manager has to be
// up. FrameDataBufferFixture owns that lifecycle without dragging in
// CommandDispatch::Initialize() — which is the reason
// Rendering/CommandDispatchTest.cpp is commented out of tests/CMakeLists.txt
// ("pulls in OpenGL statics"). Nothing here needs those.
class ChannelMaskTest : public FrameDataBufferFixture
{
};

// =============================================================================
// Per-attachment CHANNEL masks (issue #853)
//
// These drive the REAL CommandDispatch::ApplyPODRenderState against the mock
// and assert the exact indexed colour-mask calls it issues. That is the whole
// defect surface: the pass installs channel-level masks, ApplyPODRenderState
// then issues the GLOBAL SetColorMask (the indexed call for every draw buffer
// on both backends) and, before this change, could only re-disable WHOLE
// attachments — so a decal wrote every channel of the attachments its draw map
// selected.
//
// Backend-independent on purpose: the Vulkan pass suite pins this end-to-end on
// real pixels, but only on Vulkan, and the same flattening happened on OpenGL.
// The composition rule itself lives in one place and is testable without a GPU.
// =============================================================================

namespace
{
    // The channel nibble ApplyPODRenderState last pushed to `attachment`, or
    // std::nullopt if it issued no indexed call for it (meaning: the global
    // SetColorMask's value stands).
    std::optional<u32> LastAttachmentMask(const MockRendererAPI& api, u32 attachment)
    {
        std::optional<u32> found;
        for (const auto& call : api.GetRecordedCalls())
        {
            if (call.Name == "SetColorMaskForAttachment" && call.ParamU32_0 == attachment)
                found = call.ParamU32_1;
        }
        return found;
    }

    u32 ApplyAndReturnGlobalMask(const PODRenderState& state, MockRendererAPI& api)
    {
        CommandDispatch::InvalidateRenderStateCache();
        const u16 index = FrameDataBufferManager::Get().AllocateRenderState(state);
        CommandDispatch::ApplyPODRenderState(index, api);
        u32 global = 0xFu;
        for (const auto& call : api.GetRecordedCalls())
        {
            if (call.Name == "SetColorMask")
                global = call.ParamU32_1;
        }
        return global;
    }
} // namespace

// The default state must issue NO indexed call at all. This is the control: a
// loop that unconditionally re-asserted every attachment would satisfy every
// assertion below while costing eight redundant driver calls on every draw in
// the engine.
TEST_F(ChannelMaskTest, DefaultStateIssuesNoPerAttachmentCalls)
{
    MockRendererAPI api;

    PODRenderState state = CreateDefaultPODRenderState();
    ApplyAndReturnGlobalMask(state, api);

    EXPECT_EQ(api.CountCalls("SetColorMaskForAttachment"), 0u)
        << "a fully-writable state must not narrow anything per attachment";
}

// The pre-#853 behaviour, unchanged: an attachment excluded by the bit mask is
// disabled outright. Renderer3DUtilityDraws' skeleton/joint (0x01) and infinite
// grid (0xFF & ~(1 << 2)) draws depend on exactly this.
TEST_F(ChannelMaskTest, AttachmentWriteMaskStillDisablesWholeAttachments)
{
    MockRendererAPI api;

    PODRenderState state = CreateDefaultPODRenderState();
    state.colorAttachmentWriteMask = 0x01; // the DrawLine / skeleton value
    ApplyAndReturnGlobalMask(state, api);

    EXPECT_FALSE(LastAttachmentMask(api, 0).has_value())
        << "attachment 0 is enabled, so the global call already covers it";
    for (u32 attachment = 1; attachment < 8u; ++attachment)
    {
        const auto mask = LastAttachmentMask(api, attachment);
        ASSERT_TRUE(mask.has_value()) << "attachment " << attachment << " must be disabled";
        EXPECT_EQ(*mask, 0u) << "a masked-out attachment writes no channel";
    }
}

// The fix: a channel-level mask survives the global SetColorMask, per attachment.
TEST_F(ChannelMaskTest, ChannelMaskIsReassertedAfterTheGlobalCall)
{
    MockRendererAPI api;

    PODRenderState state = CreateDefaultPODRenderState();
    // RT1: xy writable, zw preserved. RT2: rgb writable, a preserved.
    state.colorAttachmentChannelMask =
        WithColorChannelMask(WithColorChannelMask(COLOR_CHANNEL_MASK_ALL, 1u,
                                                  MakeColorChannelMask(true, true, false, false)),
                             2u, MakeColorChannelMask(true, true, true, false));

    const u32 global = ApplyAndReturnGlobalMask(state, api);
    EXPECT_EQ(global, 0xFu) << "the global call still opens every channel — that is what has to be narrowed back";

    EXPECT_EQ(LastAttachmentMask(api, 1).value_or(0xFu), MakeColorChannelMask(true, true, false, false));
    EXPECT_EQ(LastAttachmentMask(api, 2).value_or(0xFu), MakeColorChannelMask(true, true, true, false));
    EXPECT_FALSE(LastAttachmentMask(api, 0).has_value()) << "untouched attachments cost no indexed call";
    EXPECT_FALSE(LastAttachmentMask(api, 3).has_value());
}

// Composition is an AND: the per-attachment nibble narrows the global mask and
// can never widen past it. A widen would let a command write a channel the
// global call had just closed.
TEST_F(ChannelMaskTest, ChannelMaskNarrowsTheGlobalMaskAndNeverWidensIt)
{
    MockRendererAPI api;

    PODRenderState state = CreateDefaultPODRenderState();
    state.colorMaskA = false; // global closes alpha
    // RT1 asks for alpha back; it must NOT get it.
    state.colorAttachmentChannelMask =
        WithColorChannelMask(COLOR_CHANNEL_MASK_ALL, 1u, MakeColorChannelMask(false, false, false, true));

    ApplyAndReturnGlobalMask(state, api);

    const auto mask = LastAttachmentMask(api, 1);
    ASSERT_TRUE(mask.has_value());
    EXPECT_EQ(*mask, 0u) << "alpha-only nibble AND alpha-closed global == nothing writable";
}

// The routing table itself, so the four modes' channel ownership is pinned
// somewhere cheap and readable. DecalRenderPass and Renderer3D::DrawDecal both
// read this function; if it drifts, both drift together and only this fails.
TEST_F(ChannelMaskTest, DecalModeChannelRoutingMatchesTheGBufferLayout)
{
    using Mode = DrawDecalCommand::DecalMode;

    const auto rt = [](Mode mode, u32 attachment)
    { return GetColorChannelMask(DecalGBufferChannelMask(mode), attachment); };

    // Albedo: RT0.rgb, RT0.a (metallic) preserved.
    EXPECT_EQ(rt(Mode::Albedo, 0u), MakeColorChannelMask(true, true, true, false));
    // Normal: RT1.xy (oct normal), RT1.zw (roughness, AO) preserved.
    EXPECT_EQ(rt(Mode::Normal, 1u), MakeColorChannelMask(true, true, false, false));
    EXPECT_EQ(rt(Mode::Normal, 0u), 0u) << "Normal mode owns no RT0 channel";
    // RMA: RT0.a (metallic) + RT1.zw (roughness, AO).
    EXPECT_EQ(rt(Mode::RMA, 0u), MakeColorChannelMask(false, false, false, true));
    EXPECT_EQ(rt(Mode::RMA, 1u), MakeColorChannelMask(false, false, true, true));
    // Emissive: RT2.rgb, RT2.a (the deferred unlit flag) preserved.
    EXPECT_EQ(rt(Mode::Emissive, 2u), MakeColorChannelMask(true, true, true, false));

    // RT4 is the R32I entity id and is in no mode's draw map: every mode must
    // leave it fully writable so no indexed call is issued for it at all.
    for (const Mode mode : { Mode::Albedo, Mode::Normal, Mode::RMA, Mode::Emissive })
    {
        EXPECT_EQ(rt(mode, 4u), 0xFu) << "a decal mode must not touch RT4's colour mask";
        EXPECT_EQ(rt(mode, 7u), 0xFu);
    }
}

// The production decal state carries the routing ONLY on the deferred path.
// On the forward path every mode collapses to the transparent albedo overlay
// drawn into scene colour (or the WB-OIT accum/revealage MRT), where the
// G-Buffer channel routing is meaningless and masking RT1/RT2 would break OIT
// compositing outright.
TEST_F(ChannelMaskTest, ForwardPathDecalStateCarriesNoChannelMask)
{
    using Mode = DrawDecalCommand::DecalMode;

    for (const Mode mode : { Mode::Albedo, Mode::Normal, Mode::RMA, Mode::Emissive })
    {
        EXPECT_EQ(CreateDecalPODRenderState(mode, /*deferredPath=*/false).colorAttachmentChannelMask,
                  COLOR_CHANNEL_MASK_ALL)
            << "a forward-path decal must not narrow any channel";
        EXPECT_EQ(CreateDecalPODRenderState(mode, /*deferredPath=*/true).colorAttachmentChannelMask,
                  DecalGBufferChannelMask(mode))
            << "a deferred decal must carry its mode's routing";
    }
}

// PODRenderState's equality is field-wise and hand-written, so a new field that
// is not added to it makes two different states compare equal — which would let
// FrameDataBufferManager hand back a cached index for a DIFFERENT mask and
// silently reinstate the bug this change fixes.
TEST_F(ChannelMaskTest, ChannelMaskParticipatesInPODRenderStateEquality)
{
    PODRenderState a = CreateDefaultPODRenderState();
    PODRenderState b = a;
    EXPECT_TRUE(a == b);

    b.colorAttachmentChannelMask =
        WithColorChannelMask(COLOR_CHANNEL_MASK_ALL, 0u, MakeColorChannelMask(true, true, true, false));
    EXPECT_FALSE(a == b) << "colorAttachmentChannelMask is missing from PODRenderState::operator==";
}
