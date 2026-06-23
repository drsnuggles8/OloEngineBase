#include "OloEnginePCH.h"

// =============================================================================
// MorphTargetGraphAnimationTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × AnimationGraphSystem::Update × AnimationStateMachine ×
//   MorphTargetSystem × MorphTargetComponent.
//
// The animation-graph path (state machine / blend tree) is the modern way to
// drive a character. Before this integration it sampled bone poses but never
// the morph-target (blend-shape) keyframes carried on the *same* clip, so a
// graph-driven face never moved even though the infrastructure
// (MorphTarget/Set/Evaluator/System, Assimp extraction, AnimationClip::
// MorphKeyframes) all existed. The gap was the runtime seam:
// AnimationGraphSystem::Update never pushed sampled weights into the entity's
// MorphTargetComponent.
//
// Scenario: an entity with SkeletonComponent + AnimationGraphComponent (one
// SingleClip state whose clip ramps a "Smile" morph 0 → 1 over one second) +
// MorphTargetComponent. After ticking via Scene::OnUpdateRuntime, assert the
// component's "Smile" weight tracks the clip's playback time, that it loops,
// and that the sampled weight drives the CPU vertex deformation
// (MorphTargetSystem::EvaluateMorphTargets) — the full sample → evaluate path
// the renderer would later upload.
//
// A regression that drops the morph-sampling block from
// AnimationGraphSystem::Update (or mis-converts the state machine's normalized
// time back to clip time) leaves the weight pinned at zero and this fails.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationLayer.h"
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // One-second clip: a flat Root bone channel (so the SingleClip state has a
    // pose to evaluate) plus a single "Smile" morph track that ramps linearly
    // from 0 at t=0 to 1 at t=1. Built inline rather than in AnimationFixtures
    // because the morph track is specific to this test.
    Ref<AnimationClip> MakeSmileClip()
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "FacialExpression";
        clip->Duration = 1.0f;

        BoneAnimation boneAnim;
        boneAnim.BoneName = "Root";
        boneAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
        boneAnim.PositionKeys.push_back({ 1.0, glm::vec3(0.0f) });
        boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.RotationKeys.push_back({ 1.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
        boneAnim.ScaleKeys.push_back({ 1.0, glm::vec3(1.0f) });
        clip->BoneAnimations.push_back(std::move(boneAnim));
        clip->InitializeBoneCache();

        clip->MorphKeyframes.push_back({ 0.0, "Smile", 0.0f });
        clip->MorphKeyframes.push_back({ 1.0, "Smile", 1.0f });
        return clip;
    }
} // namespace

class MorphTargetGraphAnimationTest : public FunctionalTest
{
  protected:
    static constexpr const char* kStateName = "Smiling";

    void BuildScene() override
    {
        EnableAnimation();

        m_Animated = GetScene().CreateEntity("FacialCharacter");
        m_Animated.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());

        AnimationState state;
        state.Name = kStateName;
        state.Type = AnimationState::MotionType::SingleClip;
        state.Clip = MakeSmileClip();
        state.Looping = true;

        auto stateMachine = Ref<AnimationStateMachine>::Create();
        stateMachine->AddState(state);
        stateMachine->SetDefaultState(kStateName);

        auto graph = Ref<AnimationGraph>::Create();
        AnimationLayer layer;
        layer.Name = "Base";
        layer.StateMachine = stateMachine;
        graph->Layers.push_back(std::move(layer));

        AnimationGraphComponent graphComp;
        graphComp.RuntimeGraph = graph;
        m_Animated.AddComponent<AnimationGraphComponent>(std::move(graphComp));

        // Morph weights land here; no MorphTargetSet yet — sampling only needs
        // the component's Weights map. A set is attached per-test for the CPU
        // deformation case.
        m_Animated.AddComponent<MorphTargetComponent>();
    }

    [[nodiscard]] MorphTargetComponent& Morph()
    {
        return m_Animated.GetComponent<MorphTargetComponent>();
    }

    Entity m_Animated;
};

// The active clip's morph track is sampled into the component every tick, and
// the weight tracks the clip-space playback time (≈ elapsed seconds here, since
// Speed = 1 and Duration = 1).
TEST_F(MorphTargetGraphAnimationTest, GraphTickSamplesMorphWeightFromActiveClip)
{
    EXPECT_FLOAT_EQ(Morph().GetWeight("Smile"), 0.0f)
        << "weight should start at zero before any tick";

    // 30 frames @ 1/60s ≈ 0.5s into a 1s clip → Smile ≈ 0.5.
    RunFrames(30);

    EXPECT_NEAR(Morph().GetWeight("Smile"), 0.5f, 0.03f)
        << "graph tick did not sample the morph track from the active clip — "
           "AnimationGraphSystem::Update's morph-sampling seam is missing or the "
           "normalized-time → clip-time conversion is wrong.";
}

// Weight follows the clip forward, then drops after the loop point — proves the
// time fed to the sampler is the live, looping state-machine clock, not a
// frozen value.
TEST_F(MorphTargetGraphAnimationTest, MorphWeightTracksClipTimeAndLoops)
{
    RunFrames(54); // ≈ 0.9s → near the top of the ramp
    const f32 nearEnd = Morph().GetWeight("Smile");
    EXPECT_GT(nearEnd, 0.8f) << "weight should be high near the end of the ramp";

    RunFrames(18); // total ≈ 1.2s → wrapped past Duration, back to ≈ 0.2s
    const f32 afterLoop = Morph().GetWeight("Smile");
    EXPECT_LT(afterLoop, nearEnd)
        << "weight did not drop after the clip looped — the state machine clock "
           "is not advancing/looping through the morph sampler.";
    EXPECT_NEAR(afterLoop, 0.2f, 0.05f);
}

// End-to-end: the graph-sampled weight drives the CPU morph deformation that
// the renderer would upload. Base vertex at origin, "Smile" pushes it +2 in Y;
// the deformed Y must equal 2 × sampled-weight.
TEST_F(MorphTargetGraphAnimationTest, SampledWeightDrivesCpuVertexDeformation)
{
    RunFrames(30); // Smile ≈ 0.5
    const f32 sampledWeight = Morph().GetWeight("Smile");
    ASSERT_GT(sampledWeight, 0.01f) << "precondition: graph must have sampled a non-zero weight";

    // Attach a single-vertex morph set whose "Smile" target displaces +2 in Y.
    auto morphSet = Ref<MorphTargetSet>::Create();
    MorphTarget smile("Smile", 1);
    smile.Vertices[0].DeltaPosition = glm::vec3(0.0f, 2.0f, 0.0f);
    ASSERT_TRUE(morphSet->AddTarget(std::move(smile)));
    Morph().MorphTargets = morphSet;

    const std::vector<glm::vec3> basePositions = { glm::vec3(0.0f) };
    const std::vector<glm::vec3> baseNormals = { glm::vec3(0.0f, 1.0f, 0.0f) };
    std::vector<glm::vec3> outPositions;
    std::vector<glm::vec3> outNormals;

    const bool deformed = MorphTargetSystem::EvaluateMorphTargets(
        Morph(), basePositions, baseNormals, outPositions, outNormals);

    ASSERT_TRUE(deformed) << "CPU evaluation should run with an active weight + morph set";
    ASSERT_EQ(outPositions.size(), 1u);
    EXPECT_NEAR(outPositions[0].y, 2.0f * sampledWeight, 1e-4f)
        << "deformed vertex did not scale with the graph-sampled weight — the "
           "sample → evaluate integration is broken.";
    EXPECT_NEAR(outPositions[0].x, 0.0f, 1e-4f);
    EXPECT_NEAR(outPositions[0].z, 0.0f, 1e-4f);
}
