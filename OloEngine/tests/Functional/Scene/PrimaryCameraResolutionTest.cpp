#include "OloEnginePCH.h"

// =============================================================================
// PrimaryCameraResolutionTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::GetPrimaryCameraEntity() × multi-camera scenes × runtime camera
//   swapping. Editor + runtime workflows commonly have several cameras in
//   one scene (main game camera, security cameras, cinematics). Only the
//   one with `Primary == true` should drive the render path. A regression
//   that picks the first camera regardless of Primary, or fails to update
//   when Primary is reassigned, silently breaks every cinematic-toggle
//   and security-camera-handoff feature.
//
// Scenario: build three cameras, only the second one Primary. Verify
// GetPrimaryCameraEntity returns the second one. Toggle Primary onto a
// different camera. Verify the lookup tracks the change.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class PrimaryCameraResolutionTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // CameraComponent's `Primary` field defaults to TRUE (Components.h),
        // which is a surprising default — every newly-spawned camera claims
        // Primary unless you explicitly opt out. We disable it on A and C so
        // only B is the active primary at construction time.
        m_CamA = GetScene().CreateEntity("CameraA");
        m_CamA.AddComponent<CameraComponent>().Primary = false;

        m_CamB = GetScene().CreateEntity("CameraB");
        auto& camB = m_CamB.AddComponent<CameraComponent>();
        camB.Primary = true;

        m_CamC = GetScene().CreateEntity("CameraC");
        m_CamC.AddComponent<CameraComponent>().Primary = false;
    }

    Entity m_CamA, m_CamB, m_CamC;
};

TEST_F(PrimaryCameraResolutionTest, ReturnsTheCameraWithPrimaryTrueAndTracksHandoff)
{
    // Phase 1: B is the primary.
    Entity primary = GetScene().GetPrimaryCameraEntity();
    ASSERT_TRUE(primary) << "no primary camera resolved even though one CameraComponent has Primary=true";
    EXPECT_EQ(primary.GetUUID(), m_CamB.GetUUID())
        << "Scene chose the wrong primary camera; expected CameraB ("
        << static_cast<u64>(m_CamB.GetUUID()) << ") got "
        << static_cast<u64>(primary.GetUUID());

    // Phase 2: handoff. B steps down, C takes over.
    m_CamB.GetComponent<CameraComponent>().Primary = false;
    m_CamC.GetComponent<CameraComponent>().Primary = true;

    primary = GetScene().GetPrimaryCameraEntity();
    ASSERT_TRUE(primary) << "after handoff, no primary camera resolved";
    EXPECT_EQ(primary.GetUUID(), m_CamC.GetUUID())
        << "Scene did not pick up the new primary camera after Primary flag was "
           "swapped at runtime";

    // Phase 3: nobody primary. Should resolve to a null/invalid Entity.
    m_CamC.GetComponent<CameraComponent>().Primary = false;
    primary = GetScene().GetPrimaryCameraEntity();
    EXPECT_FALSE(primary)
        << "Scene returned a camera even though no CameraComponent has Primary=true "
           "— the per-frame code that uses this lookup will follow a stale camera.";
}
