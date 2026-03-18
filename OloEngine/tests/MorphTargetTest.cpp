#include <gtest/gtest.h>
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetEvaluator.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Animation/MorphTargets/FacialExpressionLibrary.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSystem.h"
#include "OloEngine/Animation/AnimationClip.h"

using namespace OloEngine;

// =============================================================================
// MorphTarget Tests
// =============================================================================

TEST(MorphTargetTest, DefaultConstruction)
{
    MorphTarget target;
    EXPECT_TRUE(target.Name.empty());
    EXPECT_TRUE(target.Vertices.empty());
    EXPECT_FALSE(target.IsSparse);
}

TEST(MorphTargetTest, NamedConstruction)
{
    MorphTarget target("Smile", 100);
    EXPECT_EQ(target.Name, "Smile");
    EXPECT_EQ(target.Vertices.size(), 100u);
    EXPECT_FALSE(target.IsSparse);
}

TEST(MorphTargetTest, ConvertToSparse)
{
    MorphTarget target("Test", 5);
    // Only vertex 2 has non-zero delta
    target.Vertices[2].DeltaPosition = glm::vec3(1.0f, 0.0f, 0.0f);

    target.ConvertToSparse();
    EXPECT_TRUE(target.IsSparse);
    EXPECT_EQ(target.SparseVertices.size(), 1u);
    EXPECT_EQ(target.SparseVertices[0].VertexIndex, 2u);
    EXPECT_FLOAT_EQ(target.SparseVertices[0].Delta.DeltaPosition.x, 1.0f);
}

TEST(MorphTargetTest, ConvertToDense)
{
    MorphTarget target;
    target.Name = "Test";
    target.IsSparse = true;
    target.SparseVertices.push_back({ 3, { glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f) } });

    target.ConvertToDense(5);
    EXPECT_FALSE(target.IsSparse);
    EXPECT_EQ(target.Vertices.size(), 5u);
    EXPECT_FLOAT_EQ(target.Vertices[3].DeltaPosition.x, 0.5f);
    EXPECT_FLOAT_EQ(target.Vertices[0].DeltaPosition.x, 0.0f);
}

// =============================================================================
// MorphTargetSet Tests
// =============================================================================

TEST(MorphTargetSetTest, AddAndFindTarget)
{
    MorphTargetSet set;
    MorphTarget t1("Smile", 10);
    MorphTarget t2("Frown", 10);

    set.AddTarget(t1);
    set.AddTarget(t2);

    EXPECT_EQ(set.GetTargetCount(), 2u);
    EXPECT_EQ(set.FindTarget("Smile"), 0);
    EXPECT_EQ(set.FindTarget("Frown"), 1);
    EXPECT_EQ(set.FindTarget("Missing"), -1);
    EXPECT_TRUE(set.HasTarget("Smile"));
    EXPECT_FALSE(set.HasTarget("Missing"));
}

TEST(MorphTargetSetTest, GetVertexCount)
{
    MorphTargetSet set;
    EXPECT_EQ(set.GetVertexCount(), 0u);

    MorphTarget t("Test", 42);
    set.AddTarget(t);
    EXPECT_EQ(set.GetVertexCount(), 42u);
}

// =============================================================================
// MorphTargetEvaluator CPU Tests
// =============================================================================

TEST(MorphTargetEvaluatorTest, ZeroWeightsReturnBase)
{
    std::vector<glm::vec3> basePos = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    std::vector<glm::vec3> baseNrm = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } };

    MorphTargetSet targets;
    MorphTarget t("Test", 3);
    t.Vertices[0].DeltaPosition = glm::vec3(5.0f, 0.0f, 0.0f);
    targets.AddTarget(t);

    std::vector<f32> weights = { 0.0f };
    std::vector<glm::vec3> outPos, outNrm;

    MorphTargetEvaluator::EvaluateCPU(basePos, baseNrm, targets, weights, outPos, outNrm);

    EXPECT_EQ(outPos.size(), 3u);
    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_FLOAT_EQ(outPos[i].x, basePos[i].x);
        EXPECT_FLOAT_EQ(outPos[i].y, basePos[i].y);
        EXPECT_FLOAT_EQ(outPos[i].z, basePos[i].z);
    }
}

TEST(MorphTargetEvaluatorTest, SingleTargetFullWeight)
{
    std::vector<glm::vec3> basePos = { { 0, 0, 0 }, { 1, 0, 0 } };
    std::vector<glm::vec3> baseNrm = { { 0, 1, 0 }, { 0, 1, 0 } };

    MorphTargetSet targets;
    MorphTarget t("Move", 2);
    t.Vertices[0].DeltaPosition = glm::vec3(2.0f, 0.0f, 0.0f);
    t.Vertices[1].DeltaPosition = glm::vec3(0.0f, 3.0f, 0.0f);
    targets.AddTarget(t);

    std::vector<f32> weights = { 1.0f };
    std::vector<glm::vec3> outPos, outNrm;

    MorphTargetEvaluator::EvaluateCPU(basePos, baseNrm, targets, weights, outPos, outNrm);

    EXPECT_FLOAT_EQ(outPos[0].x, 2.0f); // 0 + 2*1
    EXPECT_FLOAT_EQ(outPos[1].y, 3.0f); // 0 + 3*1
}

TEST(MorphTargetEvaluatorTest, HalfWeight)
{
    std::vector<glm::vec3> basePos = { { 0, 0, 0 } };
    std::vector<glm::vec3> baseNrm = { { 0, 1, 0 } };

    MorphTargetSet targets;
    MorphTarget t("Half", 1);
    t.Vertices[0].DeltaPosition = glm::vec3(4.0f, 0.0f, 0.0f);
    targets.AddTarget(t);

    std::vector<f32> weights = { 0.5f };
    std::vector<glm::vec3> outPos, outNrm;

    MorphTargetEvaluator::EvaluateCPU(basePos, baseNrm, targets, weights, outPos, outNrm);

    EXPECT_FLOAT_EQ(outPos[0].x, 2.0f); // 0 + 4*0.5
}

TEST(MorphTargetEvaluatorTest, MultipleTargetsAdditive)
{
    std::vector<glm::vec3> basePos = { { 0, 0, 0 } };
    std::vector<glm::vec3> baseNrm = { { 0, 1, 0 } };

    MorphTargetSet targets;

    MorphTarget t1("A", 1);
    t1.Vertices[0].DeltaPosition = glm::vec3(1.0f, 0.0f, 0.0f);
    targets.AddTarget(t1);

    MorphTarget t2("B", 1);
    t2.Vertices[0].DeltaPosition = glm::vec3(0.0f, 2.0f, 0.0f);
    targets.AddTarget(t2);

    std::vector<f32> weights = { 1.0f, 1.0f };
    std::vector<glm::vec3> outPos, outNrm;

    MorphTargetEvaluator::EvaluateCPU(basePos, baseNrm, targets, weights, outPos, outNrm);

    EXPECT_FLOAT_EQ(outPos[0].x, 1.0f);
    EXPECT_FLOAT_EQ(outPos[0].y, 2.0f);
}

TEST(MorphTargetEvaluatorTest, SparseEvaluation)
{
    std::vector<glm::vec3> basePos = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } };
    std::vector<glm::vec3> baseNrm = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 } };

    MorphTargetSet targets;
    MorphTarget t("Sparse", 3);
    t.Vertices[1].DeltaPosition = glm::vec3(0.0f, 5.0f, 0.0f);
    t.ConvertToSparse();
    targets.AddTarget(t);

    std::vector<f32> weights = { 1.0f };
    std::vector<glm::vec3> outPos, outNrm;

    MorphTargetEvaluator::EvaluateCPU(basePos, baseNrm, targets, weights, outPos, outNrm);

    // Only vertex 1 should be affected
    EXPECT_FLOAT_EQ(outPos[0].x, 0.0f);
    EXPECT_FLOAT_EQ(outPos[0].y, 0.0f);
    EXPECT_FLOAT_EQ(outPos[1].x, 1.0f);
    EXPECT_FLOAT_EQ(outPos[1].y, 5.0f);
    EXPECT_FLOAT_EQ(outPos[2].x, 2.0f);
    EXPECT_FLOAT_EQ(outPos[2].y, 0.0f);
}

// =============================================================================
// MorphTargetComponent Tests
// =============================================================================

TEST(MorphTargetComponentTest, SetAndGetWeight)
{
    MorphTargetComponent comp;
    comp.SetWeight("Smile", 0.5f);

    EXPECT_FLOAT_EQ(comp.GetWeight("Smile"), 0.5f);
    EXPECT_FLOAT_EQ(comp.GetWeight("Missing"), 0.0f);
}

TEST(MorphTargetComponentTest, WeightClamping)
{
    MorphTargetComponent comp;
    comp.SetWeight("Test", 1.5f);
    EXPECT_FLOAT_EQ(comp.GetWeight("Test"), 1.0f);

    comp.SetWeight("Test", -0.5f);
    EXPECT_FLOAT_EQ(comp.GetWeight("Test"), 0.0f);
}

TEST(MorphTargetComponentTest, ResetAllWeights)
{
    MorphTargetComponent comp;
    comp.SetWeight("A", 0.5f);
    comp.SetWeight("B", 0.8f);

    comp.ResetAllWeights();
    EXPECT_FLOAT_EQ(comp.GetWeight("A"), 0.0f);
    EXPECT_FLOAT_EQ(comp.GetWeight("B"), 0.0f);
}

TEST(MorphTargetComponentTest, HasActiveWeights)
{
    MorphTargetComponent comp;
    EXPECT_FALSE(comp.HasActiveWeights());

    comp.SetWeight("A", 0.5f);
    EXPECT_TRUE(comp.HasActiveWeights());

    comp.ResetAllWeights();
    EXPECT_FALSE(comp.HasActiveWeights());
}

TEST(MorphTargetComponentTest, GetOrderedWeights)
{
    auto morphTargets = Ref<MorphTargetSet>::Create();
    morphTargets->AddTarget(MorphTarget("A", 1));
    morphTargets->AddTarget(MorphTarget("B", 1));
    morphTargets->AddTarget(MorphTarget("C", 1));

    MorphTargetComponent comp;
    comp.MorphTargets = morphTargets;
    comp.SetWeight("B", 0.7f);
    comp.SetWeight("C", 0.3f);

    auto ordered = comp.GetOrderedWeights();
    EXPECT_EQ(ordered.size(), 3u);
    EXPECT_FLOAT_EQ(ordered[0], 0.0f); // A
    EXPECT_FLOAT_EQ(ordered[1], 0.7f); // B
    EXPECT_FLOAT_EQ(ordered[2], 0.3f); // C
}

// =============================================================================
// FacialExpressionLibrary Tests
// =============================================================================

TEST(FacialExpressionLibraryTest, RegisterAndApply)
{
    FacialExpression happy;
    happy.Name = "Happy_Test";
    happy.TargetWeights["Smile"] = 1.0f;
    happy.TargetWeights["EyeWiden"] = 0.3f;

    FacialExpressionLibrary::RegisterExpression(happy);
    EXPECT_TRUE(FacialExpressionLibrary::HasExpression("Happy_Test"));

    MorphTargetComponent comp;
    FacialExpressionLibrary::ApplyExpression(comp, "Happy_Test");

    EXPECT_FLOAT_EQ(comp.GetWeight("Smile"), 1.0f);
    EXPECT_FLOAT_EQ(comp.GetWeight("EyeWiden"), 0.3f);
}

TEST(FacialExpressionLibraryTest, ApplyWithBlend)
{
    FacialExpression expr;
    expr.Name = "Blend_Test";
    expr.TargetWeights["Mouth"] = 1.0f;

    FacialExpressionLibrary::RegisterExpression(expr);

    MorphTargetComponent comp;
    FacialExpressionLibrary::ApplyExpression(comp, "Blend_Test", 0.5f);

    EXPECT_FLOAT_EQ(comp.GetWeight("Mouth"), 0.5f);
}

TEST(FacialExpressionLibraryTest, BlendBetweenExpressions)
{
    FacialExpression exprA;
    exprA.Name = "ExprA_Test";
    exprA.TargetWeights["Target"] = 1.0f;

    FacialExpression exprB;
    exprB.Name = "ExprB_Test";
    exprB.TargetWeights["Target"] = 0.0f;

    FacialExpressionLibrary::RegisterExpression(exprA);
    FacialExpressionLibrary::RegisterExpression(exprB);

    MorphTargetComponent comp;
    FacialExpressionLibrary::BlendExpressions(comp, "ExprA_Test", "ExprB_Test", 0.5f);

    // 1.0 * 0.5 + 0.0 * 0.5 = 0.5
    EXPECT_FLOAT_EQ(comp.GetWeight("Target"), 0.5f);
}

// =============================================================================
// MorphTargetKeyframe Tests
// =============================================================================

TEST(MorphTargetKeyframeTest, AnimationClipStoresMorphKeyframes)
{
    auto clip = Ref<AnimationClip>::Create();
    clip->Name = "TestClip";
    clip->Duration = 2.0f;

    clip->MorphKeyframes.push_back({ 0.0f, "Smile", 0.0f });
    clip->MorphKeyframes.push_back({ 1.0f, "Smile", 1.0f });
    clip->MorphKeyframes.push_back({ 2.0f, "Smile", 0.0f });

    EXPECT_EQ(clip->MorphKeyframes.size(), 3u);
    EXPECT_EQ(clip->MorphKeyframes[0].TargetName, "Smile");
    EXPECT_FLOAT_EQ(clip->MorphKeyframes[1].Weight, 1.0f);
}

// =============================================================================
// MorphTargetSystem Tests
// =============================================================================

TEST(MorphTargetSystemTest, SampleMorphKeyframesAtExactKeys)
{
    auto clip = Ref<AnimationClip>::Create();
    clip->Duration = 1.0f;
    clip->MorphKeyframes.push_back({ 0.0f, "Smile", 0.0f });
    clip->MorphKeyframes.push_back({ 1.0f, "Smile", 1.0f });

    MorphTargetComponent comp;

    // At time 0.0 -> weight should be 0.0
    MorphTargetSystem::SampleMorphKeyframes(clip, 0.0f, comp);
    EXPECT_FLOAT_EQ(comp.GetWeight("Smile"), 0.0f);

    // At time 1.0 -> weight should be 1.0
    MorphTargetSystem::SampleMorphKeyframes(clip, 1.0f, comp);
    EXPECT_FLOAT_EQ(comp.GetWeight("Smile"), 1.0f);
}

TEST(MorphTargetSystemTest, SampleMorphKeyframesInterpolated)
{
    auto clip = Ref<AnimationClip>::Create();
    clip->Duration = 1.0f;
    clip->MorphKeyframes.push_back({ 0.0f, "Blink", 0.0f });
    clip->MorphKeyframes.push_back({ 1.0f, "Blink", 1.0f });

    MorphTargetComponent comp;

    // At time 0.5 -> weight should be 0.5 (linear interpolation)
    MorphTargetSystem::SampleMorphKeyframes(clip, 0.5f, comp);
    EXPECT_NEAR(comp.GetWeight("Blink"), 0.5f, 1e-5f);
}

TEST(MorphTargetSystemTest, SampleMorphKeyframesMultipleTargets)
{
    auto clip = Ref<AnimationClip>::Create();
    clip->Duration = 1.0f;
    clip->MorphKeyframes.push_back({ 0.0f, "A", 0.0f });
    clip->MorphKeyframes.push_back({ 1.0f, "A", 1.0f });
    clip->MorphKeyframes.push_back({ 0.0f, "B", 1.0f });
    clip->MorphKeyframes.push_back({ 1.0f, "B", 0.0f });

    MorphTargetComponent comp;
    MorphTargetSystem::SampleMorphKeyframes(clip, 0.5f, comp);

    EXPECT_NEAR(comp.GetWeight("A"), 0.5f, 1e-5f);
    EXPECT_NEAR(comp.GetWeight("B"), 0.5f, 1e-5f);
}

TEST(MorphTargetSystemTest, EmptyClipDoesNotCrash)
{
    auto clip = Ref<AnimationClip>::Create();
    MorphTargetComponent comp;
    // Should not crash
    MorphTargetSystem::SampleMorphKeyframes(clip, 0.0f, comp);
    EXPECT_FALSE(comp.HasActiveWeights());
}

TEST(MorphTargetSystemTest, EvaluateMorphTargetsReturnsFalseWithNoActiveWeights)
{
    MorphTargetComponent comp;
    comp.MorphTargets = Ref<MorphTargetSet>::Create();

    std::vector<glm::vec3> basePos = { { 0, 0, 0 } };
    std::vector<glm::vec3> baseNrm = { { 0, 1, 0 } };
    std::vector<glm::vec3> outPos, outNrm;

    bool applied = MorphTargetSystem::EvaluateMorphTargets(comp, basePos, baseNrm, outPos, outNrm);
    EXPECT_FALSE(applied);
}

TEST(MorphTargetSystemTest, EvaluateMorphTargetsAppliesWeights)
{
    auto morphTargets = Ref<MorphTargetSet>::Create();
    MorphTarget t("Move", 2);
    t.Vertices[0].DeltaPosition = glm::vec3(10.0f, 0.0f, 0.0f);
    t.Vertices[1].DeltaPosition = glm::vec3(0.0f, 10.0f, 0.0f);
    morphTargets->AddTarget(t);

    MorphTargetComponent comp;
    comp.MorphTargets = morphTargets;
    comp.SetWeight("Move", 0.5f);

    std::vector<glm::vec3> basePos = { { 0, 0, 0 }, { 0, 0, 0 } };
    std::vector<glm::vec3> baseNrm = { { 0, 1, 0 }, { 0, 1, 0 } };
    std::vector<glm::vec3> outPos, outNrm;

    bool applied = MorphTargetSystem::EvaluateMorphTargets(comp, basePos, baseNrm, outPos, outNrm);
    EXPECT_TRUE(applied);
    EXPECT_FLOAT_EQ(outPos[0].x, 5.0f);
    EXPECT_FLOAT_EQ(outPos[1].y, 5.0f);
}

// =============================================================================
// GPU vs CPU Comparison Test (CPU reference)
// =============================================================================
// Note: Can't run actual GPU compute shader without an OpenGL context in unit tests.
// This test validates the CPU path produces deterministic results matching known expected
// values - the same values the GPU path should produce for identical inputs.

TEST(MorphTargetGPUvsCPUTest, CPUReferenceMatchesExpected)
{
    // Setup: 4 vertices, 2 morph targets
    std::vector<glm::vec3> basePos = {
        { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }
    };
    std::vector<glm::vec3> baseNrm = {
        { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }
    };

    MorphTargetSet targets;

    MorphTarget t1("TargetA", 4);
    t1.Vertices[0].DeltaPosition = glm::vec3(2.0f, 0.0f, 0.0f);
    t1.Vertices[1].DeltaPosition = glm::vec3(0.0f, 2.0f, 0.0f);
    t1.Vertices[2].DeltaPosition = glm::vec3(0.0f, 0.0f, 2.0f);
    t1.Vertices[3].DeltaPosition = glm::vec3(1.0f, 1.0f, 1.0f);
    targets.AddTarget(t1);

    MorphTarget t2("TargetB", 4);
    t2.Vertices[0].DeltaPosition = glm::vec3(0.0f, 0.0f, 3.0f);
    t2.Vertices[1].DeltaPosition = glm::vec3(0.0f, 0.0f, 0.0f);
    t2.Vertices[2].DeltaPosition = glm::vec3(1.0f, 0.0f, 0.0f);
    t2.Vertices[3].DeltaPosition = glm::vec3(0.0f, 2.0f, 0.0f);
    targets.AddTarget(t2);

    // Weights: target A at 0.5, target B at 0.8
    std::vector<f32> weights = { 0.5f, 0.8f };

    std::vector<glm::vec3> outPos, outNrm;
    MorphTargetEvaluator::EvaluateCPU(basePos, baseNrm, targets, weights, outPos, outNrm);

    // Expected: base + (deltaA * 0.5) + (deltaB * 0.8)
    // Vertex 0: (0,0,0) + (2,0,0)*0.5 + (0,0,3)*0.8 = (1.0, 0.0, 2.4)
    EXPECT_NEAR(outPos[0].x, 1.0f, 1e-5f);
    EXPECT_NEAR(outPos[0].y, 0.0f, 1e-5f);
    EXPECT_NEAR(outPos[0].z, 2.4f, 1e-5f);

    // Vertex 1: (1,0,0) + (0,2,0)*0.5 + (0,0,0)*0.8 = (1.0, 1.0, 0.0)
    EXPECT_NEAR(outPos[1].x, 1.0f, 1e-5f);
    EXPECT_NEAR(outPos[1].y, 1.0f, 1e-5f);
    EXPECT_NEAR(outPos[1].z, 0.0f, 1e-5f);

    // Vertex 2: (0,1,0) + (0,0,2)*0.5 + (1,0,0)*0.8 = (0.8, 1.0, 1.0)
    EXPECT_NEAR(outPos[2].x, 0.8f, 1e-5f);
    EXPECT_NEAR(outPos[2].y, 1.0f, 1e-5f);
    EXPECT_NEAR(outPos[2].z, 1.0f, 1e-5f);

    // Vertex 3: (1,1,0) + (1,1,1)*0.5 + (0,2,0)*0.8 = (1.5, 3.1, 0.5)
    EXPECT_NEAR(outPos[3].x, 1.5f, 1e-5f);
    EXPECT_NEAR(outPos[3].y, 3.1f, 1e-5f);
    EXPECT_NEAR(outPos[3].z, 0.5f, 1e-5f);
}
