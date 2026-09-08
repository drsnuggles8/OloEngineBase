#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Destruction/StructuralGraph.h"

#include "OloEngine/AI/Flocking/FlockSpatialHash.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // World-space axis-aligned bound of the piece's collider.
        //
        // The half-extents are the exact AABB of the transformed box (the sum of
        // |axis_k| * halfExtent_k), so a rotated piece gets a correct bound
        // rather than a hand-waved one. Collider priority: box → sphere →
        // capsule → the entity's own unit cube, which is what an authored block
        // with no collider resolves to. Returns false when the transform or the
        // collider produces anything non-finite; such a piece is left out of the
        // graph rather than poisoning every adjacency test against it.
        [[nodiscard]] bool ComputeWorldBounds(Scene* scene, Entity entity, glm::vec3& outCenter, glm::vec3& outHalfExtents)
        {
            const glm::mat4 world = scene->GetWorldTransform(entity);

            const glm::vec3 axisX{ world[0] };
            const glm::vec3 axisY{ world[1] };
            const glm::vec3 axisZ{ world[2] };
            if (!IsFinite(axisX) || !IsFinite(axisY) || !IsFinite(axisZ) || !IsFinite(glm::vec3(world[3])))
                return false;

            glm::vec3 localHalfExtents{ 0.5f };
            glm::vec3 localOffset{ 0.0f };
            if (entity.HasComponent<BoxCollider3DComponent>())
            {
                const auto& box = entity.GetComponent<BoxCollider3DComponent>();
                localHalfExtents = glm::abs(box.m_HalfExtents);
                localOffset = box.m_Offset;
            }
            else if (entity.HasComponent<SphereCollider3DComponent>())
            {
                const auto& sphere = entity.GetComponent<SphereCollider3DComponent>();
                localHalfExtents = glm::vec3(std::abs(sphere.m_Radius));
                localOffset = sphere.m_Offset;
            }
            else if (entity.HasComponent<CapsuleCollider3DComponent>())
            {
                const auto& capsule = entity.GetComponent<CapsuleCollider3DComponent>();
                const f32 radius = std::abs(capsule.m_Radius);
                localHalfExtents = glm::vec3(radius, std::abs(capsule.m_HalfHeight) + radius, radius);
                localOffset = capsule.m_Offset;
            }
            if (!IsFinite(localHalfExtents) || !IsFinite(localOffset))
                return false;

            outCenter = glm::vec3(world * glm::vec4(localOffset, 1.0f));
            outHalfExtents = glm::abs(axisX) * localHalfExtents.x + glm::abs(axisY) * localHalfExtents.y + glm::abs(axisZ) * localHalfExtents.z;
            return IsFinite(outCenter) && IsFinite(outHalfExtents);
        }

        // One directed edge, before it is packed into CSR form.
        struct Edge
        {
            u32 From;
            u32 To;
            u8 Lateral; // 1 = a same-course step, which the 0-1 flood charges for
        };

        // Build a CSR (offsets, targets [, weights]) from an unsorted edge list.
        // `weights` may be null when the caller does not care about edge kind.
        void BuildCSR(u32 nodeCount, const std::vector<Edge>& edges,
                      std::vector<u32>& offsets, std::vector<u32>& targets, std::vector<u8>* weights)
        {
            offsets.assign(static_cast<sizet>(nodeCount) + 1u, 0u);
            for (const Edge& e : edges)
                ++offsets[static_cast<sizet>(e.From) + 1u];
            for (sizet i = 1; i < offsets.size(); ++i)
                offsets[i] += offsets[i - 1];

            // Sort by (from, to) first so the packed order is a pure function of
            // the node indices — a flood then visits neighbours the same way run
            // to run, whatever order the spatial hash discovered the pairs in.
            std::vector<Edge> sorted(edges);
            std::sort(sorted.begin(), sorted.end(),
                      [](const Edge& a, const Edge& b)
                      { return a.From != b.From ? a.From < b.From : a.To < b.To; });

            targets.assign(sorted.size(), 0u);
            if (weights)
                weights->assign(sorted.size(), 0u);
            for (sizet k = 0; k < sorted.size(); ++k)
            {
                targets[k] = sorted[k].To;
                if (weights)
                    (*weights)[k] = sorted[k].Lateral;
            }
        }
    } // namespace

    void StructuralGraph::Clear()
    {
        NodeIDs.clear();
        Centers.clear();
        HalfExtents.clear();
        Anchor.clear();
        Alive.clear();
        MaxLateralSpan.clear();
        Island.clear();
        SupportOffsets.clear();
        SupportEdges.clear();
        SupportEdgeIsLateral.clear();
        SupportedByOffsets.clear();
        SupportedByEdges.clear();
        IslandNodeOffsets.clear();
        IslandNodes.clear();
        IndexByID.clear();
        IslandCount = 0;
        AliveCount = 0;
        BuiltSceneCount = 0;
        Built = false;
        // m_NoAnchorWarned deliberately survives: it is keyed by a stable piece
        // UUID precisely so a rebuild does not re-report the same structure.
    }

    u32 StructuralGraph::Find(UUID id) const
    {
        auto it = IndexByID.find(static_cast<u64>(id));
        return it == IndexByID.end() ? kInvalidIndex : it->second;
    }

    void StructuralGraph::MarkRemoved(UUID id)
    {
        const u32 index = Find(id);
        if (index == kInvalidIndex || !Alive[index])
            return;
        Alive[index] = 0;
        if (AliveCount > 0)
            --AliveCount;
    }

    void StructuralGraph::EnsureBuilt(Scene* scene)
    {
        if (!scene)
            return;

        const auto view = scene->GetAllEntitiesWith<StructuralNodeComponent>();
        const u32 sceneCount = static_cast<u32>(view.size());
        if (Built && sceneCount == BuiltSceneCount)
            return;

        Rebuild(scene);
    }

    void StructuralGraph::Rebuild(Scene* scene)
    {
        OLO_PROFILE_FUNCTION();

        Clear();
        if (!scene)
            return;

        // ── Nodes ────────────────────────────────────────────────────────────
        u32 sceneCount = 0;
        f32 maxContactMargin = kDefaultContactMargin;
        // Per-node contact slack. Local to the build: the pair test uses the more
        // generous of the two pieces' own margins, so a loosely built structure
        // can be told to be forgiving without making every structure forgiving.
        // `maxContactMargin` is only the conservative bound for the query radius.
        std::vector<f32> margins;
        bool truncated = false;
        for (auto view = scene->GetAllEntitiesWith<StructuralNodeComponent>(); auto e : view)
        {
            ++sceneCount;
            Entity entity{ e, scene };
            const auto& node = entity.GetComponent<StructuralNodeComponent>();

            // Aliveness is derived from the piece's own state, never from the
            // previous graph — that is what makes a mid-collapse rebuild
            // idempotent instead of resurrecting what already fell.
            if (node.m_State == StructuralState::Falling || node.m_State == StructuralState::Collapsed)
                continue;

            if (NodeIDs.size() >= kMaxNodes)
            {
                truncated = true;
                continue;
            }

            glm::vec3 center{ 0.0f };
            glm::vec3 halfExtents{ 0.0f };
            if (!ComputeWorldBounds(scene, entity, center, halfExtents))
            {
                OLO_CORE_WARN("StructuralGraph: entity {} has non-finite bounds and is excluded from the support graph",
                              static_cast<u64>(entity.GetUUID()));
                continue;
            }

            const f32 margin = (std::isfinite(node.m_ContactMargin) && node.m_ContactMargin >= 0.0f) ? node.m_ContactMargin : kDefaultContactMargin;
            margins.push_back(margin);
            maxContactMargin = std::max(maxContactMargin, margin);

            IndexByID.emplace(static_cast<u64>(entity.GetUUID()), static_cast<u32>(NodeIDs.size()));
            NodeIDs.push_back(entity.GetUUID());
            Centers.push_back(center);
            HalfExtents.push_back(halfExtents);
            Anchor.push_back(node.m_Anchor ? u8{ 1 } : u8{ 0 });
            Alive.push_back(1u);
            MaxLateralSpan.push_back(node.m_MaxLateralSpan);
        }

        if (truncated)
        {
            OLO_CORE_ERROR("StructuralGraph: the scene holds more than {} standing structural pieces; the excess is "
                           "outside the support graph and will never collapse",
                           kMaxNodes);
        }

        const u32 nodeCount = static_cast<u32>(NodeIDs.size());
        AliveCount = nodeCount;
        BuiltSceneCount = sceneCount;
        Built = true;
        ++TotalRebuilds;

        m_ScopeStamp.assign(nodeCount, 0u);
        m_ReachedStamp.assign(nodeCount, 0u);
        m_CostStamp.assign(nodeCount, 0u);
        m_Cost.assign(nodeCount, 0u);
        m_HopStamp.assign(nodeCount, 0u);
        m_Hops.assign(nodeCount, 0u);
        m_Stamp = 0;

        Island.assign(nodeCount, 0u);
        IslandNodeOffsets.assign(1u, 0u);
        if (nodeCount == 0)
        {
            SupportOffsets.assign(1u, 0u);
            SupportedByOffsets.assign(1u, 0u);
            return;
        }

        // ── Adjacency ────────────────────────────────────────────────────────
        // A uniform hash over the piece centres. The cell size is the widest
        // query any piece will make, so every query sweeps at most 3x3x3 cells.
        f32 maxExtentRadius = 0.0f;
        for (const glm::vec3& he : HalfExtents)
            maxExtentRadius = std::max(maxExtentRadius, glm::length(he));

        // The pair test is per-axis, so the widest centre distance that can still
        // overlap is |he_i + he_j + margin*(1,1,1)| — the margin enters on the
        // diagonal, hence sqrt(3). Querying with only one margin would silently
        // drop corner-touching pairs.
        const f32 queryRadius = 2.0f * maxExtentRadius + maxContactMargin * 1.7320509f;
        FlockSpatialHash grid;
        grid.Rebuild(Centers, std::max(queryRadius, FlockSpatialHash::kMinCellSize));

        std::vector<Edge> supportPairs;    // (supporter, supported, lateral?)
        std::vector<Edge> undirectedEdges; // both directions, for islands
        for (u32 i = 0; i < nodeCount; ++i)
        {
            grid.ForEachInRadius(Centers[i], queryRadius,
                                 [&](u32 j, const glm::vec3&, f32)
                                 {
                                     // Visit each unordered pair exactly once.
                                     if (j <= i)
                                         return;

                                     // Two pieces are connected when they share a
                                     // FACE, not merely a corner. `overlap` is
                                     // the un-slackened penetration per axis: all
                                     // three must be within the contact margin
                                     // (they are close enough to touch), and at
                                     // least two must genuinely overlap (the
                                     // third axis is the contact normal).
                                     //
                                     // Without the second condition, two unit
                                     // cubes meeting only along an edge count as
                                     // load-bearing, and a diagonal staircase of
                                     // corner-touching blocks holds up a wall.
                                     const f32 margin = std::max(margins[i], margins[j]);
                                     const glm::vec3 delta = glm::abs(Centers[j] - Centers[i]);
                                     const glm::vec3 overlap = HalfExtents[i] + HalfExtents[j] - delta;
                                     if (overlap.x + margin <= 0.0f || overlap.y + margin <= 0.0f || overlap.z + margin <= 0.0f)
                                         return;
                                     const int faceAxes = (overlap.x > 0.0f ? 1 : 0) + (overlap.y > 0.0f ? 1 : 0) + (overlap.z > 0.0f ? 1 : 0);
                                     if (faceAxes < 2)
                                         return;

                                     undirectedEdges.push_back({ i, j, 0u });
                                     undirectedEdges.push_back({ j, i, 0u });

                                     const f32 dy = Centers[j].y - Centers[i].y;
                                     const f32 tolerance = kLevelToleranceFraction * (HalfExtents[i].y + HalfExtents[j].y);
                                     if (dy > tolerance)
                                     {
                                         supportPairs.push_back({ i, j, 0u }); // j rests on i
                                     }
                                     else if (dy < -tolerance)
                                     {
                                         supportPairs.push_back({ j, i, 0u });
                                     }
                                     else
                                     {
                                         // Same course: a bonded row shares load
                                         // both ways, which is what lets a lintel
                                         // stand on its end supports — but each
                                         // sideways step costs the piece one of
                                         // its m_MaxLateralSpan, so the sharing
                                         // has a reach rather than being free.
                                         supportPairs.push_back({ i, j, 1u });
                                         supportPairs.push_back({ j, i, 1u });
                                     }
                                 });
        }

        BuildCSR(nodeCount, supportPairs, SupportOffsets, SupportEdges, &SupportEdgeIsLateral);

        std::vector<Edge> reversePairs;
        reversePairs.reserve(supportPairs.size());
        for (const Edge& e : supportPairs)
            reversePairs.push_back({ e.To, e.From, e.Lateral });
        BuildCSR(nodeCount, reversePairs, SupportedByOffsets, SupportedByEdges, nullptr);

        // ── Islands (connected components over the undirected edges) ─────────
        std::vector<u32> undirectedOffsets;
        std::vector<u32> undirectedFlat;
        BuildCSR(nodeCount, undirectedEdges, undirectedOffsets, undirectedFlat, nullptr);

        constexpr u32 kNoIsland = ~0u;
        Island.assign(nodeCount, kNoIsland);
        std::vector<u32> stack;
        for (u32 seed = 0; seed < nodeCount; ++seed)
        {
            if (Island[seed] != kNoIsland)
                continue;
            const u32 island = IslandCount++;
            Island[seed] = island;
            stack.push_back(seed);
            while (!stack.empty())
            {
                const u32 current = stack.back();
                stack.pop_back();
                for (u32 k = undirectedOffsets[current]; k < undirectedOffsets[current + 1]; ++k)
                {
                    const u32 next = undirectedFlat[k];
                    if (Island[next] == kNoIsland)
                    {
                        Island[next] = island;
                        stack.push_back(next);
                    }
                }
            }
        }

        // Counting sort of nodes by island — this is what lets a solve scope
        // itself in O(island) rather than scanning every node in the scene.
        IslandNodeOffsets.assign(static_cast<sizet>(IslandCount) + 1u, 0u);
        for (u32 i = 0; i < nodeCount; ++i)
            ++IslandNodeOffsets[static_cast<sizet>(Island[i]) + 1u];
        for (sizet k = 1; k < IslandNodeOffsets.size(); ++k)
            IslandNodeOffsets[k] += IslandNodeOffsets[k - 1];
        IslandNodes.assign(nodeCount, 0u);
        {
            std::vector<u32> cursor(IslandNodeOffsets.begin(), IslandNodeOffsets.end() - 1);
            for (u32 i = 0; i < nodeCount; ++i)
                IslandNodes[cursor[Island[i]]++] = i;
        }
    }

    void StructuralGraph::SolveUnsupported(const std::vector<UUID>& seeds,
                                           std::vector<UUID>& outUnsupported,
                                           std::vector<u32>& outHops)
    {
        OLO_PROFILE_FUNCTION();

        LastSolveVisitedNodes = 0;
        LastSolveIslands = 0;
        if (!Built || NodeIDs.empty() || seeds.empty())
            return;
        ++TotalSolves;
        ++m_Stamp;

        // ── Scope: only the islands that actually lost a piece ───────────────
        // A dead seed still resolves — its island is precisely what has to be
        // re-checked, which is why MarkRemoved keeps the id in IndexByID.
        m_Islands.clear();
        for (UUID id : seeds)
        {
            const u32 index = Find(id);
            if (index == kInvalidIndex)
                continue;
            const u32 island = Island[index];
            if (std::find(m_Islands.begin(), m_Islands.end(), island) == m_Islands.end())
                m_Islands.push_back(island);
        }
        if (m_Islands.empty())
            return;
        LastSolveIslands = static_cast<u32>(m_Islands.size());

        m_Queue.clear();
        u32 anchorsInScope = 0;
        u64 lowestScopedID = ~0ull;
        for (u32 island : m_Islands)
        {
            for (u32 k = IslandNodeOffsets[island]; k < IslandNodeOffsets[island + 1]; ++k)
            {
                const u32 i = IslandNodes[k];
                if (!Alive[i])
                    continue;
                m_ScopeStamp[i] = m_Stamp;
                ++LastSolveVisitedNodes;
                lowestScopedID = std::min(lowestScopedID, static_cast<u64>(NodeIDs[i]));
                if (Anchor[i])
                {
                    ++anchorsInScope;
                    m_CostStamp[i] = m_Stamp;
                    m_Cost[i] = 0u;
                    m_Queue.push_back(i);
                }
            }
        }
        if (LastSolveVisitedNodes == 0)
            return;

        if (anchorsInScope == 0 && m_NoAnchorWarned.insert(lowestScopedID).second)
        {
            // Loud and countable: an unanchored structure is an authoring
            // mistake, and the whole-island collapse below is a consequence of
            // it, not a solver bug. Latched on a stable member UUID so it says
            // this once per structure, not once per rebuild.
            OLO_CORE_WARN("StructuralGraph: a structure of {} pieces (lowest id {}) has no "
                          "StructuralNodeComponent::m_Anchor left; every remaining piece is unsupported and will "
                          "collapse",
                          LastSolveVisitedNodes, lowestScopedID);
        }

        // ── The support flood: 0-1 BFS over sideways cost ────────────────────
        // Vertical edges are free, same-course edges cost one step, and a piece
        // is stable only while its cheapest path from an anchor stays inside its
        // own MaxLateralSpan. A piece over budget is not settled as stable AND
        // its outgoing edges are never relaxed — an overhanging piece carries
        // nothing, so a hole in a wall grows sideways instead of stopping.
        //
        // Deque discipline: 0-weight relaxations go to the front and 1-weight to
        // the back, so nodes are popped in nondecreasing cost and the first
        // settle of a node is its minimum.
        while (!m_Queue.empty())
        {
            const u32 current = m_Queue.front();
            m_Queue.pop_front();
            if (m_ReachedStamp[current] == m_Stamp)
                continue; // a stale duplicate; already settled at its minimum
            const u32 cost = m_Cost[current];
            if (cost > MaxLateralSpan[current])
                continue; // cantilevered past its budget: unstable, and load-bearing for nothing
            m_ReachedStamp[current] = m_Stamp;

            for (u32 k = SupportOffsets[current]; k < SupportOffsets[current + 1]; ++k)
            {
                const u32 next = SupportEdges[k];
                if (m_ScopeStamp[next] != m_Stamp || m_ReachedStamp[next] == m_Stamp)
                    continue;
                const u32 nextCost = cost + (SupportEdgeIsLateral[k] ? 1u : 0u);
                if (m_CostStamp[next] == m_Stamp && m_Cost[next] <= nextCost)
                    continue;
                m_CostStamp[next] = m_Stamp;
                m_Cost[next] = nextCost;
                if (SupportEdgeIsLateral[k])
                    m_Queue.push_back(next);
                else
                    m_Queue.push_front(next);
            }
        }

        // ── Hop distance from the break, over the unsupported set ────────────
        // Seeded from the dead pieces themselves, so hop 1 is the ring that was
        // touching the break. A clump the ripple cannot reach (it lost its
        // support somewhere else) stays at 0 and therefore drops first.
        m_Frontier.clear();
        for (UUID id : seeds)
        {
            const u32 index = Find(id);
            if (index != kInvalidIndex && m_HopStamp[index] != m_Stamp)
            {
                m_HopStamp[index] = m_Stamp;
                m_Hops[index] = 0u;
                m_Frontier.push_back(index);
            }
        }
        u32 distance = 0;
        while (!m_Frontier.empty())
        {
            ++distance;
            m_Next.clear();
            for (u32 current : m_Frontier)
            {
                const auto visitNeighbour = [&](u32 next)
                {
                    // Only ripple through pieces that are actually coming down.
                    if (m_HopStamp[next] == m_Stamp || m_ScopeStamp[next] != m_Stamp || m_ReachedStamp[next] == m_Stamp)
                        return;
                    m_HopStamp[next] = m_Stamp;
                    m_Hops[next] = distance;
                    m_Next.push_back(next);
                };
                for (u32 k = SupportOffsets[current]; k < SupportOffsets[current + 1]; ++k)
                    visitNeighbour(SupportEdges[k]);
                for (u32 k = SupportedByOffsets[current]; k < SupportedByOffsets[current + 1]; ++k)
                    visitNeighbour(SupportedByEdges[k]);
            }
            m_Frontier.swap(m_Next);
        }

        // ── Report ───────────────────────────────────────────────────────────
        for (u32 island : m_Islands)
        {
            for (u32 k = IslandNodeOffsets[island]; k < IslandNodeOffsets[island + 1]; ++k)
            {
                const u32 i = IslandNodes[k];
                if (m_ScopeStamp[i] != m_Stamp || m_ReachedStamp[i] == m_Stamp)
                    continue;
                outUnsupported.push_back(NodeIDs[i]);
                outHops.push_back(m_HopStamp[i] == m_Stamp ? m_Hops[i] : 0u);
            }
        }
    }
} // namespace OloEngine
