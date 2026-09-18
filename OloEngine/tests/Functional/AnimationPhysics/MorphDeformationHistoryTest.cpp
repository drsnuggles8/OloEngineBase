#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
//
// =============================================================================
// MorphDeformationHistoryTest — Functional Test (issue #1227).
//
// Cross-subsystem seam under test:
//   Scene's frame boundary × the morph half of the shared animated surface.
//   #1226 gave the bone half a previous-pose palette and a rule about when it
//   may be trusted; this pins the same contract for morph weights, driven by
//   the real Scene::OnUpdateRuntime rather than by calling the system directly.
//
//   Two things are being asserted, and they pull in opposite directions.
//
//   A morphing surface that MOVED between two frames cannot be reprojected at
//   all. SkeletalDeformation.glsl builds its previous-pose position as
//   `prevSkinMatrix * restPosition`, where `restPosition` is whatever the vertex
//   buffer holds THIS frame — i.e. the surface as morphed this frame. When the
//   weights change, that previous position is a hybrid: last frame's pose on
//   this frame's surface. Emitting a velocity from it is a plausible wrong
//   image, so the history is thrown away instead, explicitly and countably.
//
//   A morphing surface that did NOT move must keep its history. Rejecting on
//   every frame a morph component merely exists would cost every expressing
//   character its temporal history permanently, and the symptom — a face that
//   is noisy under TAA — looks like a filter problem, several subsystems from
//   the cause. The pause case is the sharp end of it: a paused scene runs no
//   gameplay tick at all, so a rejection that is set and never cleared sticks
//   for the length of the pause.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Animation/SkeletalDeformation.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/SkinLayeredSpecular.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>

#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr u32 kVertexCount = 4;
    /// The one target's delta on vertex 0. Large enough that "deformed" and "not
    /// deformed" cannot be confused for float noise.
    constexpr glm::vec3 kSmileDelta{ 0.0f, 2.0f, 0.0f };

    [[nodiscard]] Ref<MeshSource> MakeMorphableQuad()
    {
        std::vector<Vertex> vertices;
        vertices.reserve(kVertexCount);
        for (u32 i = 0; i < kVertexCount; ++i)
        {
            vertices.emplace_back(glm::vec3(static_cast<f32>(i), 0.0f, 0.0f),
                                  glm::vec3(0.0f, 0.0f, 1.0f),
                                  glm::vec2(0.0f));
        }
        std::vector<u32> indices = { 0, 1, 2, 0, 2, 3 };

        auto source = Ref<MeshSource>::Create(std::move(vertices), std::move(indices));

        auto targets = Ref<MorphTargetSet>::Create();
        MorphTarget smile("Smile", kVertexCount);
        smile.Vertices[0].DeltaPosition = kSmileDelta;
        targets->AddTarget(smile);
        source->SetMorphTargets(targets);

        return source;
    }
} // namespace

class MorphDeformationHistoryTest : public FunctionalTest
{
  protected:
    static constexpr f32 kClipDuration = 10.0f;

    void BuildScene() override
    {
        m_Entity = GetScene().CreateEntity("ExpressingHead");
        m_Entity.AddComponent<MeshComponent>(MakeMorphableQuad());
        m_Entity.AddComponent<MorphTargetComponent>();

        // Skinned AS WELL as morphing, because the two halves share one history:
        // a morph discontinuity has to drop the BONE history too, or the shaders
        // keep reprojecting a pose through a surface that moved.
        m_Entity.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());
        auto& anim = m_Entity.AddComponent<AnimationStateComponent>();
        anim.m_CurrentClip = Fixtures::MakeTranslationClip(kClipDuration);
        anim.m_IsPlaying = true;
        anim.m_CurrentTime = 0.0f;
    }

    [[nodiscard]] MorphTargetComponent& Morph()
    {
        return m_Entity.GetComponent<MorphTargetComponent>();
    }

    [[nodiscard]] SkeletonData& Skeleton()
    {
        return *m_Entity.GetComponent<SkeletonComponent>().m_Skeleton;
    }

    [[nodiscard]] const TArray<Vertex>& SurfaceVertices()
    {
        return m_Entity.GetComponent<MeshComponent>().m_MeshSource->GetVertices();
    }

    Entity m_Entity;
};

// ---------------------------------------------------------------------------
// Criterion 1: the combination order.
// ---------------------------------------------------------------------------

TEST_F(MorphDeformationHistoryTest, MorphDeltasAreAppliedToTheRestSurface)
{
    // The order is morph-then-skin, and this is where it is decided: the morph
    // pass writes the morphed REST surface into the one vertex buffer every
    // skinned pass reads, and SkeletalDeformation.glsl applies the skin matrix to
    // whatever that buffer holds. So the vertex buffer after a frame must hold
    // base + w*delta with NO bone transform folded in — if anything had skinned
    // the surface on the CPU, the animating bone below would show up here and the
    // GPU would then skin it a second time.
    Morph().SetWeight("Smile", 1.0f);
    RunFrames(4);

    ASSERT_EQ(SurfaceVertices().Num(), static_cast<i32>(kVertexCount));
    const glm::vec3 deformed = SurfaceVertices()[0].Position;
    const glm::vec3 expected = glm::vec3(0.0f) + kSmileDelta;

    EXPECT_NEAR(deformed.x, expected.x, 1e-4f);
    EXPECT_NEAR(deformed.y, expected.y, 1e-4f)
        << "the deformed vertex is not base + delta in rest space — either the morph never "
           "reached the shared surface, or something applied the bone transform to it on the "
           "CPU, which the GPU producer would then apply a second time";
    EXPECT_NEAR(deformed.z, expected.z, 1e-4f);

    // Every consumer reads this one buffer, which is what makes "matching across
    // consumers" structural rather than a comparison that has to be re-measured.
    EXPECT_NEAR(SurfaceVertices()[1].Position.x, 1.0f, 1e-4f)
        << "a vertex with no delta moved — the morph wrote outside the target's own vertices";
}

TEST_F(MorphDeformationHistoryTest, HalfWeightScalesTheRestSpaceDelta)
{
    Morph().SetWeight("Smile", 0.5f);
    RunFrames(4);

    EXPECT_NEAR(SurfaceVertices()[0].Position.y, kSmileDelta.y * 0.5f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Criterion 2: previous morph state and discontinuities.
// ---------------------------------------------------------------------------

TEST_F(MorphDeformationHistoryTest, HoldingAnExpressionKeepsDeformationHistory)
{
    Morph().SetWeight("Smile", 1.0f);
    RunFrames(10);

    EXPECT_TRUE(Morph().HasMorphHistory)
        << "a morphing entity never acquired morph history at all — the frame-boundary "
           "advance is not reaching it";
    EXPECT_FALSE(Morph().RejectDeformationHistory)
        << "a held expression rejected its history anyway; rejecting whenever a morph "
           "component merely exists costs every expressing character its temporal history "
           "permanently, and the symptom looks like a TAA problem";
    EXPECT_TRUE(Skeleton().HasBoneHistory())
        << "the bone half was dropped although nothing about the surface changed";
}

TEST_F(MorphDeformationHistoryTest, AMovingExpressionRejectsHistoryRatherThanGuessing)
{
    Morph().SetWeight("Smile", 0.0f);
    RunFrames(6);
    ASSERT_TRUE(Morph().HasMorphHistory) << "precondition: history established while holding still";

    const auto& stats = Animation::SkeletalDeformationSystem::GetStats();
    const u32 resetsBefore = stats.HistoryResetsMorphSurfaceChanged;

    // The surface moves. The shaders reproject THROUGH this frame's rest surface,
    // so no previous pose they can be handed describes the geometry they will
    // draw — the only honest answer is zero motion.
    Morph().SetWeight("Smile", 1.0f);
    RunFrames(1);

    EXPECT_TRUE(Morph().RejectDeformationHistory)
        << "the morphed surface moved and the history was kept — the velocity emitted for "
           "every vertex the morph displaced is measured between two different surfaces, "
           "which TAA and motion blur faithfully smear";
    EXPECT_FALSE(Skeleton().HasBoneHistory())
        << "the BONE history survived a morph discontinuity. The two halves are one surface: "
           "a bone velocity reprojected through a rest surface that moved is exactly the "
           "wrong image the rejection exists to prevent";
    EXPECT_GT(stats.HistoryResetsMorphSurfaceChanged, resetsBefore)
        << "the rejection was not attributed — a discontinuity nobody can count reads as "
           "'this never happens' from the statistics panel";
}

TEST_F(MorphDeformationHistoryTest, TheRejectionDoesNotOutliveTheFrameThatCausedIt)
{
    Morph().SetWeight("Smile", 0.0f);
    RunFrames(4);
    Morph().SetWeight("Smile", 1.0f);
    RunFrames(1);
    ASSERT_TRUE(Morph().RejectDeformationHistory) << "precondition: the move was rejected";

    // Settle: the weights are not touched again, so the surface stops moving.
    RunFrames(6);

    EXPECT_FALSE(Morph().RejectDeformationHistory)
        << "the rejection latched. A one-frame statement about a surface that moved must be "
           "re-decided every frame, or an expression that reaches its pose and stops emits "
           "zero motion for the rest of the scene";
    EXPECT_TRUE(Skeleton().HasBoneHistory())
        << "bone history never recovered after the morph settled";
}

TEST_F(MorphDeformationHistoryTest, PausedMorphingEntityKeepsItsHistory)
{
    Morph().SetWeight("Smile", 1.0f);
    RunFrames(6);

    GetScene().SetPaused(true);
    RunFrames(8);

    // The gameplay tick — and with it the morph evaluation — does not run while
    // paused. The advance does, because it sits at the FRAME boundary: a history
    // pass registered in the gameplay schedule would stop running here and freeze
    // whatever it last decided, which is the #1226 failure in the morph half.
    EXPECT_TRUE(Morph().HasMorphHistory)
        << "morph history was lost across a pause — the frame-boundary advance is not "
           "running while paused, so it is not at the frame boundary";
    EXPECT_FALSE(Morph().RejectDeformationHistory)
        << "a paused, unchanging surface is reporting itself as having moved";
}

// ---------------------------------------------------------------------------
// Issue #1243: the expression-driven skin detail rides on THIS history.
// ---------------------------------------------------------------------------
//
// #1243's third acceptance criterion asks that expression-driven detail "blends
// deterministically with existing morph/animation state and emits valid history
// changes". It does not get a history mechanism of its own — it derives its
// weight from `AppliedWeights` and therefore INHERITS the one this file already
// pins. These cases assert that inheritance rather than re-testing the shading:
// if the detail weight is a pure function of AppliedWeights, then every frame it
// changes is a frame this file has already proved rejects history.
//
// The failure they exist to catch is a one-word substitution — `Weights` for
// `AppliedWeights` — which is invisible in a still frame, compiles, and is the
// obvious thing to reach for, because `Weights` is the authored map a script
// sets and it is one member away.

TEST_F(MorphDeformationHistoryTest, ExpressionDetailFollowsTheSurfaceOnTheGpuNotTheAuthoredWeights)
{
    Morph().SetWeight("Smile", 0.0f);
    RunFrames(4);
    ASSERT_EQ(SkinExpressionDetailWeight(Morph().AppliedWeights), 0.0f)
        << "precondition: a neutral face has no expression detail";

    // The authored weight moves. The morph pass has NOT run, so the surface on
    // the GPU is still the neutral one.
    Morph().SetWeight("Smile", 1.0f);

    EXPECT_EQ(SkinExpressionDetailWeight(Morph().AppliedWeights), 0.0f)
        << "THE DETAIL LED THE GEOMETRY BY A FRAME. The weight was read from the authored map "
           "rather than from the surface currently on the GPU, so this frame would shade with "
           "deepened pores on a face that has not moved yet — a shading change with no "
           "deformation-history rejection behind it, which is precisely the invalid history "
           "change #1243's third criterion forbids. Nothing about it is visible in a still.";

    // Now the pass runs and the surface really does carry the expression.
    RunFrames(1);
    EXPECT_GT(SkinExpressionDetailWeight(Morph().AppliedWeights), 0.0f)
        << "the detail never arrived at all once the surface moved, so the assertion above "
           "passed by being uniformly zero rather than by being correctly ordered";
}

TEST_F(MorphDeformationHistoryTest, EveryExpressionDetailChangeCarriesAHistoryRejection)
{
    Morph().SetWeight("Smile", 0.0f);
    RunFrames(6);
    ASSERT_TRUE(Morph().HasMorphHistory) << "precondition: history established while holding still";

    const f32 before = SkinExpressionDetailWeight(Morph().AppliedWeights);

    Morph().SetWeight("Smile", 1.0f);
    RunFrames(1);

    const f32 after = SkinExpressionDetailWeight(Morph().AppliedWeights);
    ASSERT_NE(before, after) << "precondition: the expression detail actually moved this frame";

    EXPECT_TRUE(Morph().RejectDeformationHistory)
        << "THE SHADING DETAIL MOVED AND THE HISTORY DID NOT. A temporal upscaler will reproject "
           "this frame's deepened pores through last frame's surface and smear them, and the "
           "smear reads as a bad upscaler rather than as a bad material. The two must move "
           "together — which they do for free while the detail weight is a pure function of "
           "AppliedWeights, and stop doing the moment it is not.";
}

TEST_F(MorphDeformationHistoryTest, AHeldExpressionKeepsItsDetailAndItsHistory)
{
    Morph().SetWeight("Smile", 1.0f);
    RunFrames(10);

    EXPECT_GT(SkinExpressionDetailWeight(Morph().AppliedWeights), 0.0f)
        << "a face holding an expression lost its detail weight — the applied weights were "
           "cleared by something other than the expression itself";
    EXPECT_FALSE(Morph().RejectDeformationHistory)
        << "a HELD expression rejected history. The detail is not changing, so nothing about "
           "it can justify throwing the history away; a rejection that fires while a face "
           "merely HAS an expression costs every expressing character its temporal history "
           "for as long as it wears one.";
}

// ---------------------------------------------------------------------------
// Criteria 3 and 4: malformed input and stale cache reuse.
// ---------------------------------------------------------------------------

TEST_F(MorphDeformationHistoryTest, AMorphSetThatDoesNotSpanTheMeshIsRefused)
{
    // A set authored for a different mesh. Applied anyway it would deform whatever
    // vertices happen to line up, which reads as a broken rig.
    auto wrongSet = Ref<MorphTargetSet>::Create();
    wrongSet->AddTarget(MorphTarget("Smile", kVertexCount + 7));

    auto& meshSource = m_Entity.GetComponent<MeshComponent>().m_MeshSource;
    meshSource->SetMorphTargets(wrongSet);

    const auto& stats = Animation::SkeletalDeformationSystem::GetStats();
    const u32 before = stats.MorphIncompatibleSets;

    Morph().SetWeight("Smile", 1.0f);
    RunFrames(3);

    EXPECT_GT(stats.MorphIncompatibleSets, before)
        << "a morph set that does not span its mesh was accepted silently";
    EXPECT_NEAR(SurfaceVertices()[0].Position.y, 0.0f, 1e-4f)
        << "the mesh was deformed by a set authored for a different mesh";
}

TEST_F(MorphDeformationHistoryTest, SwappingTheSurfaceDropsTheCachedBaseSurface)
{
    Morph().SetWeight("Smile", 1.0f);
    RunFrames(4);
    ASSERT_FALSE(Morph().BasePositions.empty()) << "precondition: a base surface was cached";

    const auto& stats = Animation::SkeletalDeformationSystem::GetStats();
    const u32 before = stats.MorphBaseCacheInvalidations;

    // A different mesh, and therefore a different base surface. The cache from the
    // old mesh is in range for the new one, so reusing it is silent and wrong: it
    // would write the old mesh's rest positions into the new mesh's vertices.
    m_Entity.GetComponent<MeshComponent>().m_MeshSource = MakeMorphableQuad();
    RunFrames(3);

    EXPECT_GT(stats.MorphBaseCacheInvalidations, before)
        << "the base-surface cache survived a mesh swap — it is keyed on nothing that "
           "changes when the surface it describes is replaced";
    EXPECT_NEAR(SurfaceVertices()[1].Position.x, 1.0f, 1e-4f)
        << "the new mesh was written with the old mesh's cached base surface";
    EXPECT_NEAR(SurfaceVertices()[0].Position.y, kSmileDelta.y, 1e-4f)
        << "the new mesh was not deformed after the swap — the cache was dropped but never rebuilt";
}
