#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    class Scene;

    // =========================================================================
    // StructuralGraph — the support graph behind progressive collapse (#786).
    //
    // ── What it models ───────────────────────────────────────────────────────
    // One node per standing StructuralNodeComponent entity. A directed edge
    // u → v means "u supports v". A node is STABLE iff an anchor node reaches it
    // along support edges WITHOUT spending more sideways steps than the node's
    // own budget allows; anything else has nothing holding it up and comes down.
    // That is the whole stability model — a weighted support flood, not an FEM,
    // as issue #786 asks for explicitly.
    //
    // Edge direction is decided by height, because that is what "supports" means
    // under gravity. For an adjacent pair (i, j) with a vertical centre offset
    // dy = centre_j.y - centre_i.y and a level tolerance derived from their own
    // vertical extents:
    //   dy >  tol → i supports j        (j rests on i)
    //   dy < -tol → j supports i
    //   |dy| ≤ tol → both               (same course: a bonded, load-sharing row,
    //                                    so a piece can be held by its neighbour
    //                                    and cantilever — the behaviour that lets
    //                                    a lintel span a gap instead of falling
    //                                    the moment nothing is directly under it)
    //
    // ── Why the flood is weighted ────────────────────────────────────────────
    // Plain reachability is not enough. With unlimited sideways load transfer a
    // wall that still has ONE base block standing is fully supported, so it is
    // either intact or entirely gone and never crumbles in between — the exact
    // thing #786 exists to fix. So the flood is a 0-1 BFS: a vertical edge costs
    // 0 and a same-course edge costs 1, and a piece is stable only if its
    // cheapest path from an anchor stays within its own m_MaxLateralSpan. A
    // piece past its span is unstable AND carries nothing onward, which is what
    // makes a hole in a wall grow rather than sit there.
    //
    // ── Derived, not authored ────────────────────────────────────────────────
    // Adjacency comes from the pieces' collider bounds at build time. The
    // rejected alternative (an authored neighbour list on the component) and the
    // reasons are recorded in docs/adr/0021.
    //
    // ── What is island-scoped, and what is not ───────────────────────────────
    // The SOLVE is island-scoped, and that is the claim worth making: a break in
    // one structure re-floods only the connected components that lost a piece,
    // touching none of the rest of the scene. It is O(island), not O(scene) —
    // scratch is stamped rather than cleared, and the in-scope node set comes
    // from a per-island node list built with the graph. LastSolveVisitedNodes
    // reports exactly how many nodes it looked at, and StructuralCollapseTest
    // pins it against a second, untouched structure.
    //
    // The BUILD is not, and does not pretend to be: it is a full O(N log N) pass
    // and it re-runs whenever the scene's structural-node count changes, which
    // during a collapse means the ticks on which pieces are destroyed. It is
    // idempotent — aliveness is derived from each piece's own m_State, never
    // from the previous graph — so a rebuild mid-collapse reconstructs the same
    // standing structure rather than resurrecting what already fell.
    //
    // Runtime-only: held by unique_ptr on the Scene (the FlockingWorkspace
    // pattern), never serialized, never copied by Scene::Copy — it is fully
    // rederived from the components.
    // =========================================================================
    struct StructuralGraph
    {
        // A pair whose expanded bounds overlap is adjacent. Kept small: this is
        // slack for authoring imprecision, not a reach.
        static constexpr f32 kDefaultContactMargin = 0.05f;

        // Two pieces count as "the same course" when their centres are within
        // this fraction of their combined vertical half-extents. 0.25 keeps a
        // running-bond row mutually supporting while a piece stacked on top
        // (dy ≈ a full piece height) is unambiguously one-way.
        static constexpr f32 kLevelToleranceFraction = 0.25f;

        // A piece whose extent radius exceeds this multiple of the scene's median
        // is paired by a linear scan instead of through the spatial hash. Keeping
        // it in the grid would widen EVERY other piece's query to reach it, which
        // is the O(N^2) cliff median-sized cells exist to avoid.
        static constexpr f32 kOversizeFactor = 4.0f;

        // Bound on the build: past this many pieces the adjacency pass refuses
        // the rest rather than silently costing a frame. Structures here are
        // tens of pieces; this exists so a pathological scene fails loudly.
        static constexpr u32 kMaxNodes = 16384;

        static constexpr u32 kInvalidIndex = ~0u;

        // ── Built state, index-parallel over nodes ───────────────────────────
        std::vector<UUID> NodeIDs;
        std::vector<glm::vec3> Centers;     // world-space bound centre
        std::vector<glm::vec3> HalfExtents; // world-space axis-aligned half-extents
        std::vector<u8> Anchor;
        std::vector<u8> Alive;           // 0 once the piece has left the structure
        std::vector<u32> MaxLateralSpan; // per-piece cantilever budget, in same-course steps
        std::vector<u32> Island;

        // CSR adjacency. Supports[i] = the nodes i holds up; SupportedBy[i] = the
        // nodes holding i up. Their union is the undirected neighbourhood, which
        // is what the collapse ripple travels along.
        std::vector<u32> SupportOffsets;
        std::vector<u32> SupportEdges;
        // Edge weight, index-parallel with SupportEdges: 1 for a same-course
        // (sideways) edge, 0 for a vertical one. This is what the 0-1 BFS costs.
        std::vector<u8> SupportEdgeIsLateral;
        std::vector<u32> SupportedByOffsets;
        std::vector<u32> SupportedByEdges;

        // Nodes grouped by island, so scoping a solve never scans the whole
        // graph. IslandNodes[IslandNodeOffsets[k] .. IslandNodeOffsets[k+1]).
        std::vector<u32> IslandNodeOffsets;
        std::vector<u32> IslandNodes;

        std::unordered_map<u64, u32> IndexByID;

        u32 IslandCount = 0;
        u32 AliveCount = 0;
        // Order-independent fold over the UUIDs of every structural-node entity in
        // the scene at build time. This, not a count, is EnsureBuilt's staleness
        // test: a tick that destroys one piece and creates another leaves the count
        // unchanged while the graph is thoroughly wrong — the dead piece still
        // holding up its neighbours, the new one absent from IndexByID so a break
        // on it seeds no island at all.
        u64 BuiltSignature = 0;
        bool Built = false;

        // ── Statistics; the island-scoping contract is asserted against these ──
        u32 LastSolveVisitedNodes = 0;
        u32 LastSolveIslands = 0;
        u64 TotalSolves = 0;
        u64 TotalRebuilds = 0;

        void Clear();

        // Rebuild from every standing StructuralNodeComponent entity.
        void Rebuild(Scene* scene);

        // Rebuild iff the scene's set of structural-node entities has changed
        // since the build. O(N) over a view this tick already walks, and unlike a
        // count it cannot be fooled by a simultaneous create and destroy. A piece
        // merely DETACHING does not change it — that is tracked in place.
        void EnsureBuilt(Scene* scene);

        // Index of `id`, or kInvalidIndex. Dead nodes still resolve — their
        // island is what scopes the re-solve their death triggered.
        [[nodiscard]] u32 Find(UUID id) const;

        // Take a piece out of the structure. Idempotent.
        void MarkRemoved(UUID id);

        // Flood support from the anchors of every island touched by `seeds`, and
        // report the alive nodes those islands could not reach.
        //
        // `outUnsupported` / `outHops` are index-parallel and appended to (not
        // cleared). `outHops` is the undirected hop distance from the nearest
        // seed, measured over the unsupported set — the stagger that makes a
        // collapse ripple outwards from the break instead of dropping flat.
        void SolveUnsupported(const std::vector<UUID>& seeds,
                              std::vector<UUID>& outUnsupported,
                              std::vector<u32>& outHops);

      private:
        // Per-solve scratch, all indexed by node and all STAMPED rather than
        // cleared: a solve bumps m_Stamp and treats "stamp != m_Stamp" as unset,
        // so touching k nodes costs O(k) and never O(nodes). Sized once per
        // build. u64 so the stamp cannot wrap inside a session.
        u64 m_Stamp = 0;
        std::vector<u64> m_ScopeStamp;
        std::vector<u64> m_ReachedStamp; // settled AND stable
        std::vector<u64> m_CostStamp;
        std::vector<u32> m_Cost; // cheapest same-course steps from an anchor
        std::vector<u64> m_HopStamp;
        std::vector<u32> m_Hops;
        std::deque<u32> m_Queue;
        std::vector<u32> m_Frontier;
        std::vector<u32> m_Next;
        std::vector<u32> m_Islands;

        // "This island has no anchor" warning latch, keyed by the island's
        // lowest member UUID so it survives a rebuild and reports once per
        // structure rather than once per rebuild.
        std::unordered_set<u64> m_NoAnchorWarned;
    };
} // namespace OloEngine
