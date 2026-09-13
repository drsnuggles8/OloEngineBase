// =============================================================================
// FoliageGBufferRoutingTest.cpp
//
// Pins which framebuffer each foliage variant is drawn into (issue #1225).
//
// The bug this exists to stop coming back: the octahedral impostor card (#433)
// was routed to FoliageRenderPass, a forward overlay that runs AFTER
// DeferredLightingPass and writes only SceneColor. A mesh-foliage canopy
// therefore appeared in Beauty and in the shadow cascades but was ABSENT from
// GBufferAlbedo and GBufferNormal, so every G-Buffer-derived term — SSAO, SSGI,
// SSR — treated the canopy as empty sky.
//
// That is invisible in a beauty shot, which is exactly why it survived: the
// canopy renders, it just renders in the wrong pass. So the contract is pinned
// here on the stream-selection rule itself rather than left to a screenshot.
//
// Two halves:
//
//   1. The routing rule. SelectFoliageRenderStream's own static_asserts already
//      pin it at compile time; these cases restate the ones a future edit is
//      most likely to get wrong, so a regression names itself in the test log
//      instead of only failing to build.
//   2. The shader interface. A shader drawn into the G-Buffer must declare the
//      full MRT output set, and a shader drawn into the forward Scene MRT must
//      NOT — mixing them writes a float output onto the R32_SINT entity-ID
//      target (the #955 class of bug). ShaderStageContractTest reflects the
//      real SPIR-V; this asserts the cheaper source-level property that the
//      deferred variant exists at all and that the two impostor shaders stay
//      the same card.
//
// OLO_TEST_LAYER: unit
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] std::string ReadShader(const char* name)
        {
            const fs::path path = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / name;
            std::ifstream in(path);
            if (!in)
                return {};
            std::ostringstream oss;
            oss << in.rdbuf();
            return oss.str();
        }

        /// The `#type fragment` section only — the vertex stage has its own
        /// `out` declarations and would satisfy any output assertion by accident.
        [[nodiscard]] std::string FragmentStageOf(const std::string& source)
        {
            const auto fs_ = source.find("#type fragment");
            if (fs_ == std::string::npos)
                return {};
            return source.substr(fs_);
        }

        [[nodiscard]] std::string VertexStageOf(const std::string& source)
        {
            const auto vs = source.find("#type vertex");
            if (vs == std::string::npos)
                return {};
            const auto fsStart = source.find("#type fragment", vs);
            return source.substr(vs, (fsStart == std::string::npos) ? std::string::npos : fsStart - vs);
        }

        [[nodiscard]] bool Contains(const std::string& haystack, const char* needle)
        {
            return haystack.find(needle) != std::string::npos;
        }
    } // namespace

    // ── The G-Buffer variant exists and writes the full MRT set ──────────

    TEST(FoliageGBufferRouting, ImpostorHasADeferredVariantWritingEveryGBufferTarget)
    {
        const std::string source = ReadShader("Foliage_Impostor_GBuffer.glsl");
        ASSERT_FALSE(source.empty()) << "Foliage_Impostor_GBuffer.glsl is missing — the impostor canopy "
                                        "has no deferred route and cannot reach the G-Buffer";

        const std::string frag = FragmentStageOf(source);
        ASSERT_FALSE(frag.empty());

        // The full writer set. Location 5 (BakedGI) matters even though foliage
        // carries no lightmap: an MRT output a shader never writes is undefined
        // in that attachment, and RT5's alpha is read as a "has baked GI" flag
        // (issue #865).
        EXPECT_TRUE(Contains(frag, "o_GBufferAlbedo"));
        EXPECT_TRUE(Contains(frag, "o_GBufferNormal"));
        EXPECT_TRUE(Contains(frag, "o_GBufferEmissive"));
        EXPECT_TRUE(Contains(frag, "o_GBufferVelocity"));
        EXPECT_TRUE(Contains(frag, "o_GBufferEntityID"));
        EXPECT_TRUE(Contains(frag, "o_GBufferBakedGI"));

        // It must NOT also carry the forward path's single lit output — a shader
        // declaring both interfaces is the one that gets drawn into the wrong
        // framebuffer without anyone noticing.
        EXPECT_FALSE(Contains(frag, "out vec4 FragColor"))
            << "the deferred variant still declares the forward lit output";

        // Deferred lighting owns shading now, so the card must not relight
        // itself from the single-light UBO.
        EXPECT_FALSE(Contains(frag, "MultiLightData"))
            << "the deferred variant still relights itself; DeferredLightingPass shades the G-Buffer";
    }

    TEST(FoliageGBufferRouting, ForwardImpostorKeepsTheLitOutputAndNoGBufferTargets)
    {
        const std::string frag = FragmentStageOf(ReadShader("Foliage_Impostor.glsl"));
        ASSERT_FALSE(frag.empty());

        // The forward card composites into SceneColor after deferred lighting,
        // so it relights itself and writes one colour + velocity.
        EXPECT_TRUE(Contains(frag, "out vec4 FragColor"));
        EXPECT_FALSE(Contains(frag, "o_GBufferAlbedo"))
            << "the forward card declares a G-Buffer output — executing it against the Scene MRT "
               "maps a float output onto the R32_SINT entity-ID target (issue #955)";
    }

    // ── The two impostor variants must stay the same card ────────────────

    TEST(FoliageGBufferRouting, BothImpostorVariantsSampleTheSameAtlasTheSameWay)
    {
        const std::string forward = ReadShader("Foliage_Impostor.glsl");
        const std::string deferred = ReadShader("Foliage_Impostor_GBuffer.glsl");
        ASSERT_FALSE(forward.empty());
        ASSERT_FALSE(deferred.empty());

        // Same octahedral machinery on both sides. If one gains a sampling
        // change the other does not, the card moves or samples a different
        // frame at the Forward/Deferred seam — a difference no single-path
        // capture can see.
        for (const char* marker : { "OctahedralImpostor.glsl", "u_NormalDepthAtlas", "u_AlbedoAtlas",
                                    "OctaDirToGrid", "OctaFrameToDir", "u_ImpostorParams0", "u_ImpostorParams1" })
        {
            EXPECT_TRUE(Contains(forward, marker)) << "forward variant lost: " << marker;
            EXPECT_TRUE(Contains(deferred, marker)) << "deferred variant lost: " << marker;
        }

        // Both must consume the same two pull streams on Vulkan, or the card
        // reads a different instance stream in one path.
        for (const std::string& stage : { VertexStageOf(forward), VertexStageOf(deferred) })
        {
            ASSERT_FALSE(stage.empty());
            EXPECT_TRUE(Contains(stage, "binding = 57"));
            EXPECT_TRUE(Contains(stage, "binding = 63"));
            EXPECT_TRUE(Contains(stage, "OLO_INSTANCE_SINGLE"));
        }

        // The deferred fragment reads u_EntityID out of the instance SSBO, so
        // its vertex stage must forward the instance index the forward variant
        // deliberately suppresses.
        EXPECT_TRUE(Contains(VertexStageOf(forward), "OLO_INSTANCE_NO_FORWARD"));
        EXPECT_FALSE(Contains(VertexStageOf(deferred), "OLO_INSTANCE_NO_FORWARD"))
            << "the deferred variant suppresses v_InstanceIndex but its fragment reads u_EntityID";
        EXPECT_TRUE(Contains(FragmentStageOf(deferred), "InstanceBlock.glsl"));
    }

    // ── Neither foliage path may be alpha-blended ────────────────────────

    TEST(FoliageGBufferRouting, ImpostorIsAlphaTestedNotAlphaBlended)
    {
        // This is the property that makes the G-Buffer route legal at all: an
        // alpha-BLENDED surface cannot do an MRT G-Buffer write, an
        // alpha-TESTED one can. Both variants must discard, not blend.
        for (const char* name : { "Foliage_Impostor.glsl", "Foliage_Impostor_GBuffer.glsl" })
        {
            const std::string frag = FragmentStageOf(ReadShader(name));
            ASSERT_FALSE(frag.empty()) << name;
            EXPECT_TRUE(Contains(frag, "discard")) << name << " does not alpha-test";
        }
    }
} // namespace OloEngine::Tests
