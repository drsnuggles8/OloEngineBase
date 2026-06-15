#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure reasoning behind olo_physics_why_no_collision (issue
// #306 item A). The reasoning lives in a header-only free function with no Jolt /
// EnTT / editor dependencies precisely so it can be exercised here without a live
// editor, GPU, or physics simulation — the test binary compiles the MCP dispatch
// core but deliberately NOT McpTools.cpp (the editor-backed handlers). The live
// tools are verified separately over the MCP attach loop; this pins the verdict
// cascade that maps gathered facts -> root-cause explanation.
#include "MCP/McpPhysicsExplain.h"

#include <algorithm>
#include <string>

namespace
{
    using OloEngine::MCP::PhysicsExplain::BodyType;
    using OloEngine::MCP::PhysicsExplain::BodyTypeName;
    using OloEngine::MCP::PhysicsExplain::EntityPhysicsFacts;
    using OloEngine::MCP::PhysicsExplain::ExplainWhyNoCollision;
    using OloEngine::MCP::PhysicsExplain::WhyNoCollisionInput;
    using OloEngine::MCP::PhysicsExplain::WhyNoCollisionVerdict;

    // A fully collision-eligible entity: rigidbody + collider + live body, dynamic,
    // not a trigger, on a named layer.
    EntityPhysicsFacts MakeLiveBody(BodyType type = BodyType::Dynamic, const char* layer = "MOVING")
    {
        EntityPhysicsFacts f;
        f.EntityExists = true;
        f.HasRigidbody = true;
        f.HasCollider = true;
        f.HasBody = true;
        f.Type = type;
        f.IsTrigger = false;
        f.LayerName = layer;
        return f;
    }

    // The canonical "these two SHOULD collide and DO overlap" input. Each test
    // mutates exactly one field to isolate a single branch of the cascade.
    WhyNoCollisionInput MakeCollidable()
    {
        WhyNoCollisionInput in;
        in.SameEntity = false;
        in.PhysicsRunning = true;
        in.A = MakeLiveBody(BodyType::Dynamic, "MOVING");
        in.B = MakeLiveBody(BodyType::Static, "NON_MOVING");
        in.LayersCollide = true;
        in.BoundsOverlap = true;
        return in;
    }

    // True if any check trace line contains the substring (used to assert the
    // cascade recorded the steps it claims).
    bool HasCheckContaining(const WhyNoCollisionVerdict& v, const std::string& needle)
    {
        return std::any_of(v.Checks.begin(), v.Checks.end(),
                           [&](const std::string& line)
                           { return line.find(needle) != std::string::npos; });
    }
} // namespace

TEST(McpPhysicsExplain, BodyTypeNameCoversAllTypes)
{
    EXPECT_STREQ("Static", BodyTypeName(BodyType::Static));
    EXPECT_STREQ("Dynamic", BodyTypeName(BodyType::Dynamic));
    EXPECT_STREQ("Kinematic", BodyTypeName(BodyType::Kinematic));
}

TEST(McpPhysicsExplain, FullyEligibleAndOverlappingWouldCollide)
{
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(MakeCollidable());
    EXPECT_EQ("would_collide", v.ReasonCode);
    EXPECT_TRUE(v.CanCollide);
    EXPECT_FALSE(v.Summary.empty());
    // Every gate up to and including bounds-overlap should have passed.
    EXPECT_TRUE(HasCheckContaining(v, "[ok] the bodies' bounding volumes overlap"));
    // No failing checks on the happy path.
    EXPECT_FALSE(HasCheckContaining(v, "[fail]"));
}

TEST(McpPhysicsExplain, SameEntityIsRejectedFirst)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.SameEntity = true;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("same_entity", v.ReasonCode);
    EXPECT_FALSE(v.CanCollide);
    // Rejected before any later checks were recorded.
    EXPECT_EQ(1u, v.Checks.size());
}

TEST(McpPhysicsExplain, PhysicsNotRunning)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.PhysicsRunning = false;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("physics_not_running", v.ReasonCode);
    EXPECT_FALSE(v.CanCollide);
}

TEST(McpPhysicsExplain, EntityAMissing)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.A.EntityExists = false;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("entity_a_missing", v.ReasonCode);
    EXPECT_FALSE(v.CanCollide);
}

TEST(McpPhysicsExplain, EntityBMissing)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.B.EntityExists = false;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("entity_b_missing", v.ReasonCode);
}

TEST(McpPhysicsExplain, EntityANoRigidbody)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.A.HasRigidbody = false;
    in.A.HasCollider = false;
    in.A.HasBody = false;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("entity_a_no_rigidbody", v.ReasonCode);
    EXPECT_FALSE(v.CanCollide);
}

TEST(McpPhysicsExplain, EntityANoColliderReportedAfterRigidbody)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.A.HasRigidbody = true;
    in.A.HasCollider = false;
    in.A.HasBody = false;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("entity_a_no_collider", v.ReasonCode);
    EXPECT_TRUE(HasCheckContaining(v, "[ok] A has a Rigidbody3DComponent"));
}

TEST(McpPhysicsExplain, EntityANoLiveBody)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.A.HasRigidbody = true;
    in.A.HasCollider = true;
    in.A.HasBody = false;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("entity_a_no_body", v.ReasonCode);
}

TEST(McpPhysicsExplain, EntityASideCheckedBeforeEntityB)
{
    // Both sides broken: A lacks a rigidbody, B lacks a collider. The cascade must
    // surface A's root cause, not B's.
    WhyNoCollisionInput in = MakeCollidable();
    in.A.HasRigidbody = false;
    in.A.HasCollider = false;
    in.A.HasBody = false;
    in.B.HasCollider = false;
    in.B.HasBody = false;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("entity_a_no_rigidbody", v.ReasonCode);
}

TEST(McpPhysicsExplain, EntityBNoRigidbody)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.B.HasRigidbody = false;
    in.B.HasCollider = false;
    in.B.HasBody = false;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("entity_b_no_rigidbody", v.ReasonCode);
}

TEST(McpPhysicsExplain, BothStaticNeverCollide)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.A.Type = BodyType::Static;
    in.B.Type = BodyType::Static;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("both_static", v.ReasonCode);
    EXPECT_FALSE(v.CanCollide);
}

TEST(McpPhysicsExplain, StaticVsDynamicPassesTheStaticGate)
{
    // One static, one dynamic must NOT trip both_static.
    WhyNoCollisionInput in = MakeCollidable();
    in.A.Type = BodyType::Static;
    in.B.Type = BodyType::Dynamic;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_NE("both_static", v.ReasonCode);
    EXPECT_EQ("would_collide", v.ReasonCode);
}

TEST(McpPhysicsExplain, LayersDontCollide)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.LayersCollide = false;
    in.A.LayerName = "Player";
    in.B.LayerName = "Ghost";
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("layers_dont_collide", v.ReasonCode);
    EXPECT_FALSE(v.CanCollide);
    // The layer names should appear in the explanation so the user knows which pair.
    EXPECT_NE(std::string::npos, v.Summary.find("Player"));
    EXPECT_NE(std::string::npos, v.Summary.find("Ghost"));
}

TEST(McpPhysicsExplain, TriggerProducesNoSolidResponse)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.A.IsTrigger = true;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("trigger_no_solid_response", v.ReasonCode);
    EXPECT_FALSE(v.CanCollide);
}

TEST(McpPhysicsExplain, NotOverlappingCanStillCollide)
{
    WhyNoCollisionInput in = MakeCollidable();
    in.BoundsOverlap = false;
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(in);
    EXPECT_EQ("not_overlapping", v.ReasonCode);
    // They are eligible — just not touching right now.
    EXPECT_TRUE(v.CanCollide);
}

TEST(McpPhysicsExplain, ChecksAreOrderedAndPrefixed)
{
    const WhyNoCollisionVerdict v = ExplainWhyNoCollision(MakeCollidable());
    ASSERT_FALSE(v.Checks.empty());
    // First recorded check is always the distinct-entities gate.
    EXPECT_NE(std::string::npos, v.Checks.front().find("distinct entities"));
    // Every line carries one of the status prefixes.
    for (const std::string& line : v.Checks)
    {
        const bool prefixed = line.rfind("[ok] ", 0) == 0 || line.rfind("[fail] ", 0) == 0 || line.rfind("[warn] ", 0) == 0;
        EXPECT_TRUE(prefixed) << "unprefixed check line: " << line;
    }
}
