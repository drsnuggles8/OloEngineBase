#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// WaterWakeBuoyancyTest — Functional / cross-subsystem test for issue #968's
// acceptance criterion "a boat and buoy respond consistently when the
// physical-wake mode is enabled; visual-only mode is explicitly tested".
//
// Seam under test: the wake-shape records BoatWakeSystem publishes to
// WaterWakeSystem on the physics path, evaluated by WaterProbe::SampleSurfaceY
// and felt by BuoyancySystem — all inside a real Scene tick, with no renderer.
// That "no renderer" part is the point and is why the visual-only switch lives
// on WaterProbe::Volume rather than in WaterWakeSystem's published settings: a
// gate that only opens on the render path would be permanently shut here, and
// every assertion below would pass by agreeing that nothing happened.
//
// The two modes are asserted against EACH OTHER rather than against absolute
// heights. A wake is a small perturbation of a wavy sea, so an absolute
// threshold is a tuning constant in disguise; "the same buoy, the same tick
// count, the same everything except the switch" is the comparison that actually
// isolates the feature. The visual-only case is the negative control for the
// physical one and vice versa.
//
// Functional-test contract: ADR 0001/0002/0003, docs/testing.md section 7.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Physics3D/WaterProbe.h"
#include "OloEngine/Renderer/Water/WaterWake.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <cmath>
#include <initializer_list>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr f32 kBuoyHalfExtent = 0.4f;
    constexpr f32 kBuoyMass = 128.0f; // ~half the water it displaces => floats at the waterline
} // namespace

class WaterWakeBuoyancyTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Assembled per-test so each scenario's switches are explicit.
    }

    void TearDown() override
    {
        Time::ClearMockTime();
        // The records are process-wide static state, so a test that left hulls
        // standing would shape the sea in the NEXT test in this process — the
        // cross-scene inheritance defect Scene::OnRuntimeStart's Reset() exists
        // to prevent, arriving here as an unrelated flake instead.
        WaterWakeSystem::Reset();
        FunctionalTest::TearDown();
    }

    /// Flat-calm water (amplitude 0) so the ONLY thing that can displace the
    /// surface is the wake. A wavy sea would work too and would be more
    /// realistic, but then a failure could not distinguish "the wake did
    /// nothing" from "a crest happened to be there".
    Entity SpawnWater(bool wakeEnabled, bool affectsPhysics)
    {
        Entity water = GetScene().CreateEntity("Water");
        water.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
        auto& wc = water.AddComponent<WaterComponent>();
        wc.m_Enabled = true;
        wc.m_WorldSizeX = 400.0f;
        wc.m_WorldSizeZ = 400.0f;
        wc.m_WaveAmplitude = 0.0f;
        wc.m_WakeShapeEnabled = wakeEnabled;
        wc.m_WakeShapeAffectsPhysics = affectsPhysics;
        wc.m_WakeShapeHeightScale = 1.0f;
        wc.m_WakeShapeFlattenStrength = 0.9f;
        return water;
    }

    Entity SpawnBuoy(const glm::vec3& pos)
    {
        Entity buoy = GetScene().CreateEntity("Buoy");
        buoy.GetComponent<TransformComponent>().Translation = pos;

        auto& body = buoy.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = kBuoyMass;
        body.m_LinearDrag = 0.0f;
        body.m_AngularDrag = 0.0f;

        auto& col = buoy.AddComponent<BoxCollider3DComponent>();
        col.m_HalfExtents = glm::vec3(kBuoyHalfExtent);

        auto& b = buoy.AddComponent<BuoyancyComponent>();
        b.m_ProbeExtents = glm::vec3(kBuoyHalfExtent);
        b.m_FluidDensity = 1000.0f;
        b.m_SubmergenceRamp = 0.8f;
        b.m_LinearDrag = 5.0f; // settle inside the test window
        b.m_AngularDrag = 3.0f;
        return buoy;
    }

    /// Publish one stationary hull sitting right where the buoy is, with a
    /// strong bow bump ahead of it.
    ///
    /// Injected directly rather than driven through a BoatComponent: a real boat
    /// needs propulsion, a collider and several seconds of settling before its
    /// wake is worth measuring, and all of that is BoatSystem's behaviour rather
    /// than this seam's. What this test is for is the SAMPLING path —
    /// WaterProbe -> WaterWake::Evaluate -> BuoyancySystem — and injecting the
    /// record exercises exactly that with nothing else moving.
    /// BoatWakeSystem's own production of the record is covered by
    /// WaterWakeShapeTest and the visual evidence pass.
    static void PublishHullsAt(std::initializer_list<glm::vec2> centres, f32 speed)
    {
        WaterWakeSystem::BeginFrame();
        for (const glm::vec2 centreXZ : centres)
        {
            WaterWakeHullDesc desc;
            desc.m_CentreXZ = centreXZ;
            desc.m_ForwardXZ = { 0.0f, 1.0f };
            desc.m_HalfBeam = 1.4f;
            desc.m_HalfLength = 3.5f;
            desc.m_Speed = speed;
            desc.m_Gate = 1.0f;
            desc.m_ArmSampleCount = WaterWake::kMaxArmSamples;
            for (u32 i = 0; i < WaterWake::kMaxArmSamples; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(WaterWake::kMaxArmSamples - 1u);
                const f32 age = 0.15f + t * (2.5f - 0.15f);
                WaterWakeArmSample& s = desc.m_Arms[i];
                s.m_CentreXZ = centreXZ - glm::vec2(0.0f, speed * age);
                s.m_ForwardXZ = { 0.0f, 1.0f };
                s.m_AgeSeconds = age;
                s.m_Speed = speed;
                s.m_Gate = 1.0f;
            }
            ASSERT_TRUE(WaterWakeSystem::SubmitHull(desc));
        }
    }

    /// One hull, the common case.
    static void PublishHullUnder(glm::vec2 centreXZ, f32 speed)
    {
        PublishHullsAt({ centreXZ }, speed);
    }

    static f32 Y(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation.y;
    }
};

// =============================================================================
// The two modes, against each other.
// =============================================================================

TEST_F(WaterWakeBuoyancyTest, VisualOnlyModeLeavesBuoyancyExactlyAsItWas)
{
    // The wake is ON, and shaping what the shader draws — but the tile's
    // physics switch is clear, so a floating body must behave exactly as it did
    // before this feature existed. That is what "retain a visual-only switch
    // while parity is established" has to mean to be worth anything.
    Time::SetMockTime(0.0f);
    SpawnWater(/*wakeEnabled=*/true, /*affectsPhysics=*/false);

    // The buoy sits in the bow bump — the strongest part of the wake — so if
    // the switch leaked, this is where it would show.
    const glm::vec2 hullCentre{ 0.0f, -3.5f };
    Entity buoy = SpawnBuoy({ 0.0f, 2.0f, 0.0f });
    EnablePhysics3D();

    PublishHullUnder(hullCentre, 10.0f);

    // NEGATIVE CONTROL: the records really do raise the surface here. Without
    // this, a fixture whose wake happened to be zero at the buoy would pass
    // this test and the physical one below for opposite reasons.
    {
        const std::vector<WaterProbe::Volume> volumes = WaterProbe::CollectEnabledVolumes(&GetScene());
        ASSERT_EQ(volumes.size(), 1u);
        const WaterWake::Sample s =
            WaterWake::Evaluate(WaterWakeSystem::GetHullData(), WaterWakeSystem::GetHullCount(), 1.0f,
                                0.9f, { 0.0f, 0.0f }, 0.0f);
        ASSERT_GT(s.m_Height, 0.05f) << "the injected hull raises no water at the buoy";
        // ...and the tile reports the wake as physics-inert.
        ASSERT_FALSE(volumes[0].m_WakeAffectsPhysics);
        // The sampled surface is therefore the flat plane, wake or no wake.
        EXPECT_FLOAT_EQ(WaterProbe::SampleSurfaceY(volumes[0], { 0.0f, 0.0f }, 0.0f), 0.0f)
            << "visual-only mode leaked into the physics surface";
    }

    TickFor(6.0f);
    EXPECT_NEAR(Y(buoy), 0.0f, 0.25f)
        << "in visual-only mode the buoy must float at the flat waterline; y=" << Y(buoy);
}

TEST_F(WaterWakeBuoyancyTest, PhysicalModeLiftsTheBuoyOntoTheWake)
{
    // The same scene, the same hull, the same tick count — only the switch
    // differs. The buoy must end measurably HIGHER than the visual-only case,
    // because the surface under it is now raised by the bow bump.
    Time::SetMockTime(0.0f);
    SpawnWater(/*wakeEnabled=*/true, /*affectsPhysics=*/true);

    const glm::vec2 hullCentre{ 0.0f, -3.5f };
    Entity buoy = SpawnBuoy({ 0.0f, 2.0f, 0.0f });
    EnablePhysics3D();

    f32 expectedSurfaceY = 0.0f;
    {
        const std::vector<WaterProbe::Volume> volumes = WaterProbe::CollectEnabledVolumes(&GetScene());
        ASSERT_EQ(volumes.size(), 1u);
        ASSERT_TRUE(volumes[0].m_WakeAffectsPhysics);
        PublishHullUnder(hullCentre, 10.0f);
        expectedSurfaceY = WaterProbe::SampleSurfaceY(volumes[0], { 0.0f, 0.0f }, 0.0f);
    }

    // The discriminator has to clear the settle tolerance, or "rests on the
    // wake" and "rests on the flat plane" are the same measurement.
    ASSERT_GT(expectedSurfaceY, 0.15f)
        << "the wake raises the surface by less than the rest tolerance — this test cannot "
           "distinguish the two modes";

    // Republish every tick, as BoatWakeSystem does: BeginFrame drops the
    // previous frame's records, so a hull that stops being submitted stops
    // shaping the sea. Doing it any other way here would test a stale record
    // rather than a live one.
    for (int i = 0; i < 360; ++i) // 6 s at 60 Hz
    {
        PublishHullUnder(hullCentre, 10.0f);
        RunFrames(1);
    }

    EXPECT_NEAR(Y(buoy), expectedSurfaceY, 0.25f)
        << "the buoy did not settle on the wake surface; y=" << Y(buoy)
        << " expected~=" << expectedSurfaceY;
    EXPECT_GT(Y(buoy), 0.1f) << "the buoy is still at the flat waterline — the wake did not reach physics";
}

TEST_F(WaterWakeBuoyancyTest, ADisabledWakeIsIndistinguishableFromNoWakeAtAll)
{
    // The master switch off, with the physics switch ON, must still be a flat
    // sea. This is the combination a scene lands in by toggling the feature off
    // without also clearing the physics opt-in, and a wake that survived it
    // would keep pushing boats around invisibly.
    Time::SetMockTime(0.0f);
    SpawnWater(/*wakeEnabled=*/false, /*affectsPhysics=*/true);
    Entity buoy = SpawnBuoy({ 0.0f, 2.0f, 0.0f });
    EnablePhysics3D();

    PublishHullUnder({ 0.0f, -3.5f }, 10.0f);

    const std::vector<WaterProbe::Volume> volumes = WaterProbe::CollectEnabledVolumes(&GetScene());
    ASSERT_EQ(volumes.size(), 1u);
    EXPECT_FLOAT_EQ(volumes[0].m_WakeHeightScale, 0.0f);
    EXPECT_FLOAT_EQ(WaterProbe::SampleSurfaceY(volumes[0], { 0.0f, 0.0f }, 0.0f), 0.0f);

    TickFor(6.0f);
    EXPECT_NEAR(Y(buoy), 0.0f, 0.25f) << "a disabled wake still moved the buoy; y=" << Y(buoy);
}

TEST_F(WaterWakeBuoyancyTest, ABigAndASmallBodyAreLiftedTheSameAmountByTheSameWake)
{
    // "A boat and buoy respond consistently when the physical-wake mode is
    // enabled" — the criterion, measured as LIFT rather than as absolute height.
    //
    // Absolute height cannot express it: a big box and a small box float at
    // different heights for reasons that have nothing to do with the wake (the
    // corner-probe model's equilibrium depends on the probe extents), so a first
    // version compared the two directly, found them 0.41 m apart, and was
    // measuring buoyancy's own size response rather than this feature. Lift
    // subtracts that out — the sea here is flat calm, so a body far from any
    // boat rests at exactly its own no-wake height, and the same body near one
    // rests at that plus whatever the wake added.
    Time::SetMockTime(0.0f);
    SpawnWater(/*wakeEnabled=*/true, /*affectsPhysics=*/true);

    // Each body gets its OWN hull, directly astern of it, rather than the two
    // sharing one. The wake is strong only within a couple of metres of the bow,
    // and a first version put both bodies 2.2 m off ONE hull's beam, where the
    // height is ~0.1% of the bow bump: both "lifts" were settling noise and one
    // came out negative. Two identical hulls put each body in an identical bow
    // bump BY CONSTRUCTION — a stronger statement than "similar" — and 12 m
    // apart is far enough that the boxes cannot touch, which the 1.2 m spacing
    // of that first version was not (the contact was quietly part of the
    // measurement).
    constexpr f32 kLateral = 6.0f;
    constexpr f32 kFarZ = 60.0f; // well outside either wake's bounding circle
    const auto publish = [&]
    { PublishHullsAt({ { -kLateral, -3.5f }, { kLateral, -3.5f } }, 10.0f); };

    Entity smallNear = SpawnBuoy({ -kLateral, 2.0f, 0.0f });
    Entity smallFar = SpawnBuoy({ -kLateral, 2.0f, kFarZ });
    Entity largeNear = SpawnBuoy({ kLateral, 2.0f, 0.0f });
    Entity largeFar = SpawnBuoy({ kLateral, 2.0f, kFarZ });

    // Twice the extent, eight times the mass, so the large pair also floats at
    // its waterline. Different SIZE is the discriminator: a sampler that scaled
    // the wake by the body it was asked about — rather than reporting one shared
    // surface — would lift the two by different amounts, and two identical
    // bodies could never show that.
    for (Entity e : { largeNear, largeFar })
    {
        e.GetComponent<BuoyancyComponent>().m_ProbeExtents = glm::vec3(kBuoyHalfExtent * 2.0f);
        e.GetComponent<BoxCollider3DComponent>().m_HalfExtents = glm::vec3(kBuoyHalfExtent * 2.0f);
        e.GetComponent<Rigidbody3DComponent>().m_Mass = kBuoyMass * 8.0f;
    }
    EnablePhysics3D();

    // NEGATIVE CONTROLS, checked here and NOT after the settle loop.
    //
    // The records live for exactly one tick: BoatWakeSystem calls
    // WaterWakeSystem::BeginFrame() after the physics fence, so once a tick has
    // returned they are already cleared and every surface query reads a flat
    // sea. Asserting after the loop is how a first version of this reported "the
    // near bodies are not over a raised surface" against a working feature.
    // BuoyancySystem itself is unaffected — it runs inside the physics kick,
    // before the fence, while the records are still standing.
    publish();
    {
        const std::vector<WaterProbe::Volume> volumes = WaterProbe::CollectEnabledVolumes(&GetScene());
        ASSERT_EQ(volumes.size(), 1u);
        const f32 nearPort = WaterProbe::SampleSurfaceY(volumes[0], { -kLateral, 0.0f }, 0.0f);
        const f32 nearStarboard = WaterProbe::SampleSurfaceY(volumes[0], { kLateral, 0.0f }, 0.0f);
        ASSERT_FLOAT_EQ(WaterProbe::SampleSurfaceY(volumes[0], { -kLateral, kFarZ }, 0.0f), 0.0f)
            << "the far control bodies are inside a wake — 'lift' would be the difference between "
               "two lifted things";
        ASSERT_GT(nearPort, 0.1f) << "the near bodies are not over a raised surface";
        ASSERT_FLOAT_EQ(nearPort, nearStarboard)
            << "the two hulls are not raising the surface identically, so the two bodies are not "
               "being asked the same question";
    }

    for (int i = 0; i < 480; ++i) // 8 s at 60 Hz — the heavier pair settles slower
    {
        publish();
        RunFrames(1);
    }

    const f32 smallLift = Y(smallNear) - Y(smallFar);
    const f32 largeLift = Y(largeNear) - Y(largeFar);

    EXPECT_GT(smallLift, 0.05f) << "the small buoy was not lifted by the wake; lift=" << smallLift;
    EXPECT_GT(largeLift, 0.05f) << "the large buoy was not lifted by the wake; lift=" << largeLift;
    EXPECT_NEAR(smallLift, largeLift, 0.15f)
        << "the two bodies disagree about how much the same water rose under them: small lift="
        << smallLift << " large lift=" << largeLift;
}
