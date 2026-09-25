// =============================================================================
// FoliageDepthNormalPrepassShaderTest.cpp
//
// Issue #1474 put forward foliage in the forward depth-normal prepass through
// two twin programs, Foliage_Instance_DepthNormal.glsl and
// Foliage_Impostor_DepthNormal.glsl. FoliageRenderPass then draws the same
// leaves with the colour programs at GL_LEQUAL, so each twin must carve exactly
// its colour program's depth, or the colour pass loses edge fragments to depth
// it did not write. That rests on four structural facts, pinned here:
//
//   * each twin's vertex stage is its colour program's shared include, with the
//     same instance-forwarding define, and that include declares
//     `invariant gl_Position` (two programs, one depth);
//   * each twin discards what its colour program discards: the same number of
//     discards in the instance pair, and the impostor pair compiling the shared
//     discard rule with the same OLO_FOLIAGE_IMPOSTOR_ALPHA_BLENDED choice;
//   * each twin writes scene attachment 2 and nothing else (CommandDispatch
//     masks every other attachment during the prepass);
//   * the normal goes through the one forward view-normal encode.
//
// Whether the twins draw the right pixels on a GPU is
// ForwardScreenSpaceAOEvidenceTest's foliage cells.
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

        struct Stages
        {
            std::string Vertex;
            std::string Fragment;
        };

        [[nodiscard]] Stages Load(const char* name)
        {
            Stages stages;
            for (const auto& [kind, stage] : SH::SplitByType(SH::ReadWholeFile(SH::ResolveShaderRoot() / name)))
            {
                if (kind == shaderc_glsl_vertex_shader)
                    stages.Vertex = stage;
                else if (kind == shaderc_glsl_fragment_shader)
                    stages.Fragment = stage;
            }
            return stages;
        }

        // Source with // and /* */ comments removed, so a comment quoting a
        // token can neither satisfy nor break a count.
        [[nodiscard]] std::string StripComments(const std::string& src)
        {
            std::string out;
            out.reserve(src.size());
            for (std::size_t i = 0; i < src.size(); ++i)
            {
                if (src.compare(i, 2, "//") == 0)
                {
                    while (i < src.size() && src[i] != '\n')
                        ++i;
                    out += '\n';
                }
                else if (src.compare(i, 2, "/*") == 0)
                {
                    const auto end = src.find("*/", i + 2);
                    i = end == std::string::npos ? src.size() : end + 1;
                }
                else
                {
                    out += src[i];
                }
            }
            return out;
        }

        [[nodiscard]] int CountOutsideComments(const std::string& src, const std::string& token)
        {
            const std::string code = StripComments(src);
            int count = 0;
            for (auto at = code.find(token); at != std::string::npos; at = code.find(token, at + token.size()))
                ++count;
            return count;
        }

        struct Twin
        {
            const char* Prepass;
            const char* Colour;
            const char* VertexInclude;
        };

        constexpr Twin kTwins[] = {
            { "Foliage_Instance_DepthNormal.glsl", "Foliage_Instance.glsl", "FoliageInstanceVertexStage.glsl" },
            { "Foliage_Impostor_DepthNormal.glsl", "Foliage_Impostor.glsl", "FoliageImpostorVertexStage.glsl" },
        };
    } // namespace

    TEST(FoliageDepthNormalPrepassShader, EachTwinRunsItsColourProgramsInvariantVertexStage)
    {
        for (const Twin& twin : kTwins)
        {
            SCOPED_TRACE(twin.Prepass);
            const Stages prepass = Load(twin.Prepass);
            const Stages colour = Load(twin.Colour);
            ASSERT_FALSE(prepass.Vertex.empty());
            ASSERT_FALSE(colour.Vertex.empty());

            const std::string include = std::string("include/") + twin.VertexInclude;
            EXPECT_TRUE(MentionsOutsideComments(prepass.Vertex, include)) << "the twin no longer includes " << include;
            EXPECT_TRUE(MentionsOutsideComments(colour.Vertex, include)) << twin.Colour << " no longer includes " << include;
            EXPECT_EQ(DefinesOutsideComments(prepass.Vertex, "OLO_INSTANCE_NO_FORWARD"),
                      DefinesOutsideComments(colour.Vertex, "OLO_INSTANCE_NO_FORWARD"))
                << "the twin and its colour program compile the shared vertex stage differently";

            const std::string stage = SH::ReadWholeFile(SH::ResolveShaderRoot() / "include" / twin.VertexInclude);
            EXPECT_EQ(CountOutsideComments(stage, "invariant gl_Position;"), 1)
                << twin.VertexInclude << " lost `invariant gl_Position`: the prepass twin and the colour program are "
                   "two programs, and nothing else makes their depth equal for the colour pass's GL_LEQUAL test";
        }
    }

    TEST(FoliageDepthNormalPrepassShader, EachTwinDiscardsWhatItsColourProgramDiscards)
    {
        for (const Twin& twin : kTwins)
        {
            SCOPED_TRACE(twin.Prepass);
            const Stages prepass = Load(twin.Prepass);
            const Stages colour = Load(twin.Colour);
            ASSERT_FALSE(prepass.Fragment.empty());
            ASSERT_FALSE(colour.Fragment.empty());

            EXPECT_EQ(CountOutsideComments(prepass.Fragment, "discard;"), CountOutsideComments(colour.Fragment, "discard;"))
                << "the twin's discards no longer match its colour program's";
            EXPECT_EQ(DefinesOutsideComments(prepass.Fragment, "OLO_FOLIAGE_IMPOSTOR_ALPHA_BLENDED"),
                      DefinesOutsideComments(colour.Fragment, "OLO_FOLIAGE_IMPOSTOR_ALPHA_BLENDED"))
                << "the twin compiles the shared impostor discard rule differently from its colour program";
        }

        // The instance pair's three discards are the same three conditions.
        const Stages prepass = Load("Foliage_Instance_DepthNormal.glsl");
        const Stages colour = Load("Foliage_Instance.glsl");
        for (const char* condition : { "foliageLodKeep(u_MeshParams.x > 0.5, v_MeshCoverage, gl_FragCoord.xy, v_InstanceSeed,",
                                       "< v_AlphaCutoff)", "if (fadeFactor <= 0.0)" })
        {
            EXPECT_EQ(CountOutsideComments(prepass.Fragment, condition), 1) << "twin: " << condition;
            EXPECT_EQ(CountOutsideComments(colour.Fragment, condition), 1) << "colour: " << condition;
        }
    }

    TEST(FoliageDepthNormalPrepassShader, EachTwinWritesOnlyTheViewNormal)
    {
        for (const Twin& twin : kTwins)
        {
            SCOPED_TRACE(twin.Prepass);
            const Stages prepass = Load(twin.Prepass);
            ASSERT_FALSE(prepass.Fragment.empty());
            EXPECT_EQ(CountOutsideComments(prepass.Fragment, ") out "), 1) << "the twin declares more than one output";
            EXPECT_EQ(CountOutsideComments(prepass.Fragment, "layout(location = 2) out vec2 o_ViewNormal;"), 1)
                << "the twin's one output is not scene attachment 2's view normal";
            EXPECT_EQ(CountOutsideComments(prepass.Fragment, "o_ViewNormal = oloForwardViewNormalOutput(u_View,"), 1)
                << "the twin does not encode through include/ViewNormalOutput.glsl";
        }
    }
} // namespace OloEngine::Tests
