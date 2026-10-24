#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// IKTargetEntityResolvesToItsWorldPositionTest — Functional Test (found on
// issue #1559).
//
// Seam: IKTargetComponent::AimTargetEntity × Scene::ResolveIKTargets ×
// PropagateWorldTransforms. The IK solvers take WORLD-space targets (the post
// pass maps them into model space with the entity's world transform), but the
// resolver read the target entity's LOCAL translation. A target parented under
// anything -- a toy in a hand, a point riding a bone -- was aimed at as if its
// parent-relative offset were a world position.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class IKTargetEntityResolvesToItsWorldPositionTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Carrier = GetScene().CreateEntity("Carrier");
        m_Carrier.GetComponent<TransformComponent>().Translation = { 5.0f, 0.0f, 2.0f };
        m_Target = GetScene().CreateEntity("Target");
        m_Target.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        m_Target.SetParent(m_Carrier);
        m_Loose = GetScene().CreateEntity("Loose");
        m_Loose.GetComponent<TransformComponent>().Translation = { -3.0f, 2.0f, 1.0f };

        m_Aimer = GetScene().CreateEntity("Aimer");
        IKTargetComponent ik;
        ik.AimIKEnabled = true;
        ik.AimTargetEntity = m_Target.GetUUID();
        m_Aimer.AddComponent<IKTargetComponent>(ik);
    }

    Entity m_Carrier;
    Entity m_Target;
    Entity m_Loose;
    Entity m_Aimer;
};

TEST_F(IKTargetEntityResolvesToItsWorldPositionTest, AParentedTargetIsAimedAtWhereItIsDrawn)
{
    RunFrames(1); // compose the world transforms

    IKTargetComponent resolved;
    ASSERT_TRUE(GetScene().ResolveIKTargets(m_Aimer, resolved));
    EXPECT_NEAR(resolved.AimTarget.x, 5.0f, 1e-5f) << "the target's parent-relative offset was taken as a world position";
    EXPECT_NEAR(resolved.AimTarget.y, 1.0f, 1e-5f);
    EXPECT_NEAR(resolved.AimTarget.z, 2.0f, 1e-5f);

    // A root target is where its transform says: unchanged behaviour.
    m_Aimer.GetComponent<IKTargetComponent>().AimTargetEntity = m_Loose.GetUUID();
    ASSERT_TRUE(GetScene().ResolveIKTargets(m_Aimer, resolved));
    EXPECT_NEAR(glm::length(resolved.AimTarget - glm::vec3(-3.0f, 2.0f, 1.0f)), 0.0f, 1e-5f);
}
