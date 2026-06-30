#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// Unit coverage for the generalized snapshot interpolation registry (issue #462):
// the per-component policy table, the FNV-1a wire-id hashing, the multi-component
// self-describing snapshot round-trip, and the lerp/slerp/step interpolation
// semantics — all without a runtime tick (the Functional axis covers the live
// server→client replication path; see
// Functional/Networking/InterpolatedComponentsSmoothOnRemoteClientTest.cpp).

#include "OloEngine/Networking/Replication/ComponentInterpolationRegistry.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Replication/SnapshotInterpolator.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>

#include <set>

using namespace OloEngine;

// ── Registry contents + policy table ─────────────────────────────────────

TEST(ComponentInterpolationRegistryTest, DefaultsAreRegisteredWithExpectedPolicies)
{
    const auto* transform = ComponentInterpolationRegistry::FindByName("TransformComponent");
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->Policy, EInterpolationPolicy::Lerp);
    EXPECT_NE(transform->Interpolate, nullptr);
    EXPECT_NE(transform->Snap, nullptr);
    EXPECT_NE(transform->Smooth, nullptr); // transform eases during reconciliation

    const auto* rb3d = ComponentInterpolationRegistry::FindByName("Rigidbody3DComponent");
    ASSERT_NE(rb3d, nullptr);
    EXPECT_EQ(rb3d->Policy, EInterpolationPolicy::Lerp);

    const auto* anim = ComponentInterpolationRegistry::FindByName("AnimationStateComponent");
    ASSERT_NE(anim, nullptr);
    EXPECT_EQ(anim->Policy, EInterpolationPolicy::Step);
    EXPECT_EQ(anim->Smooth, nullptr); // discrete state — nothing to ease
}

TEST(ComponentInterpolationRegistryTest, WireIdsAreStableHashesAndDistinct)
{
    // Id == FNV-1a-32 of the name, so it's recomputable and order-independent.
    const auto* transform = ComponentInterpolationRegistry::FindByName("TransformComponent");
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->Id, ComponentInterpolationRegistry::HashName("TransformComponent"));

    // FindById round-trips the hash back to the same entry.
    const auto* byId = ComponentInterpolationRegistry::FindById(ComponentInterpolationRegistry::HashName("TransformComponent"));
    ASSERT_NE(byId, nullptr);
    EXPECT_EQ(byId->Name, "TransformComponent");

    // No id collisions across the default set.
    std::set<u32> ids;
    for (const auto& entry : ComponentInterpolationRegistry::GetEntries())
    {
        EXPECT_TRUE(ids.insert(entry.Id).second) << "duplicate wire id for " << entry.Name;
    }

    // Unknown id resolves to nullptr.
    EXPECT_EQ(ComponentInterpolationRegistry::FindById(0xDEADBEEF), nullptr);
}

class RegistryMutationTest : public ::testing::Test
{
  protected:
    // Clear() drops customs; the next read re-registers the defaults, so the
    // snapshot paths in other tests never observe an empty registry.
    void SetUp() override
    {
        ComponentInterpolationRegistry::Clear();
    }
    void TearDown() override
    {
        ComponentInterpolationRegistry::Clear();
    }
};

TEST_F(RegistryMutationTest, CustomRegistrationAppendsAfterDefaults)
{
    const sizet before = ComponentInterpolationRegistry::GetEntries().size();

    InterpolationEntry custom;
    custom.Name = "MyGameplayStateComponent";
    custom.Policy = EInterpolationPolicy::Step;
    ComponentInterpolationRegistry::Register(std::move(custom));

    EXPECT_EQ(ComponentInterpolationRegistry::GetEntries().size(), before + 1);
    const auto* found = ComponentInterpolationRegistry::FindByName("MyGameplayStateComponent");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->Id, ComponentInterpolationRegistry::HashName("MyGameplayStateComponent"));
}

TEST_F(RegistryMutationTest, DuplicateRegistrationIsIgnored)
{
    const sizet before = ComponentInterpolationRegistry::GetEntries().size();

    // Re-registering an existing name (here a default) must be a no-op — otherwise
    // snapshots would emit two copies while FindBy* only resolve the first.
    InterpolationEntry dup;
    dup.Name = "TransformComponent";
    dup.Policy = EInterpolationPolicy::Step; // deliberately wrong, to prove it's rejected
    ComponentInterpolationRegistry::Register(std::move(dup));

    EXPECT_EQ(ComponentInterpolationRegistry::GetEntries().size(), before);
    // The original Transform entry (Lerp) is retained, not the Step duplicate.
    const auto* transform = ComponentInterpolationRegistry::FindByName("TransformComponent");
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->Policy, EInterpolationPolicy::Lerp);
}

// ── Multi-component snapshot round-trip ──────────────────────────────────

TEST(ComponentInterpolationRegistryTest, SnapshotRoundTripsTransformRigidbodyAndAnimation)
{
    // Server scene: one replicated entity carrying all three default components.
    Scene server;
    Entity e = server.CreateEntityWithUUID(UUID(7777), "Networked");
    e.AddComponent<NetworkIdentityComponent>();
    {
        auto& t = e.GetComponent<TransformComponent>();
        t.Translation = { 1.0f, 2.0f, 3.0f };
        t.Scale = { 2.0f, 2.0f, 2.0f };

        auto& rb = e.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = BodyType3D::Dynamic;
        rb.m_InitialLinearVelocity = { 4.0f, 5.0f, 6.0f };

        auto& anim = e.AddComponent<AnimationStateComponent>();
        anim.m_State = AnimationStateComponent::State::Bounce;
        anim.m_CurrentClipIndex = 3;
        anim.m_CurrentTime = 0.75f;
        anim.m_IsPlaying = true;
    }

    const std::vector<u8> snapshot = EntitySnapshot::Capture(server);
    ASSERT_FALSE(snapshot.empty());

    // The parsed record carries all three components for the one entity.
    const ParsedSnapshot parsed = EntitySnapshot::Parse(snapshot);
    ASSERT_EQ(parsed.size(), 1u);
    ASSERT_TRUE(parsed.contains(7777u));
    EXPECT_EQ(parsed.at(7777u).size(), 3u);

    // Client scene: same UUID, sentinel values; Apply should restore all three.
    Scene client;
    Entity c = client.CreateEntityWithUUID(UUID(7777), "Mirror");
    c.AddComponent<NetworkIdentityComponent>();
    c.AddComponent<Rigidbody3DComponent>();
    c.AddComponent<AnimationStateComponent>();
    c.GetComponent<TransformComponent>().Translation = { -99.0f, -99.0f, -99.0f };

    EntitySnapshot::Apply(client, snapshot);

    const auto& ct = c.GetComponent<TransformComponent>();
    EXPECT_FLOAT_EQ(ct.Translation.x, 1.0f);
    EXPECT_FLOAT_EQ(ct.Translation.y, 2.0f);
    EXPECT_FLOAT_EQ(ct.Translation.z, 3.0f);
    EXPECT_FLOAT_EQ(ct.Scale.x, 2.0f);

    const auto& crb = c.GetComponent<Rigidbody3DComponent>();
    EXPECT_EQ(crb.m_Type, BodyType3D::Dynamic);
    EXPECT_FLOAT_EQ(crb.m_InitialLinearVelocity.x, 4.0f);
    EXPECT_FLOAT_EQ(crb.m_InitialLinearVelocity.y, 5.0f);
    EXPECT_FLOAT_EQ(crb.m_InitialLinearVelocity.z, 6.0f);

    const auto& canim = c.GetComponent<AnimationStateComponent>();
    EXPECT_EQ(canim.m_State, AnimationStateComponent::State::Bounce);
    EXPECT_EQ(canim.m_CurrentClipIndex, 3);
    EXPECT_FLOAT_EQ(canim.m_CurrentTime, 0.75f);
    EXPECT_TRUE(canim.m_IsPlaying);
}

// ── Per-component interpolation policy semantics ─────────────────────────

namespace
{
    // Capture the current component state of `e` as a one-entity snapshot.
    std::vector<u8> CaptureState(Scene& scene)
    {
        return EntitySnapshot::Capture(scene);
    }
} // namespace

TEST(ComponentInterpolationRegistryTest, InterpolateBlendsPerPolicyAtMidpoint)
{
    // A server-authoritative client-side entity with all three components.
    Scene scene;
    Entity e = scene.CreateEntityWithUUID(UUID(42), "Remote");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = true;
    nic.Authority = ENetworkAuthority::Server;
    e.AddComponent<Rigidbody3DComponent>();
    e.AddComponent<AnimationStateComponent>();

    // "Before" snapshot.
    {
        auto& t = e.GetComponent<TransformComponent>();
        t.Translation = { 0.0f, 0.0f, 0.0f };
        t.SetRotationEuler({ 0.0f, 0.0f, 0.0f });
        auto& rb = e.GetComponent<Rigidbody3DComponent>();
        rb.m_InitialLinearVelocity = { 0.0f, 0.0f, 0.0f };
        auto& anim = e.GetComponent<AnimationStateComponent>();
        anim.m_State = AnimationStateComponent::State::Idle;
        anim.m_CurrentClipIndex = 0;
    }
    const std::vector<u8> before = CaptureState(scene);

    // "After" snapshot — distinct values for every component.
    {
        auto& t = e.GetComponent<TransformComponent>();
        t.Translation = { 10.0f, 0.0f, 0.0f };
        t.SetRotationEuler({ 0.0f, glm::half_pi<float>(), 0.0f }); // 90° yaw
        auto& rb = e.GetComponent<Rigidbody3DComponent>();
        rb.m_InitialLinearVelocity = { 8.0f, 0.0f, 0.0f };
        auto& anim = e.GetComponent<AnimationStateComponent>();
        anim.m_State = AnimationStateComponent::State::Bounce;
        anim.m_CurrentClipIndex = 5;
    }
    const std::vector<u8> after = CaptureState(scene);

    // Reset to a sentinel so we can observe the interpolated write.
    {
        auto& t = e.GetComponent<TransformComponent>();
        t.Translation = { -1.0f, -1.0f, -1.0f };
        e.GetComponent<Rigidbody3DComponent>().m_InitialLinearVelocity = { -1.0f, -1.0f, -1.0f };
        e.GetComponent<AnimationStateComponent>().m_CurrentClipIndex = -1;
    }

    // Drive the interpolator at alpha = 0.5: tickRate 20, renderDelay 0.1
    // (2 ticks behind), snapshots at 10 and 14 → renderTick 12 → alpha (12-10)/4.
    SnapshotInterpolator interp;
    interp.SetServerTickRate(20);
    interp.SetRenderDelay(0.1f);
    interp.PushSnapshot(10, before);
    interp.PushSnapshot(14, after);
    interp.Interpolate(scene, 1.0f / 60.0f);

    // Lerp: translation at the midpoint.
    const auto& t = e.GetComponent<TransformComponent>();
    EXPECT_NEAR(t.Translation.x, 5.0f, 1e-3f) << "translation should lerp to the midpoint";

    // Slerp: 90° yaw halved to ~45°.
    EXPECT_NEAR(t.GetRotationEuler().y, glm::quarter_pi<float>(), 1e-3f) << "rotation should slerp halfway";

    // Lerp: rigidbody velocity at the midpoint.
    EXPECT_NEAR(e.GetComponent<Rigidbody3DComponent>().m_InitialLinearVelocity.x, 4.0f, 1e-3f)
        << "rigidbody velocity should lerp to the midpoint";

    // Step: animation holds the *before* value — no blend, no pop to the after value.
    const auto& anim = e.GetComponent<AnimationStateComponent>();
    EXPECT_EQ(anim.m_State, AnimationStateComponent::State::Idle) << "step policy holds the before state";
    EXPECT_EQ(anim.m_CurrentClipIndex, 0) << "step policy holds the before clip index";
}
