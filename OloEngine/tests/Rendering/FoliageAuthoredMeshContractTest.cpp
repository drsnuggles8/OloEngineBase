// =============================================================================
// FoliageAuthoredMeshContractTest.cpp
//
// A foliage layer with an authored plant mesh draws TWO shapes over one
// instance stream (issue #1233): its real geometry up close and its flat card
// (or octahedral impostor) beyond. This pins the three contracts that hold that
// together, all of which fail SILENTLY — the frame looks plausible and
// something downstream is wrong:
//
//   1. CONSERVATIVE BOUNDS. A quad's box is not a pine's. Bounding a tree with
//      a blade of grass's extent pops it out at the screen edge, and the
//      registry's per-instance box and FoliageRenderer's per-layer AABB have to
//      be the same rule or one culls what the other kept.
//   2. THE HAND-OVER PARTITION. The mesh draw and the card draw each keep the
//      pixels the other discards. Two independently-written fades leave a
//      stretch where a pine and a billboard of that pine are both opaque —
//      which, in the G-Buffer, has no alpha to resolve it.
//   3. ONE PLACEMENT RULE ACROSS EVERY PASS. Depth, picking, shadows and motion
//      must use the geometry the beauty pass used. This is the criterion with
//      no CPU symptom at all: the plant renders correctly and its shadow is a
//      quad. It is enforced structurally — every foliage vertex stage places
//      geometry through include/FoliageInstanceGeometry.glsl — and what this
//      file pins is that the structure stays in place.
//
// Shader text is checked whole-identifier and outside comments, so a doc
// comment quoting an identifier can neither satisfy nor break an assertion.
//
// OLO_TEST_LAYER: unit
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/ShaderSourceScan.h"
#include "OloEngine/Terrain/Foliage/FoliageInstanceRegistry.h"
#include "ShaderHarness.h"

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace SH = OloEngine::Tests::ShaderHarness;
        using OloEngine::ShaderSourceScan::MentionsOutsideComments;

        [[nodiscard]] std::string ReadShader(const char* name)
        {
            return SH::ReadWholeFile(SH::ResolveShaderRoot() / name);
        }

        [[nodiscard]] std::string VertexStageOf(const char* name)
        {
            for (const auto& [kind, stage] : SH::SplitByType(ReadShader(name)))
            {
                if (kind == shaderc_glsl_vertex_shader)
                    return stage;
            }
            return {};
        }

        // The C++ mirror of foliageLodKeep() in FoliageInstanceGeometry.glsl,
        // over the `dither` value rather than the dither FUNCTION — issue
        // #1237 gave that two implementations (the legacy sine hash and the
        // decorrelated interleaved-gradient one) behind an authored switch,
        // and which one produces the number is not what this pins. What it
        // pins is that whatever number comes out, the two draws PARTITION on
        // it, which is a property of the comparison and holds for both.
        // Mirrored rather than shared because the GLSL runs on the GPU: what
        // this pins is that the RULE partitions, which is a property of the
        // rule and not of the language it is written in. The structural
        // assertions below are what pin that both draws run this one rule.
        [[nodiscard]] bool LodKeep(bool isAuthoredMesh, f32 coverage, f32 dither)
        {
            if (coverage <= 0.0f)
                return !isAuthoredMesh;
            if (coverage >= 1.0f)
                return isAuthoredMesh;
            return isAuthoredMesh ? (dither < coverage) : (dither >= coverage);
        }
    } // namespace

    // ── 1. Conservative bounds ───────────────────────────────────────────────

    TEST(FoliageAuthoredMeshBounds, CardProfileReproducesTheHistoricalCardBox)
    {
        // The default profile IS the card, and must keep bounding it exactly as
        // it did before the authored mesh existed: standing ON the ground point,
        // h*s tall, 0.5*s of horizontal half-extent either side. A silent change
        // here would re-cull every existing scene's grass.
        constexpr f32 scale = 1.5f;
        constexpr f32 height = 2.0f;
        const glm::vec3 pos(10.0f, 3.0f, -4.0f);

        const BoundingBox box = FoliageInstanceBounds(pos, scale, height, FoliageBoundsProfile{});

        EXPECT_FLOAT_EQ(box.Min.x, pos.x - 0.5f * scale);
        EXPECT_FLOAT_EQ(box.Max.x, pos.x + 0.5f * scale);
        EXPECT_FLOAT_EQ(box.Min.z, pos.z - 0.5f * scale);
        EXPECT_FLOAT_EQ(box.Max.z, pos.z + 0.5f * scale);
        EXPECT_FLOAT_EQ(box.Min.y, pos.y);
        EXPECT_FLOAT_EQ(box.Max.y, pos.y + height * scale);
    }

    TEST(FoliageAuthoredMeshBounds, MeshProfileWidensToTheRealGeometryAndKeepsTheCard)
    {
        // A layer with a mesh draws BOTH shapes, so its bound is the union. The
        // mesh term scales by height*scale (it is scaled uniformly, matching the
        // impostor); the card term by scale alone. Whichever is larger wins —
        // there is no single term that covers both, because `height` is per
        // instance.
        FoliageBoundsProfile profile;
        profile.m_HalfExtentXZ = 0.5f;              // the card, unchanged
        profile.m_HalfExtentXZHeightScaled = 0.35f; // a canopy, in unit-mesh units
        profile.m_MinY = -0.1f;                     // roots below the origin
        profile.m_MaxY = 1.0f;

        const glm::vec3 pos(0.0f);

        // Tall instance: the canopy (0.35 * 8 = 2.8) beats the card (0.5 * 1).
        {
            const BoundingBox box = FoliageInstanceBounds(pos, 1.0f, 8.0f, profile);
            EXPECT_FLOAT_EQ(box.Max.x, 2.8f);
            EXPECT_FLOAT_EQ(box.Min.x, -2.8f);
            EXPECT_FLOAT_EQ(box.Min.y, -0.8f) << "a mesh authored below its origin must take the bound down with it";
            EXPECT_FLOAT_EQ(box.Max.y, 8.0f);
        }

        // Short instance: the card (0.5 * 2 = 1.0) beats the canopy
        // (0.35 * 0.4 = 0.14), and it is the card that gets drawn out there.
        {
            const BoundingBox box = FoliageInstanceBounds(pos, 2.0f, 0.2f, profile);
            EXPECT_FLOAT_EQ(box.Max.x, 1.0f);
        }
    }

    TEST(FoliageAuthoredMeshBounds, RegistryRecordsAndGroupsCarryTheProfileBounds)
    {
        // The registry's per-instance box and its spatial groups' bounds both
        // come from the profile the layer handed BeginLayer. A group that still
        // bounded cards would let a culler drop a tree whose canopy is on
        // screen (issue #1230's groups are what #1233's geometry hangs off).
        FoliageBoundsProfile profile;
        profile.m_HalfExtentXZHeightScaled = 1.25f;
        profile.m_MaxY = 1.0f;

        FoliageLayer layer;
        layer.Name = "Pines";
        layer.MeshPath = "Assets/Meshes/pine.obj";

        FoliageInstanceRegistry registry;
        registry.BeginGeneration({ layer });
        registry.BeginLayer(0, layer, /*placementSeed=*/7, /*spacing=*/4.0f,
                            /*worldSizeX=*/64.0f, /*worldSizeZ=*/64.0f,
                            FoliageRepresentation::AuthoredMesh, /*variantUnavailable=*/false, profile);

        FoliageInstanceData row{};
        row.PositionScale = glm::vec4(8.0f, 0.0f, 8.0f, 1.0f);
        row.RotationHeight = glm::vec4(0.0f, 6.0f, 1.0f, 0.0f);
        row.ColorAlpha = glm::vec4(1.0f);
        registry.AddInstance(2, 2, row, 0);
        registry.EndLayer();
        registry.EndGeneration();

        ASSERT_EQ(registry.GetRecords().size(), 1u);
        const auto& record = registry.GetRecords().front();
        EXPECT_FLOAT_EQ(record.m_LocalBounds.Max.x, 8.0f + 1.25f * 6.0f)
            << "the record still bounds a quad, not the authored mesh";
        EXPECT_FLOAT_EQ(record.m_LocalBounds.Max.y, 6.0f);

        ASSERT_FALSE(registry.GetGroups().empty());
        const auto& group = registry.GetGroups()[record.m_GroupIndex];
        EXPECT_GE(group.m_LocalBounds.Max.x, record.m_LocalBounds.Max.x)
            << "the spatial group does not contain the instance it holds";

        // The representation is explicit metadata, not something a consumer
        // re-derives from layer flags (issue #1230's first criterion).
        EXPECT_EQ(record.m_Representation, FoliageRepresentation::AuthoredMesh);
        EXPECT_EQ(registry.GetCensus().m_AuthoredMeshInstances, 1u);
        EXPECT_EQ(registry.GetCensus().m_MeshCardInstances, 0u);
    }

    TEST(FoliageAuthoredMeshBounds, AMeshThatWillNotLoadCountsAsAnUnavailableVariant)
    {
        // A missing asset gets an explicit diagnostic, never a silent fall back
        // to the quad (issue #1233's third criterion). The plants still draw —
        // as cards — so they are not Unsupported; the VARIANT is.
        FoliageLayer layer;
        layer.Name = "Broken";
        layer.MeshPath = "Assets/Meshes/does-not-exist.obj";

        FoliageInstanceRegistry registry;
        registry.BeginGeneration({ layer });
        registry.BeginLayer(0, layer, 1, 4.0f, 64.0f, 64.0f,
                            FoliageRepresentation::MeshCard, /*variantUnavailable=*/true);
        FoliageInstanceData row{};
        row.PositionScale = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
        row.RotationHeight = glm::vec4(0.0f, 1.0f, 1.0f, 0.0f);
        registry.AddInstance(0, 0, row, 0);
        registry.EndLayer();
        registry.EndGeneration();

        EXPECT_EQ(registry.GetCensus().m_UnsupportedVariants, 1u);
        EXPECT_EQ(registry.GetCensus().m_MeshCardInstances, 1u);
        EXPECT_EQ(registry.GetCensus().m_AuthoredMeshInstances, 0u);
    }

    // ── 2. The hand-over partition ───────────────────────────────────────────

    TEST(FoliageAuthoredMeshLod, MeshAndCardPartitionEveryPixelExactlyOnce)
    {
        // The property that matters is not that each side fades — it is that
        // between them they cover each pixel exactly once, at every point in
        // the band. That is what stops a pine and a card of that pine being
        // opaque at the same time in a pass with no alpha blending.
        for (i32 step = 0; step <= 20; ++step)
        {
            const f32 coverage = static_cast<f32>(step) / 20.0f;
            for (i32 d = 0; d < 64; ++d)
            {
                const f32 dither = static_cast<f32>(d) / 64.0f;
                const bool mesh = LodKeep(true, coverage, dither);
                const bool card = LodKeep(false, coverage, dither);
                EXPECT_NE(mesh, card) << "coverage " << coverage << " dither " << dither
                                      << ": both draws kept the pixel, or neither did";
            }
        }
    }

    TEST(FoliageAuthoredMeshLod, ALayerWithNoMeshIsTheCardPathUnchanged)
    {
        // bandEnd == 0 means "no authored mesh", and the card must then own
        // every pixel exactly as it did before #1233 — no dither, no cut.
        for (i32 d = 0; d < 64; ++d)
        {
            const f32 dither = static_cast<f32>(d) / 64.0f;
            EXPECT_TRUE(LodKeep(false, 0.0f, dither));
            EXPECT_FALSE(LodKeep(true, 0.0f, dither));
        }
    }

    // ── 3. One placement rule across every pass ──────────────────────────────

    class FoliagePlacementSharingTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Forward = VertexStageOf("Foliage_Instance.glsl");
            m_Deferred = VertexStageOf("Foliage_Instance_GBuffer.glsl");
            m_Depth = VertexStageOf("Foliage_Depth.glsl");
            m_Impostor = VertexStageOf("Foliage_Impostor.glsl");
            m_SharedStage = ReadShader("include/FoliageInstanceVertexStage.glsl");
            m_Geometry = ReadShader("include/FoliageInstanceGeometry.glsl");
            ASSERT_FALSE(m_Forward.empty());
            ASSERT_FALSE(m_Deferred.empty());
            ASSERT_FALSE(m_Depth.empty());
            ASSERT_FALSE(m_Impostor.empty());
            ASSERT_FALSE(m_SharedStage.empty());
            ASSERT_FALSE(m_Geometry.empty());
        }

        std::string m_Forward, m_Deferred, m_Depth, m_Impostor, m_SharedStage, m_Geometry;
    };

    TEST_F(FoliagePlacementSharingTest, TheTwoInstanceProgramsAreTheOneSharedVertexStage)
    {
        for (const auto* vs : { &m_Forward, &m_Deferred })
        {
            EXPECT_TRUE(MentionsOutsideComments(*vs, "FoliageInstanceVertexStage.glsl"));
            EXPECT_FALSE(MentionsOutsideComments(*vs, "gl_Position"))
                << "a vertex stage computes its own gl_Position instead of including the shared stage";
        }
    }

    TEST_F(FoliagePlacementSharingTest, EveryPassPlacesGeometryThroughTheOneInclude)
    {
        // The shadow depth stage keeps a main() of its own — it runs under the
        // shadow camera UBO with no wind field bound — but it may not decide
        // WHERE a plant goes. Every stage calls the same two functions.
        for (const auto* vs : { &m_SharedStage, &m_Depth })
        {
            EXPECT_TRUE(MentionsOutsideComments(*vs, "FoliageInstanceGeometry.glsl"));
            EXPECT_TRUE(MentionsOutsideComments(*vs, "foliageInstanceLocalPos"))
                << "a stage scales the instance itself — the card's anisotropic rule and the mesh's "
                   "uniform one have to come from one place or a pass draws a different plant";
            EXPECT_TRUE(MentionsOutsideComments(*vs, "foliageInstanceRotation"));
            EXPECT_TRUE(MentionsOutsideComments(*vs, "foliageDeform"))
                << "a stage sways the plant with its own sine — a shadow out of phase with its plant "
                   "is the same desync as a shadow of the wrong shape";
        }
    }

    TEST_F(FoliagePlacementSharingTest, EveryPassDecidesTheHandOverWithTheOneRule)
    {
        // Including the impostor: it is the FAR side of the same partition, so
        // a layer with both an authored mesh and an impostor would otherwise
        // draw the pine and a card of that pine on top of each other up close.
        for (const auto* src : { &m_SharedStage, &m_Depth, &m_Impostor })
        {
            EXPECT_TRUE(MentionsOutsideComments(*src, "foliageMeshCoverageLod") ||
                        MentionsOutsideComments(*src, "FoliageImpostorVertexStage.glsl"))
                << "a stage does not compute the hand-over share, and cannot agree with the others";
        }

        // Measured from the RENDER ORIGIN, not the camera: the shadow pass
        // renders under a light's camera and leaves u_CameraPosition at zero, so
        // a hand-over keyed on it would shadow the card where the lit frame drew
        // the mesh. Every pass shifts by the same render origin (issue #429).
        //
        // Checked at the CALL, not by the absence of the identifier: every stage
        // legitimately DECLARES u_CameraPosition as part of the CameraMatrices
        // block, and the forward fragment needs it for the layer's own distance
        // fade. What must not happen is it reaching this particular argument.
        //
        // Since #1237 the call takes a pre-computed `lodDist` rather than the
        // distance expression inline, so what is checked is the LINE THAT
        // DEFINES IT: it must measure from u_MeshViewPos and must not mention
        // u_CameraPosition. Pinning the whole call text instead would break on
        // any reformatting of a three-argument call, which is not the property
        // worth guarding.
        for (const auto* vs : { &m_SharedStage, &m_Depth })
        {
            const sizet def = vs->find("float lodDist");
            ASSERT_NE(def, std::string::npos)
                << "no stage-local hand-over distance — the call's first argument is now unpinnable";
            const sizet eol = std::min(vs->find('\n', def), vs->size());
            const std::string line = vs->substr(def, eol - def);
            EXPECT_NE(line.find("u_MeshViewPos"), std::string::npos)
                << "the hand-over distance is not measured from u_MeshViewPos — if it is measured from "
                   "u_CameraPosition, the shadow pass (whose camera is the light) picks a different shape than "
                   "the lit frame; if from the render origin, it becomes a ring around the world origin whenever "
                   "camera-relative rendering is off. Got: "
                << line;
            EXPECT_EQ(line.find("u_CameraPosition"), std::string::npos) << line;
            EXPECT_NE(vs->find("foliageMeshCoverageLod(lodDist"), std::string::npos)
                << "the hand-over is not fed that distance";
        }
    }

    TEST_F(FoliagePlacementSharingTest, TheMeshAndCardScalingRulesAreTheImpostorsRule)
    {
        // The mesh is scaled UNIFORMLY by height * scale, which is what the
        // impostor card does (`radius = meshRadius * height * scale`). Stretching
        // the mesh to the card's aspect instead renders a pine as a needle, and
        // the near geometry and the far impostor become different trees.
        EXPECT_TRUE(MentionsOutsideComments(m_Geometry, "foliageInstanceLocalPos"));
        EXPECT_TRUE(MentionsOutsideComments(m_Geometry, "meshLocal"));
        EXPECT_TRUE(MentionsOutsideComments(m_Geometry, "cardLocal"));

        const std::string impostorStage = ReadShader("include/FoliageImpostorVertexStage.glsl");
        ASSERT_FALSE(impostorStage.empty());
        EXPECT_TRUE(MentionsOutsideComments(impostorStage, "foliageMeshCoverageLod"))
            << "the impostor card does not take part in the hand-over and will overlap the near mesh";
    }
} // namespace OloEngine::Tests
