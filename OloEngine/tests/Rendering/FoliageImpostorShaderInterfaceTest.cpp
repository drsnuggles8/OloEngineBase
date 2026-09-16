// =============================================================================
// FoliageImpostorShaderInterfaceTest.cpp
//
// The octahedral impostor card has a forward program (Foliage_Impostor.glsl)
// and a deferred one (Foliage_Impostor_GBuffer.glsl, issue #1225). They must
// be the SAME card — same placement, same atlas sampling, same discard rule —
// differing only in what they write. That is enforced STRUCTURALLY: both stages
// come from shared includes, so the two files cannot drift. What this test
// pins is that the structure stays in place and that each file's own part —
// its output interface — is what it claims:
//
//   * both vertex stages are the shared FoliageImpostorVertexStage.glsl and
//     nothing else, with the instance-index forwarding decided by the includer;
//   * both fragment stages sample through FoliageImpostorSampling.glsl and
//     carry no sampling or discard of their own (the discard rule lives in the
//     include on purpose — the two used to disagree, and Deferred drew a
//     thinner canopy that retreated ~7 m earlier);
//   * the deferred fragment writes the full G-Buffer MRT set and does not also
//     carry the forward lit output or relight itself; the forward fragment is
//     the reverse.
//
// Whether the SPIR-V those files produce matches the production G-Buffer
// attachment formats is ShaderStageContractTest's job; whether the two cards
// land in the same place is FoliageImpostorPlacementTest's (it expands the
// shared include, so it now covers both). Text is checked whole-identifier and
// outside comments, so a doc comment quoting an identifier can neither satisfy
// nor break an assertion here.
//
// OLO_TEST_LAYER: unit
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/ShaderSourceScan.h"
#include "ShaderHarness.h"

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>

#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        namespace SH = OloEngine::Tests::ShaderHarness;
        using OloEngine::ShaderSourceScan::DefinesOutsideComments;
        using OloEngine::ShaderSourceScan::MentionsOutsideComments;

        struct ImpostorProgram
        {
            std::string m_Vertex;
            std::string m_Fragment;
        };

        [[nodiscard]] ImpostorProgram Load(const char* name)
        {
            const auto root = SH::ResolveShaderRoot();
            ImpostorProgram program;
            for (const auto& [kind, stage] : SH::SplitByType(SH::ReadWholeFile(root / name)))
            {
                if (kind == shaderc_glsl_vertex_shader)
                    program.m_Vertex = stage;
                else if (kind == shaderc_glsl_fragment_shader)
                    program.m_Fragment = stage;
            }
            return program;
        }
    } // namespace

    class FoliageImpostorShaderInterfaceTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Forward = Load("Foliage_Impostor.glsl");
            m_Deferred = Load("Foliage_Impostor_GBuffer.glsl");
            ASSERT_FALSE(m_Forward.m_Vertex.empty());
            ASSERT_FALSE(m_Forward.m_Fragment.empty());
            ASSERT_FALSE(m_Deferred.m_Vertex.empty()) << "Foliage_Impostor_GBuffer.glsl is missing — the impostor "
                                                         "canopy has no deferred route and cannot reach the G-Buffer";
            ASSERT_FALSE(m_Deferred.m_Fragment.empty());
        }

        ImpostorProgram m_Forward;
        ImpostorProgram m_Deferred;
    };

    TEST_F(FoliageImpostorShaderInterfaceTest, BothVertexStagesAreTheOneSharedStage)
    {
        for (const auto* vs : { &m_Forward.m_Vertex, &m_Deferred.m_Vertex })
        {
            EXPECT_TRUE(MentionsOutsideComments(*vs, "FoliageImpostorVertexStage.glsl"));
            // No private placement math: a stage that declares its own main()
            // has forked the card.
            EXPECT_FALSE(MentionsOutsideComments(*vs, "gl_Position"))
                << "a vertex stage computes its own gl_Position instead of including the shared stage";
        }

        // The ONE thing the includer decides. The forward fragment never reads
        // the instance index (an unconsumed varying is a Vulkan validation
        // warning); the deferred fragment reads u_EntityID through it.
        EXPECT_TRUE(DefinesOutsideComments(m_Forward.m_Vertex, "OLO_INSTANCE_NO_FORWARD"));
        EXPECT_FALSE(DefinesOutsideComments(m_Deferred.m_Vertex, "OLO_INSTANCE_NO_FORWARD"))
            << "the deferred variant suppresses v_InstanceIndex but its fragment reads u_EntityID";
        EXPECT_TRUE(MentionsOutsideComments(m_Deferred.m_Fragment, "InstanceBlock.glsl"));
    }

    TEST_F(FoliageImpostorShaderInterfaceTest, BothFragmentStagesSampleAndDiscardThroughTheOneInclude)
    {
        for (const auto* fs : { &m_Forward.m_Fragment, &m_Deferred.m_Fragment })
        {
            EXPECT_TRUE(MentionsOutsideComments(*fs, "FoliageImpostorSampling.glsl"));
            EXPECT_TRUE(MentionsOutsideComments(*fs, "SampleImpostorCard"));
            // Sampling machinery and the discard rule belong to the include.
            // A fragment that re-implements either has re-opened the seam.
            EXPECT_FALSE(MentionsOutsideComments(*fs, "OctaDirToGrid")) << "a fragment samples the atlas itself";
            EXPECT_FALSE(MentionsOutsideComments(*fs, "discard")) << "a fragment carries its own discard rule";
        }
    }

    TEST_F(FoliageImpostorShaderInterfaceTest, DeferredWritesTheGBufferAndDoesNotRelight)
    {
        const auto& fs = m_Deferred.m_Fragment;
        // The full writer set. RT5 (BakedGI) matters even with no lightmap: an
        // MRT output never written is undefined, and RT5's alpha reads as a
        // "has baked GI" flag (issue #865).
        for (const char* out : { "o_GBufferAlbedo", "o_GBufferNormal", "o_GBufferEmissive", "o_GBufferVelocity",
                                 "o_GBufferEntityID", "o_GBufferBakedGI" })
        {
            EXPECT_TRUE(MentionsOutsideComments(fs, out)) << out;
        }
        EXPECT_FALSE(MentionsOutsideComments(fs, "FragColor")) << "the deferred variant still declares the forward lit output";
        // MultiLightBuffer, not MultiLightData: #1234 replaced the impostor's
        // truncated four-int-plus-Light[0] block with the real full one, the
        // same block PBR_MultiLight declares. Chasing the rename matters most
        // HERE — a negative assertion against a name nothing declares any more
        // passes for the wrong reason and stops testing anything.
        EXPECT_FALSE(MentionsOutsideComments(fs, "MultiLightBuffer"))
            << "the deferred variant still relights itself; DeferredLightingPass shades the G-Buffer";
    }

    TEST_F(FoliageImpostorShaderInterfaceTest, ForwardKeepsTheLitOutputAndNoGBufferTarget)
    {
        const auto& fs = m_Forward.m_Fragment;
        EXPECT_TRUE(MentionsOutsideComments(fs, "FragColor"));
        // See the deferred case above for the #1234 rename. The forward card
        // relights itself, so it must still name the light block — and since
        // #1234 it walks the WHOLE array rather than Light[0] alone.
        EXPECT_TRUE(MentionsOutsideComments(fs, "MultiLightBuffer"));
        EXPECT_FALSE(MentionsOutsideComments(fs, "o_GBufferAlbedo"))
            << "the forward card declares a G-Buffer output — executed against the Scene MRT that maps a float "
               "onto the R32_SINT entity-ID target (issue #955)";
    }
} // namespace OloEngine::Tests
