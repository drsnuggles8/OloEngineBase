#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// StructuralCollapseTest — Functional Test (issue #786).
//
// Cross-subsystem seam under test:
//   Gameplay (damage) × the structural support solver × Physics3D (a condemned
//   piece switching from Static to Dynamic and falling) × Scene/ECS (structural
//   spawn+destroy through the gameplay scheduler) × the #459 debris budget.
//
// The behaviour that matters is emergent and its failure mode is quiet: a
// support flood that is subtly wrong still produces something that LOOKS like a
// collapse. So these tests pin the SHAPE of the collapse, not just that one
// happened — which column comes down, which one does not, how many nodes the
// solver looked at, and how much debris a big collapse is allowed to make.
//
// These run through the real Scene::OnUpdateRuntime tick (RunFrames), so the
// solver executes on the scheduled "Destructible" node with physics live — the
// same path the shipping game takes.
//
// Everything here addresses pieces by UUID, never by a stored Entity. A piece
// that collapses is DESTROYED, and Entity::operator bool only checks for a null
// handle — HasComponent on a destroyed entity is an EnTT assert, not a false.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Gameplay/Destruction/DestructibleSystem.h"
#include "OloEngine/Gameplay/Destruction/StructuralGraph.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr f32 kBlock = 1.0f; // unit block: a 1 m cube, so grid spacing is 1 m

    // One block of a structure: a unit cube with a box collider, a Static
    // rigidbody (so it holds the building up until it is condemned), a
    // DestructibleComponent (so it can shatter) and a StructuralNodeComponent
    // (so it takes part in the support graph).
    //
    // Note the component order: the rigidbody goes on BEFORE the structural
    // node, which is the awkward order on purpose — a piece assembled this way
    // gets a Jolt body with no MotionProperties, and the release path has to
    // rebuild it rather than assert.
    UUID MakeBlock(Scene& scene, const std::string& name, const glm::vec3& pos, bool anchor,
                   f32 collapseDelay = 0.0f, f32 fallDuration = 0.0f)
    {
        Entity e = scene.CreateEntity(name);
        e.GetComponent<TransformComponent>().Translation = pos;

        BoxCollider3DComponent box;
        box.m_HalfExtents = glm::vec3(0.5f * kBlock);
        e.AddComponent<BoxCollider3DComponent>(box);

        Rigidbody3DComponent rb;
        rb.m_Type = BodyType3D::Static;
        rb.m_Mass = 10.0f;
        e.AddComponent<Rigidbody3DComponent>(rb);

        auto& dc = e.AddComponent<DestructibleComponent>();
        dc.m_Health = 100.0f;
        dc.m_MaxHealth = 100.0f;
        dc.m_ChunkCount = 4;
        dc.m_ChunkScale = 0.2f;
        dc.m_ExplosionImpulse = 0.5f;
        dc.m_DebrisLifetime = 30.0f; // long, so the budget test counts spawns, not decay

        auto& node = e.AddComponent<StructuralNodeComponent>();
        node.m_Anchor = anchor;
        // Zero delays by default: a contract test wants the collapse to resolve
        // in a bounded number of ticks, not to look pretty. The stagger has its
        // own test (CollapseRipplesOutwardFromTheBreak).
        node.m_CollapseDelay = collapseDelay;
        node.m_FallDuration = fallDuration;
        return e.GetUUID();
    }
} // namespace

class StructuralCollapseTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        Entity floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent fb;
        fb.m_Type = BodyType3D::Static;
        BoxCollider3DComponent fc;
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(fc);
        floor.AddComponent<Rigidbody3DComponent>(fb);

        EnablePhysics3D();
    }

    [[nodiscard]] std::optional<Entity> Resolve(UUID id)
    {
        return GetScene().TryGetEntityWithUUID(id);
    }

    // Never condemned: the solver has always found it a path to an anchor.
    [[nodiscard]] bool IsIntact(UUID id)
    {
        auto e = Resolve(id);
        return e && e->HasComponent<StructuralNodeComponent>() && e->GetComponent<StructuralNodeComponent>().m_State == StructuralState::Stable;
    }

    // Condemned but not yet cut loose — still standing, still load-bearing.
    [[nodiscard]] bool IsCondemned(UUID id)
    {
        auto e = Resolve(id);
        return e && e->HasComponent<StructuralNodeComponent>() && e->GetComponent<StructuralNodeComponent>().m_State == StructuralState::Detaching;
    }

    // Cut loose from the structure: falling, shattered, or already gone.
    [[nodiscard]] bool HasLeftStructure(UUID id)
    {
        auto e = Resolve(id);
        if (!e || !e->HasComponent<StructuralNodeComponent>())
            return true; // destroyed by its own shatter
        const auto state = e->GetComponent<StructuralNodeComponent>().m_State;
        return state == StructuralState::Falling || state == StructuralState::Collapsed;
    }

    [[nodiscard]] u32 CollapseHops(UUID id)
    {
        auto e = Resolve(id);
        return (e && e->HasComponent<StructuralNodeComponent>()) ? e->GetComponent<StructuralNodeComponent>().m_CollapseHops : 0u;
    }

    [[nodiscard]] bool Damage(UUID id, f32 amount)
    {
        auto e = Resolve(id);
        return e && DestructibleSystem::ApplyDamage(&GetScene(), *e, amount);
    }

    [[nodiscard]] u32 CountDebris()
    {
        u32 n = 0;
        for (auto e : GetScene().GetAllEntitiesWith<DebrisComponent>())
        {
            (void)e;
            ++n;
        }
        return n;
    }

    // A free-standing column of `height` blocks at x, the bottom one anchored.
    // Returned bottom-to-top.
    std::vector<UUID> MakeColumn(f32 x, u32 height, const char* prefix)
    {
        std::vector<UUID> column;
        for (u32 y = 0; y < height; ++y)
        {
            column.push_back(MakeBlock(GetScene(), std::string(prefix) + std::to_string(y),
                                       { x, 0.5f * kBlock + static_cast<f32>(y) * kBlock, 0.0f },
                                       /*anchor=*/y == 0));
        }
        return column;
    }
};

// The load-bearing case: knock out the bottom of a column and everything above
// it comes down, because nothing above it has a path to the anchor any more.
// Nothing else in the scene moves.
TEST_F(StructuralCollapseTest, RemovingASupportCollapsesTheColumnAboveIt)
{
    // Two columns four metres apart, so their bounds never touch and they are
    // two separate structures.
    std::vector<UUID> hit = MakeColumn(0.0f, 4, "Hit");
    std::vector<UUID> spare = MakeColumn(4.0f, 4, "Spare");

    RunFrames(1);
    EXPECT_EQ(GetScene().GetStructuralGraph().IslandCount, 2u)
        << "two columns four metres apart must be two independent structures";

    // Destroy the anchor block at the base of the first column.
    ASSERT_TRUE(Damage(hit[0], 500.0f));

    // Zero collapse delays, so each tick condemns and drops one more level.
    RunFrames(8);

    for (sizet i = 1; i < hit.size(); ++i)
    {
        EXPECT_TRUE(HasLeftStructure(hit[i]))
            << "block " << i << " of the hit column lost its only path to the anchor and must come down";
    }
    for (sizet i = 0; i < spare.size(); ++i)
    {
        EXPECT_TRUE(IsIntact(spare[i]))
            << "block " << i << " of the untouched column must not move";
    }
}

// A piece with two independent supports survives losing one. This is the test
// that fails if the solver is really asking "did my direct supporter break?"
// rather than "is there any path to an anchor?".
TEST_F(StructuralCollapseTest, APieceWithTwoSupportsSurvivesLosingOne)
{
    // Two anchored legs two metres apart in x, with a bonded row bridging their
    // tops:
    //   LA LM LB     y = 1.5
    //   A  .  B      y = 0.5   (anchors)
    UUID legA = MakeBlock(GetScene(), "LegA", { 0.0f, 0.5f, 0.0f }, /*anchor=*/true);
    UUID legB = MakeBlock(GetScene(), "LegB", { 2.0f, 0.5f, 0.0f }, /*anchor=*/true);
    UUID lintelA = MakeBlock(GetScene(), "LintelA", { 0.0f, 1.5f, 0.0f }, false);
    UUID lintelMid = MakeBlock(GetScene(), "LintelMid", { 1.0f, 1.5f, 0.0f }, false);
    UUID lintelB = MakeBlock(GetScene(), "LintelB", { 2.0f, 1.5f, 0.0f }, false);

    RunFrames(1);

    // Take out one leg. The row is still reachable from the other one, along the
    // same-course edges that make a bonded course share load.
    ASSERT_TRUE(Damage(legA, 500.0f));
    RunFrames(8);

    EXPECT_TRUE(IsIntact(legB)) << "the surviving anchor must not move";
    EXPECT_TRUE(IsIntact(lintelB)) << "the end still sitting on an anchor must hold";
    EXPECT_TRUE(IsIntact(lintelMid)) << "the row must cantilever from the surviving leg, not drop";
    EXPECT_TRUE(IsIntact(lintelA)) << "the far end is still reachable through the bonded course";

    // Now take the other leg. With no anchor left, the whole span comes down.
    ASSERT_TRUE(Damage(legB, 500.0f));
    RunFrames(8);

    EXPECT_TRUE(HasLeftStructure(lintelA));
    EXPECT_TRUE(HasLeftStructure(lintelMid));
    EXPECT_TRUE(HasLeftStructure(lintelB));
}

// Sideways load transfer is bounded. A bonded row hanging off one anchor stands
// only as far as its m_MaxLateralSpan reaches; past that it is unsupported even
// though it is still connected to the anchor. This is the test that fails if the
// flood is reduced back to plain reachability — under which the whole row would
// survive, and a wall could never partially collapse.
TEST_F(StructuralCollapseTest, ABondedRowStandsOnlyAsFarAsItsLateralSpanReaches)
{
    // One anchored leg, with a row of six bonded pieces running off its top.
    // Their sideways cost from the anchor is 0, 1, 2, 3, 4, 5.
    constexpr u32 kSpan = 2;
    UUID leg = MakeBlock(GetScene(), "Leg", { 0.0f, 0.5f, 0.0f }, /*anchor=*/true);
    std::vector<UUID> row;
    for (u32 i = 0; i < 6; ++i)
    {
        UUID id = MakeBlock(GetScene(), "Row" + std::to_string(i),
                            { static_cast<f32>(i) * kBlock, 1.5f, 0.0f }, false);
        Resolve(id)->GetComponent<StructuralNodeComponent>().m_MaxLateralSpan = kSpan;
        row.push_back(id);
    }

    // A second, disjoint piece whose removal makes the solver run at all — the
    // row is only re-checked when something in its island leaves, so give the
    // island a piece to lose that is not part of what we are measuring.
    UUID sacrificial = MakeBlock(GetScene(), "Sacrificial", { 0.0f, 2.5f, 0.0f }, false);
    Resolve(sacrificial)->GetComponent<StructuralNodeComponent>().m_MaxLateralSpan = kSpan;

    RunFrames(1);
    ASSERT_TRUE(Damage(sacrificial, 500.0f));
    RunFrames(8);

    // row[0] sits directly on the leg (cost 0); row[1] and row[2] are 1 and 2
    // sideways steps, still inside the budget.
    EXPECT_TRUE(IsIntact(row[0])) << "the piece sitting on the anchor must hold";
    EXPECT_TRUE(IsIntact(row[1])) << "one sideways step is inside a span of 2";
    EXPECT_TRUE(IsIntact(row[2])) << "two sideways steps is exactly the span";

    // row[3..5] are 3, 4 and 5 steps out — past the budget, so they fall even
    // though they are still touching a piece that is standing.
    EXPECT_TRUE(HasLeftStructure(row[3])) << "three sideways steps exceeds a span of 2 and must come down";
    EXPECT_TRUE(HasLeftStructure(row[4]));
    EXPECT_TRUE(HasLeftStructure(row[5]));
}

// A ring of pieces holding each other up with no path to the ground is not
// stable — mutual support is not support. This is the cycle case, and it is
// exactly what a naive "is anything touching me?" check gets wrong.
TEST_F(StructuralCollapseTest, ACycleWithNoAnchorPathCollapsesEntirely)
{
    // A 2x2 clique of pieces, all mutually adjacent, none anchored, floating at
    // y = 4 so nothing else can reach them.
    std::vector<UUID> ring;
    ring.push_back(MakeBlock(GetScene(), "R0", { 0.0f, 4.0f, 0.0f }, false));
    ring.push_back(MakeBlock(GetScene(), "R1", { 1.0f, 4.0f, 0.0f }, false));
    ring.push_back(MakeBlock(GetScene(), "R2", { 1.0f, 4.0f, 1.0f }, false));
    ring.push_back(MakeBlock(GetScene(), "R3", { 0.0f, 4.0f, 1.0f }, false));

    // A separate anchored column, so the scene is not degenerate and the ring's
    // collapse cannot be blamed on "nothing is anchored anywhere".
    std::vector<UUID> column = MakeColumn(8.0f, 3, "Col");

    RunFrames(1);

    // Break one ring member; the rest have only each other.
    ASSERT_TRUE(Damage(ring[0], 500.0f));
    RunFrames(8);

    for (sizet i = 1; i < ring.size(); ++i)
        EXPECT_TRUE(HasLeftStructure(ring[i])) << "ring member " << i << " has no anchor path and must collapse";
    for (sizet i = 0; i < column.size(); ++i)
        EXPECT_TRUE(IsIntact(column[i])) << "the anchored column is a different structure and must not move";
}

// The scoping contract: a break re-floods only the structure that lost a piece.
// LastSolveVisitedNodes is the solver saying how many nodes it looked at, and it
// must stay at the size of ONE structure however many others are in the scene.
TEST_F(StructuralCollapseTest, ASolveVisitsOnlyTheIslandThatLostAPiece)
{
    constexpr u32 kColumnHeight = 6;
    constexpr u32 kColumns = 6;
    std::vector<UUID> hit = MakeColumn(0.0f, kColumnHeight, "Hit");
    for (u32 c = 1; c < kColumns; ++c)
        (void)MakeColumn(4.0f * static_cast<f32>(c), kColumnHeight, "Other");

    RunFrames(1);
    StructuralGraph& graph = GetScene().GetStructuralGraph();
    ASSERT_EQ(graph.NodeIDs.size(), static_cast<sizet>(kColumnHeight) * kColumns);
    ASSERT_EQ(graph.IslandCount, kColumns);

    const u64 solvesBefore = graph.TotalSolves;
    ASSERT_TRUE(Damage(hit[0], 500.0f));
    RunFrames(1);

    EXPECT_GT(graph.TotalSolves, solvesBefore) << "the break must trigger a re-solve";
    EXPECT_EQ(graph.LastSolveIslands, 1u) << "only the structure that lost a piece may be re-solved";
    EXPECT_LE(graph.LastSolveVisitedNodes, kColumnHeight)
        << "the solve must stay inside one column (" << kColumnHeight << " blocks), not walk all "
        << graph.NodeIDs.size() << " nodes in the scene";
}

// Progressive, not simultaneous: with a per-hop collapse delay the pieces
// nearest the break go first while the far end is still standing.
TEST_F(StructuralCollapseTest, CollapseRipplesOutwardFromTheBreak)
{
    // A single anchored leg with a long bonded run resting on its top, so losing
    // the leg condemns the whole run at increasing hop distances.
    constexpr f32 kDelay = 0.25f; // per hop
    UUID leg = MakeBlock(GetScene(), "Leg", { 0.0f, 0.5f, 0.0f }, /*anchor=*/true, kDelay);
    std::vector<UUID> span;
    for (u32 i = 0; i < 5; ++i)
    {
        span.push_back(MakeBlock(GetScene(), "Span" + std::to_string(i),
                                 { static_cast<f32>(i) * kBlock, 1.5f, 0.0f }, false, kDelay));
    }

    RunFrames(1);
    ASSERT_TRUE(Damage(leg, 500.0f));
    RunFrames(1);

    // The whole run is condemned at once — the solver is not staggered, the
    // DETACHMENT is. Hop distance is what spreads it over time.
    EXPECT_TRUE(IsCondemned(span[0]));
    EXPECT_TRUE(IsCondemned(span[4]));
    EXPECT_GT(CollapseHops(span[4]), CollapseHops(span[0]))
        << "hop distance must grow with distance from the break";

    // span[0] detaches at 1 hop x 0.25 s x 2 = 0.5 s; span[4] at 5 hops => 1.5 s.
    // Sample in between: the near end has gone, the far end has not.
    TickFor(0.8f);
    EXPECT_TRUE(HasLeftStructure(span[0])) << "the piece sitting on the break must be the first to go";
    EXPECT_TRUE(IsCondemned(span[4])) << "the far end must still be standing while the near end comes down";

    // Given enough time the whole run is down.
    TickFor(2.5f);
    for (sizet i = 0; i < span.size(); ++i)
        EXPECT_TRUE(HasLeftStructure(span[i])) << "span piece " << i << " must eventually collapse";
}

// A condemned piece with a rigidbody detaches and FALLS as one body before it
// shatters — it does not blink out of existence into debris.
TEST_F(StructuralCollapseTest, ADetachedPieceFallsAsARigidbodyBeforeShattering)
{
    UUID leg = MakeBlock(GetScene(), "Leg", { 0.0f, 0.5f, 0.0f }, /*anchor=*/true);
    UUID top = MakeBlock(GetScene(), "Top", { 0.0f, 1.5f, 0.0f }, false,
                         /*collapseDelay=*/0.0f, /*fallDuration=*/1.0f);

    RunFrames(1);
    const f32 restingY = Resolve(top)->GetComponent<TransformComponent>().Translation.y;

    ASSERT_TRUE(Damage(leg, 500.0f));
    RunFrames(2);

    ASSERT_TRUE(Resolve(top).has_value());
    EXPECT_EQ(Resolve(top)->GetComponent<StructuralNodeComponent>().m_State, StructuralState::Falling)
        << "the condemned piece must be released to physics";
    EXPECT_EQ(Resolve(top)->GetComponent<Rigidbody3DComponent>().m_Type, BodyType3D::Dynamic)
        << "a released piece must be switched from Static to Dynamic, or it hangs in the air";
    const u32 debrisFromTheLeg = CountDebris();
    EXPECT_EQ(debrisFromTheLeg, 4u) << "only the block that was actually hit has shattered so far";

    // Half a second of fall: it must have moved down, and still be one body.
    TickFor(0.5f);
    ASSERT_TRUE(Resolve(top).has_value()) << "the piece must not shatter before its fall duration is up";
    EXPECT_LT(Resolve(top)->GetComponent<TransformComponent>().Translation.y, restingY - 0.05f)
        << "a detached piece must actually fall under gravity";
    EXPECT_EQ(Resolve(top)->GetComponent<StructuralNodeComponent>().m_State, StructuralState::Falling);

    // Past the fall duration it hands over to the #459 debris path, which
    // destroys the source (m_DestroyOnBreak defaults true).
    TickFor(1.0f);
    EXPECT_FALSE(Resolve(top).has_value()) << "the fall must end in a shatter that consumes the piece";
    EXPECT_GT(CountDebris(), debrisFromTheLeg) << "the shatter must go through the existing debris path";
}

// The budget is a hard invariant during a collapse too. A structure with far
// more potential debris than the cap must never exceed it — the collapse feeds
// the SAME budget the #459 path enforces, it does not get its own allowance.
TEST_F(StructuralCollapseTest, ALargeCollapseStaysInsideTheDebrisBudget)
{
    // 12 x 4 = 48 blocks x 16 chunks each = 768 potential debris pieces against
    // a cap of 256.
    constexpr u32 kColumns = 12;
    constexpr u32 kHeight = 4;
    std::vector<UUID> bases;
    for (u32 c = 0; c < kColumns; ++c)
    {
        for (u32 y = 0; y < kHeight; ++y)
        {
            UUID b = MakeBlock(GetScene(), "B" + std::to_string(c) + "_" + std::to_string(y),
                               { static_cast<f32>(c) * kBlock, 0.5f + static_cast<f32>(y) * kBlock, 0.0f },
                               /*anchor=*/y == 0);
            Resolve(b)->GetComponent<DestructibleComponent>().m_ChunkCount = 16;
            if (y == 0)
                bases.push_back(b);
        }
    }

    RunFrames(1);

    // Take out every anchor at once: the whole wall is condemned.
    for (UUID b : bases)
        ASSERT_TRUE(Damage(b, 500.0f));

    u32 peak = 0;
    for (u32 frame = 0; frame < 120; ++frame)
    {
        RunFrames(1);
        const u32 live = CountDebris();
        peak = std::max(peak, live);
        ASSERT_LE(live, DestructibleSystem::kMaxLiveDebris)
            << "live debris exceeded the global budget on frame " << frame
            << " — a collapse must not get its own allowance";
    }

    EXPECT_GT(peak, 0u) << "the wall must actually have produced debris";
}

// A structure with no anchor at all comes down on the first break rather than
// standing on nothing. The solver also warns about it (see StructuralGraph), but
// the BEHAVIOUR is what a scene depends on, so pin that.
TEST_F(StructuralCollapseTest, AnUnanchoredStructureComesDownOnTheFirstBreak)
{
    std::vector<UUID> stack;
    for (u32 y = 0; y < 4; ++y)
    {
        stack.push_back(MakeBlock(GetScene(), "S" + std::to_string(y),
                                  { 0.0f, 0.5f + static_cast<f32>(y) * kBlock, 0.0f }, /*anchor=*/false));
    }

    RunFrames(1);
    // Nothing has disturbed it yet, so it stands — the solver only runs when a
    // piece leaves. That is deliberate; an unanchored structure is caught by the
    // warning, not by collapsing the moment the scene loads.
    EXPECT_TRUE(IsIntact(stack[0]));

    ASSERT_TRUE(Damage(stack[3], 500.0f));
    RunFrames(8);

    for (sizet i = 0; i < 3; ++i)
        EXPECT_TRUE(HasLeftStructure(stack[i])) << "block " << i << " of an unanchored stack must collapse";
}

// A destructible with no StructuralNodeComponent keeps the #459 behaviour
// exactly: it shatters in isolation and drags nothing down with it. Composition,
// not replacement.
TEST_F(StructuralCollapseTest, APlainDestructibleIsUnaffectedByTheSolver)
{
    Entity loose = GetScene().CreateEntity("LooseCrate");
    loose.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
    auto& dc = loose.AddComponent<DestructibleComponent>();
    dc.m_ChunkCount = 5;
    dc.m_ChunkScale = 0.2f;
    dc.m_DestroyOnBreak = false;
    const UUID looseID = loose.GetUUID();

    // A structural piece right next to it must not be dragged down, because a
    // piece with no StructuralNodeComponent is not in the graph at all.
    UUID neighbour = MakeBlock(GetScene(), "Neighbour", { 1.0f, 1.0f, 0.0f }, /*anchor=*/true);

    RunFrames(1);
    EXPECT_EQ(GetScene().GetStructuralGraph().NodeIDs.size(), 1u)
        << "only the entity carrying a StructuralNodeComponent belongs to the graph";

    ASSERT_TRUE(Damage(looseID, 500.0f));
    RunFrames(4);

    EXPECT_EQ(CountDebris(), 5u) << "a plain destructible must still shatter into exactly its chunk count";
    EXPECT_TRUE(IsIntact(neighbour)) << "a non-structural break must not propagate into the graph";
}

// The staleness test must key on WHICH pieces exist, not how many. A tick that
// destroys one piece and creates another leaves the count unchanged while the
// graph is thoroughly wrong: the destroyed piece still counted as holding up its
// neighbours, and the new one absent from the index so a break on it seeds
// nothing at all.
TEST_F(StructuralCollapseTest, AddingAndRemovingAPieceInOneTickStillInvalidatesTheGraph)
{
    std::vector<UUID> column = MakeColumn(0.0f, 3, "C");
    UUID doomed = MakeBlock(GetScene(), "Doomed", { 12.0f, 0.5f, 0.0f }, /*anchor=*/true);

    RunFrames(1);
    StructuralGraph& graph = GetScene().GetStructuralGraph();
    ASSERT_EQ(graph.NodeIDs.size(), 4u);

    // Destroy one piece and create another in the same tick — the scene's
    // structural-node COUNT is unchanged across the pair.
    GetScene().DestroyEntity(*Resolve(doomed));
    std::vector<UUID> replacement = MakeColumn(20.0f, 1, "R");
    RunFrames(1);

    EXPECT_EQ(graph.Find(doomed), StructuralGraph::kInvalidIndex)
        << "the destroyed piece must not survive in the graph as a load-bearing member";
    EXPECT_NE(graph.Find(replacement[0]), StructuralGraph::kInvalidIndex)
        << "the piece created in the same tick must be picked up";

    // And the graph still works: the original column collapses normally.
    ASSERT_TRUE(Damage(column[0], 500.0f));
    RunFrames(8);
    EXPECT_TRUE(HasLeftStructure(column[1]));
    EXPECT_TRUE(HasLeftStructure(column[2]));
}

// Retuning a piece is an input change, not just a cosmetic one: the graph caches
// m_Anchor / m_ContactMargin / m_MaxLateralSpan at build time, and Lua, the
// editor inspector, MCP and C# can all write them mid-Play without knowing the
// graph exists. The staleness signature has to notice, or the solver keeps
// answering with the values the piece used to have.
TEST_F(StructuralCollapseTest, RetuningAPieceMidPlayInvalidatesTheGraph)
{
    // Anchored base, two blocks stacked on it.
    std::vector<UUID> column = MakeColumn(0.0f, 3, "C");
    RunFrames(1);

    StructuralGraph& graph = GetScene().GetStructuralGraph();
    ASSERT_EQ(graph.NodeIDs.size(), 3u);
    const u64 rebuildsAfterBuild = graph.TotalRebuilds;

    // A tick that changes nothing must not rebuild.
    RunFrames(1);
    EXPECT_EQ(graph.TotalRebuilds, rebuildsAfterBuild) << "an unchanged scene must not rebuild the graph";

    // Un-anchor the base — the exact edit the inspector's Anchor checkbox makes.
    Resolve(column[0])->GetComponent<StructuralNodeComponent>().m_Anchor = false;
    RunFrames(1);
    EXPECT_GT(graph.TotalRebuilds, rebuildsAfterBuild) << "toggling m_Anchor must invalidate the graph";

    // And the solver must act on the NEW value: with no anchor left, breaking
    // the top piece brings the rest down. If the graph had kept the stale
    // anchored base, the column would have stood.
    ASSERT_TRUE(Damage(column[2], 500.0f));
    RunFrames(8);
    EXPECT_TRUE(HasLeftStructure(column[0]))
        << "the un-anchored base must collapse — the solver is still using the stale m_Anchor";
    EXPECT_TRUE(HasLeftStructure(column[1]));
}

// The save-game serializer for StructuralNodeComponent is hand-written and
// unguarded, and it carries the collapse state on purpose: a structure saved
// half-down must reload half-down, with a piece already falling kept out of the
// rebuilt support graph rather than restored as load-bearing.
TEST_F(StructuralCollapseTest, CollapseStateSurvivesASaveGameRoundTrip)
{
    constexpr f32 kEps = 1e-4f;

    Entity e = GetScene().CreateEntity("StructuralSaveGame");
    auto& node = e.AddComponent<StructuralNodeComponent>();
    node.m_Anchor = true;
    node.m_ContactMargin = 0.22f;
    node.m_MaxLateralSpan = 5;
    node.m_CollapseDelay = 0.4f;
    node.m_FallDuration = 2.5f;
    node.m_ShatterOnCollapse = false;
    node.m_State = StructuralState::Detaching;
    node.m_CollapseHops = 3;
    node.m_StateTimer = 1.6f; // == m_CollapseDelay * (hops + 1), what the solver sets

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity re = restored->FindEntityByName("StructuralSaveGame");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<StructuralNodeComponent>())
        << "StructuralNodeComponent dropped by the save-game round-trip";

    const auto& rn = re.GetComponent<StructuralNodeComponent>();
    EXPECT_TRUE(rn.m_Anchor);
    EXPECT_NEAR(rn.m_ContactMargin, 0.22f, kEps);
    EXPECT_EQ(rn.m_MaxLateralSpan, 5u);
    EXPECT_NEAR(rn.m_CollapseDelay, 0.4f, kEps);
    EXPECT_NEAR(rn.m_FallDuration, 2.5f, kEps);
    EXPECT_FALSE(rn.m_ShatterOnCollapse);
    EXPECT_EQ(rn.m_State, StructuralState::Detaching) << "a half-collapsed structure must reload half-collapsed";
    EXPECT_EQ(rn.m_CollapseHops, 3u);
    EXPECT_NEAR(rn.m_StateTimer, 1.6f, kEps)
        << "the restore clamp must bound the timer by what the runtime could have set, not by a flat ceiling";
}

// The graph is derived from the pieces standing right now, and rebuilding it
// mid-collapse must not resurrect what already fell.
TEST_F(StructuralCollapseTest, ARebuildMidCollapseDoesNotResurrectFallenPieces)
{
    std::vector<UUID> column = MakeColumn(0.0f, 5, "C");
    RunFrames(1);

    StructuralGraph& graph = GetScene().GetStructuralGraph();
    ASSERT_EQ(graph.NodeIDs.size(), 5u);

    ASSERT_TRUE(Damage(column[0], 500.0f));
    RunFrames(3);

    // Snapshot who had already left BEFORE the rebuild — those are the pieces a
    // rebuild could wrongly resurrect. (Pieces that leave during the rebuild
    // tick are not evidence either way; the graph was built before they went.)
    std::vector<UUID> departed;
    for (UUID id : column)
    {
        if (HasLeftStructure(id))
            departed.push_back(id);
    }
    ASSERT_FALSE(departed.empty()) << "the collapse must have removed at least one piece by now";

    // Adding a fresh piece changes the scene's structural-node count, which is
    // exactly what forces EnsureBuilt to rebuild.
    UUID latecomer = MakeBlock(GetScene(), "Latecomer", { 20.0f, 0.5f, 0.0f }, /*anchor=*/true);
    const u64 rebuildsBefore = graph.TotalRebuilds;
    RunFrames(1);
    EXPECT_GT(graph.TotalRebuilds, rebuildsBefore) << "adding a piece must invalidate the graph";

    for (UUID id : departed)
    {
        EXPECT_EQ(graph.Find(id), StructuralGraph::kInvalidIndex)
            << "a piece that already left the structure must not be rebuilt into it";
    }
    EXPECT_NE(graph.Find(latecomer), StructuralGraph::kInvalidIndex)
        << "a piece added after the build must be picked up by the rebuild";
}
