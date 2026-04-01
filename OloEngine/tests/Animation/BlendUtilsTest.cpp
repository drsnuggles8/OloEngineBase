#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/BlendUtils.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Animation;

//==============================================================================
// Transform Math Tests
//==============================================================================

TEST(BlendUtilsTest, TransformPointAppliesTRS)
{
    BoneTransform t;
    t.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
    t.Rotation = glm::identity<glm::quat>();
    t.Scale = glm::vec3(2.0f);

    auto result = BlendUtils::TransformPoint(t, glm::vec3(1.0f, 0.0f, 0.0f));

    EXPECT_NEAR(result.x, 12.0f, 1e-5f); // 10 + 2*1
    EXPECT_NEAR(result.y, 0.0f, 1e-5f);
    EXPECT_NEAR(result.z, 0.0f, 1e-5f);
}

TEST(BlendUtilsTest, TransformVectorIgnoresTranslation)
{
    BoneTransform t;
    t.Translation = glm::vec3(10.0f, 20.0f, 30.0f);
    t.Rotation = glm::identity<glm::quat>();
    t.Scale = glm::vec3(3.0f);

    auto result = BlendUtils::TransformVector(t, glm::vec3(1.0f, 0.0f, 0.0f));

    EXPECT_NEAR(result.x, 3.0f, 1e-5f); // Only scale, no translation
    EXPECT_NEAR(result.y, 0.0f, 1e-5f);
    EXPECT_NEAR(result.z, 0.0f, 1e-5f);
}

TEST(BlendUtilsTest, InverseTransformUndoesOriginal)
{
    BoneTransform t;
    t.Translation = glm::vec3(5.0f, -3.0f, 7.0f);
    t.Rotation = glm::angleAxis(glm::radians(45.0f), glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)));
    t.Scale = glm::vec3(2.0f, 0.5f, 1.5f);

    auto inv = BlendUtils::InverseTransform(t);
    auto identity = BlendUtils::MultiplyTransforms(t, inv);

    EXPECT_NEAR(identity.Translation.x, 0.0f, 1e-4f);
    EXPECT_NEAR(identity.Translation.y, 0.0f, 1e-4f);
    EXPECT_NEAR(identity.Translation.z, 0.0f, 1e-4f);
    EXPECT_NEAR(std::abs(glm::dot(identity.Rotation, glm::identity<glm::quat>())), 1.0f, 1e-4f);
    EXPECT_NEAR(identity.Scale.x, 1.0f, 1e-4f);
    EXPECT_NEAR(identity.Scale.y, 1.0f, 1e-4f);
    EXPECT_NEAR(identity.Scale.z, 1.0f, 1e-4f);
}

TEST(BlendUtilsTest, MultiplyTransformsIsAssociative)
{
    BoneTransform a = { glm::vec3(1, 2, 3), glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0)), glm::vec3(1.0f) };
    BoneTransform b = { glm::vec3(-1, 0, 2), glm::angleAxis(glm::radians(60.0f), glm::vec3(1, 0, 0)), glm::vec3(1.0f) };
    BoneTransform c = { glm::vec3(0, 5, -1), glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)), glm::vec3(1.0f) };

    auto ab_c = BlendUtils::MultiplyTransforms(BlendUtils::MultiplyTransforms(a, b), c);
    auto a_bc = BlendUtils::MultiplyTransforms(a, BlendUtils::MultiplyTransforms(b, c));

    EXPECT_NEAR(ab_c.Translation.x, a_bc.Translation.x, 1e-4f);
    EXPECT_NEAR(ab_c.Translation.y, a_bc.Translation.y, 1e-4f);
    EXPECT_NEAR(ab_c.Translation.z, a_bc.Translation.z, 1e-4f);
    EXPECT_NEAR(std::abs(glm::dot(ab_c.Rotation, a_bc.Rotation)), 1.0f, 1e-4f);
}

//==============================================================================
// ComputeModelSpacePose Tests
//==============================================================================

TEST(BlendUtilsTest, ModelSpacePoseRootIsLocal)
{
    std::vector<BoneTransform> local = {
        { glm::vec3(1, 2, 3), glm::identity<glm::quat>(), glm::vec3(1) }
    };
    std::vector<int> parents = { -1 };

    std::vector<BoneTransform> modelSpace;
    BlendUtils::ComputeModelSpacePose(local, parents, modelSpace);

    EXPECT_NEAR(modelSpace[0].Translation.x, 1.0f, 1e-5f);
    EXPECT_NEAR(modelSpace[0].Translation.y, 2.0f, 1e-5f);
    EXPECT_NEAR(modelSpace[0].Translation.z, 3.0f, 1e-5f);
}

TEST(BlendUtilsTest, ModelSpacePoseChildChainsCorrectly)
{
    std::vector<BoneTransform> local = {
        { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) },
        { glm::vec3(0.0f, 2.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) },
        { glm::vec3(0.0f, 3.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) }
    };
    std::vector<int> parents = { -1, 0, 1 };

    std::vector<BoneTransform> modelSpace;
    BlendUtils::ComputeModelSpacePose(local, parents, modelSpace);

    EXPECT_NEAR(modelSpace[0].Translation.y, 0.0f, 1e-5f);
    EXPECT_NEAR(modelSpace[1].Translation.y, 2.0f, 1e-5f);
    EXPECT_NEAR(modelSpace[2].Translation.y, 5.0f, 1e-5f); // 2 + 3
}

TEST(BlendUtilsTest, ComputeModelSpaceTransformMatchesFullPose)
{
    std::vector<BoneTransform> local = {
        { glm::vec3(1, 0, 0), glm::identity<glm::quat>(), glm::vec3(1) },
        { glm::vec3(0, 2, 0), glm::identity<glm::quat>(), glm::vec3(1) },
        { glm::vec3(0, 0, 3), glm::identity<glm::quat>(), glm::vec3(1) }
    };
    std::vector<int> parents = { -1, 0, 1 };

    std::vector<BoneTransform> modelSpace;
    BlendUtils::ComputeModelSpacePose(local, parents, modelSpace);

    auto single = BlendUtils::ComputeModelSpaceTransform(2, local, parents);

    EXPECT_NEAR(single.Translation.x, modelSpace[2].Translation.x, 1e-5f);
    EXPECT_NEAR(single.Translation.y, modelSpace[2].Translation.y, 1e-5f);
    EXPECT_NEAR(single.Translation.z, modelSpace[2].Translation.z, 1e-5f);
}

//==============================================================================
// LerpPose Tests
//==============================================================================

TEST(BlendUtilsTest, LerpPoseWeight0ReturnsA)
{
    BoneTransform a = { glm::vec3(1, 2, 3), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform b = { glm::vec3(10, 20, 30), glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)), glm::vec3(2) };

    std::vector<BoneTransform> poseA = { a };
    std::vector<BoneTransform> poseB = { b };
    std::vector<BoneTransform> result(1);

    BlendUtils::LerpPose(poseA, poseB, 0.0f, result);

    EXPECT_NEAR(result[0].Translation.x, 1.0f, 1e-5f);
    EXPECT_NEAR(result[0].Translation.y, 2.0f, 1e-5f);
    EXPECT_NEAR(result[0].Translation.z, 3.0f, 1e-5f);
    EXPECT_NEAR(result[0].Scale.x, 1.0f, 1e-5f);
}

TEST(BlendUtilsTest, LerpPoseWeight1ReturnsB)
{
    BoneTransform a = { glm::vec3(1, 2, 3), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform b = { glm::vec3(10, 20, 30), glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)), glm::vec3(2) };

    std::vector<BoneTransform> poseA = { a };
    std::vector<BoneTransform> poseB = { b };
    std::vector<BoneTransform> result(1);

    BlendUtils::LerpPose(poseA, poseB, 1.0f, result);

    EXPECT_NEAR(result[0].Translation.x, 10.0f, 1e-5f);
    EXPECT_NEAR(result[0].Translation.y, 20.0f, 1e-5f);
    EXPECT_NEAR(result[0].Translation.z, 30.0f, 1e-5f);
    EXPECT_NEAR(result[0].Scale.x, 2.0f, 1e-5f);
}

TEST(BlendUtilsTest, LerpPoseWeightHalfInterpolates)
{
    BoneTransform a = { glm::vec3(0, 0, 0), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform b = { glm::vec3(10, 0, 0), glm::identity<glm::quat>(), glm::vec3(3) };

    std::vector<BoneTransform> poseA = { a };
    std::vector<BoneTransform> poseB = { b };
    std::vector<BoneTransform> result(1);

    BlendUtils::LerpPose(poseA, poseB, 0.5f, result);

    EXPECT_NEAR(result[0].Translation.x, 5.0f, 1e-5f);
    EXPECT_NEAR(result[0].Scale.x, 2.0f, 1e-5f);
}

//==============================================================================
// AdditivePose Tests
//==============================================================================

TEST(BlendUtilsTest, AdditiveWeight0ReturnsBase)
{
    // With weight 0, result should equal base
    BoneTransform base = { glm::vec3(1, 2, 3), glm::angleAxis(0.5f, glm::vec3(0, 1, 0)), glm::vec3(1) };
    BoneTransform additive = { glm::vec3(10, 20, 30), glm::angleAxis(1.5f, glm::vec3(1, 0, 0)), glm::vec3(2) };
    BoneTransform rest = { glm::vec3(0), glm::identity<glm::quat>(), glm::vec3(1) };

    std::vector<BoneTransform> basePose = { base };
    std::vector<BoneTransform> addPose = { additive };
    std::vector<BoneTransform> restPose = { rest };
    std::vector<int> parents = { -1 };
    std::vector<BoneTransform> result(1);

    BlendUtils::AdditivePose(basePose, addPose, restPose, 0.0f, 0, parents, result);

    EXPECT_NEAR(result[0].Translation.x, base.Translation.x, 1e-5f);
    EXPECT_NEAR(result[0].Translation.y, base.Translation.y, 1e-5f);
    EXPECT_NEAR(result[0].Translation.z, base.Translation.z, 1e-5f);
    EXPECT_NEAR(std::abs(glm::dot(result[0].Rotation, base.Rotation)), 1.0f, 1e-4f);
    EXPECT_NEAR(result[0].Scale.x, base.Scale.x, 1e-5f);
}

TEST(BlendUtilsTest, AdditiveRestPoseChangesNothing)
{
    // Adding the rest pose itself (delta = 0) should return base unchanged
    BoneTransform base = { glm::vec3(1, 2, 3), glm::angleAxis(0.5f, glm::vec3(0, 1, 0)), glm::vec3(1.5f) };
    BoneTransform rest = { glm::vec3(0), glm::identity<glm::quat>(), glm::vec3(1) };

    std::vector<BoneTransform> basePose = { base };
    std::vector<BoneTransform> restPose = { rest };
    std::vector<int> parents = { -1 };
    std::vector<BoneTransform> result(1);

    // Additive with additive=rest → delta is zero
    BlendUtils::AdditivePose(basePose, restPose, restPose, 1.0f, 0, parents, result);

    EXPECT_NEAR(result[0].Translation.x, base.Translation.x, 1e-4f);
    EXPECT_NEAR(result[0].Translation.y, base.Translation.y, 1e-4f);
    EXPECT_NEAR(result[0].Translation.z, base.Translation.z, 1e-4f);
    EXPECT_NEAR(std::abs(glm::dot(result[0].Rotation, base.Rotation)), 1.0f, 1e-3f);
    EXPECT_NEAR(result[0].Scale.x, base.Scale.x, 1e-4f);
}

TEST(BlendUtilsTest, AdditiveTranslationAddsWeightedDelta)
{
    BoneTransform base = { glm::vec3(1, 0, 0), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform additive = { glm::vec3(5, 0, 0), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform rest = { glm::vec3(0), glm::identity<glm::quat>(), glm::vec3(1) };

    std::vector<BoneTransform> basePose = { base };
    std::vector<BoneTransform> addPose = { additive };
    std::vector<BoneTransform> restPose = { rest };
    std::vector<int> parents = { -1 };
    std::vector<BoneTransform> result(1);

    BlendUtils::AdditivePose(basePose, addPose, restPose, 1.0f, 0, parents, result);

    // result = base + 1.0 * (additive - rest) = (1,0,0) + (5,0,0) = (6,0,0)
    EXPECT_NEAR(result[0].Translation.x, 6.0f, 1e-5f);
}

//==============================================================================
// MaskedLerpPose Tests
//==============================================================================

TEST(BlendUtilsTest, MaskedLerpRootBone0BlendsAll)
{
    BoneTransform a = { glm::vec3(0), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform b = { glm::vec3(10, 0, 0), glm::identity<glm::quat>(), glm::vec3(1) };

    std::vector<BoneTransform> poseA = { a, a };
    std::vector<BoneTransform> poseB = { b, b };
    std::vector<int> parents = { -1, 0 };
    std::vector<BoneTransform> result(2);

    BlendUtils::MaskedLerpPose(poseA, poseB, 0.5f, 0, parents, result);

    // Both bones should be blended
    EXPECT_NEAR(result[0].Translation.x, 5.0f, 1e-5f);
    EXPECT_NEAR(result[1].Translation.x, 5.0f, 1e-5f);
}

TEST(BlendUtilsTest, MaskedLerpOnlyAffectsSubtree)
{
    BoneTransform a = { glm::vec3(0), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform b = { glm::vec3(10, 0, 0), glm::identity<glm::quat>(), glm::vec3(1) };

    // 4-bone skeleton:
    // 0 (root) -> 1 (left arm) -> 2 (left hand)
    //          -> 3 (right arm)
    std::vector<BoneTransform> poseA = { a, a, a, a };
    std::vector<BoneTransform> poseB = { b, b, b, b };
    std::vector<int> parents = { -1, 0, 1, 0 };
    std::vector<BoneTransform> result(4);

    // Mask: only bone 1 and its descendants (bone 2)
    BlendUtils::MaskedLerpPose(poseA, poseB, 1.0f, 1, parents, result);

    // Bone 0: unaffected (root, parent of mask root)
    EXPECT_NEAR(result[0].Translation.x, 0.0f, 1e-5f);
    // Bone 1: affected (mask root)
    EXPECT_NEAR(result[1].Translation.x, 10.0f, 1e-5f);
    // Bone 2: affected (child of mask root)
    EXPECT_NEAR(result[2].Translation.x, 10.0f, 1e-5f);
    // Bone 3: unaffected (sibling of mask root)
    EXPECT_NEAR(result[3].Translation.x, 0.0f, 1e-5f);
}

TEST(BlendUtilsTest, MaskedLerpAlpha0LeavesAllUnchanged)
{
    BoneTransform a = { glm::vec3(1, 2, 3), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform b = { glm::vec3(10, 20, 30), glm::identity<glm::quat>(), glm::vec3(1) };

    std::vector<BoneTransform> poseA = { a, a };
    std::vector<BoneTransform> poseB = { b, b };
    std::vector<int> parents = { -1, 0 };
    std::vector<BoneTransform> result(2);

    BlendUtils::MaskedLerpPose(poseA, poseB, 0.0f, 0, parents, result);

    EXPECT_NEAR(result[0].Translation.x, 1.0f, 1e-5f);
    EXPECT_NEAR(result[1].Translation.x, 1.0f, 1e-5f);
}

//==============================================================================
// Additive Masking Tests
//==============================================================================

TEST(BlendUtilsTest, AdditiveMaskOnlyAffectsSubtree)
{
    BoneTransform base = { glm::vec3(1, 0, 0), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform additive = { glm::vec3(5, 0, 0), glm::identity<glm::quat>(), glm::vec3(1) };
    BoneTransform rest = { glm::vec3(0), glm::identity<glm::quat>(), glm::vec3(1) };

    // 3-bone chain: 0 -> 1 -> 2
    std::vector<BoneTransform> basePose = { base, base, base };
    std::vector<BoneTransform> addPose = { additive, additive, additive };
    std::vector<BoneTransform> restPose = { rest, rest, rest };
    std::vector<int> parents = { -1, 0, 1 };
    std::vector<BoneTransform> result(3);

    // Apply additive only starting from bone 1
    BlendUtils::AdditivePose(basePose, addPose, restPose, 1.0f, 1, parents, result);

    // Bone 0: unaffected, should be base
    EXPECT_NEAR(result[0].Translation.x, 1.0f, 1e-5f);
    // Bone 1: affected, should be base + (additive - rest) = 1 + 5 = 6
    EXPECT_NEAR(result[1].Translation.x, 6.0f, 1e-5f);
    // Bone 2: affected (descendant of bone 1)
    EXPECT_NEAR(result[2].Translation.x, 6.0f, 1e-5f);
}
