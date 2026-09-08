#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Destruction/StructuralGraph.h"

#include "OloEngine/AI/Flocking/FlockSpatialHash.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <bit>
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

        // Order-independent fold over an entity-id set. splitmix64 each id before
        // summing, so a swap of two members cannot cancel out the way a bare XOR
        // or a bare sum can, and wrapping addition keeps it independent of the
        // view's iteration order.
        [[nodiscard]] u64 MixID(u64 id)
        {
            u64 x = id + 0x9E3779B97F4A7C15ull;
            x ^= x >> 30;
            x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 27;
            x *= 0x94D049BB133111EBull;
            x ^= x >> 31;
            return x;
        }

        // The graph's INPUT identity for one piece: its id plus every authored
        // field Rebuild caches. Folding only the id would leave the graph a stale
        // answer the moment anyone retunes a piece, and m_Anchor / m_ContactMargin
        // / m_MaxLateralSpan are all reachable at runtime from Lua, the editor
        // inspector, MCP and C# — none of which know this graph exists. Making the
        // signature notice is the only version of that which cannot be forgotten
        // at a new mutation site.
        //
        // m_State is deliberately NOT here: a piece leaving the structure is
        // tracked in place by MarkRemoved, and folding it would force a full
        // rebuild on every tick of a collapse.
        [[nodiscard]] u64 StructuralInputSignature(UUID id, const StructuralNodeComponent& node)
        {
            u64 h = MixID(static_cast<u64>(id));
            h = MixID(h ^ (node.m_Anchor ? 0x9E3779B97F4A7C15ull : 0ull));
            // Hash the BITS, not the value — no float comparison, and a NaN margin
            // still produces a stable signature (Rebuild sanitizes the value).
            h = MixID(h ^ static_cast<u64>(std::bit_cast<u32>(node.m_ContactMargin)));
            h = MixID(h ^ (static_cast<u64>(node.m_MaxLateralSpan) * 0xD1B54A32D192ED03ull));
            return h;
        }

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
        BuiltSignature = 0;
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

        u64 signature = 0;
        for (auto view = scene->GetAllEntitiesWith<StructuralNodeComponent>(); auto e : view)
        {
            Entity entity{ e, scene };
            signature += StructuralInputSignature(entity.GetUUID(), entity.GetComponent<StructuralNodeComponent>());
        }

        if (Built && signature == BuiltSignature)
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
        u64 signature = 0;
        f32 maxContactMargin = kDefaultContactMargin;
        // Per-node contact slack. Local to the build: the pair test uses the more
        // generous of the two pieces' own margins, so a loosely built structure
        // can be told to be forgiving without making every structure forgiving.
        // `maxContactMargin` is only the conservative bound for the query radius.
        std::vector<f32> margins;
        bool truncated = false;
        for (auto view = scene->GetAllEntitiesWith<StructuralNodeComponent>(); auto e : view)
        {
            Entity entity{ e, scene };
            const auto& node = entity.GetComponent<StructuralNodeComponent>();
            // Must fold exactly what EnsureBuilt folds, or the two disagree and
            // the graph either never rebuilds or rebuilds every tick.
            signature += StructuralInputSignature(entity.GetUUID(), node);

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
        BuiltSignature = signature;
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
        // A uniform hash over the piece centres.
        std::vector<f32> extentRadii;
        extentRadii.reserve(nodeCount);
        for (const glm::vec3& he : HalfExtents)
            extentRadii.push_back(glm::length(he));

        // The pair test is per-axis, so the widest centre distance that can still
        // overlap is |he_i + he_j + margin*(1,1,1)| — the margin enters on the
        // diagonal, hence sqrt(3). Querying with only one margin would silently
        // drop corner-touching pairs.
        const f32 marginReach = maxContactMargin * 1.7320509f;

        // Size the cells from the TYPICAL piece, not the largest, and take
        // OVERSIZED pieces out of the grid pass entirely.
        //
        // Both halves are needed. A query has to reach the largest partner it
        // could possibly overlap, so as long as one giant piece — a ground slab
        // modelled as a structural node, say — is in the grid, EVERY query is
        // widened to reach it and the pass degenerates towards O(N^2) on every
        // rebuild, i.e. most ticks of a collapse. Median-sized cells alone do not
        // fix that; they only change the constant.
        //
        // So: normal pieces pair with normal pieces through the grid, bounded by
        // the largest NORMAL radius, and anything involving an oversized piece is
        // paired by a linear scan below. Oversized pieces are rare by
        // construction, so paying O(N) for each of them is far cheaper than
        // making every other piece pay for their existence.
        std::vector<f32> sortedRadii(extentRadii);
        std::nth_element(sortedRadii.begin(), sortedRadii.begin() + sortedRadii.size() / 2, sortedRadii.end());
        const f32 medianExtentRadius = sortedRadii[sortedRadii.size() / 2];

        const f32 oversizeThreshold = kOversizeFactor * std::max(medianExtentRadius, 1.0e-4f);
        std::vector<u8> isOversized(nodeCount, 0u);
        std::vector<u32> oversized;
        f32 maxNormalRadius = 0.0f;
        for (u32 i = 0; i < nodeCount; ++i)
        {
            if (extentRadii[i] > oversizeThreshold)
            {
                isOversized[i] = 1u;
                oversized.push_back(i);
            }
            else
            {
                maxNormalRadius = std::max(maxNormalRadius, extentRadii[i]);
            }
        }

        const f32 cellSize = std::max(2.0f * medianExtentRadius + marginReach, FlockSpatialHash::kMinCellSize);
        FlockSpatialHash grid;
        grid.Rebuild(Centers, cellSize);

        std::vector<Edge> supportPairs;    // (supporter, supported, lateral?)
        std::vector<Edge> undirectedEdges; // both directions, for islands
        // The pair test, in one place: both the grid pass and the oversized scan
        // below feed it, so there is exactly one copy of the adjacency rule.
        // Callers guarantee i < j, which is what makes each unordered pair land
        // here once.
        const auto considerPair = [&](u32 i, u32 j)
        {
            // Two pieces are connected when they share a FACE, not merely a
            // corner. `overlap` is the un-slackened penetration per axis: all
            // three must be within the contact margin (they are close enough to
            // touch), and at least two must genuinely overlap (the third axis is
            // the contact normal).
            //
            // Without the second condition, two unit cubes meeting only along an
            // edge count as load-bearing, and a diagonal staircase of
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
                // Same course: a bonded row shares load both ways, which is what
                // lets a lintel stand on its end supports — but each sideways
                // step costs the piece one of its m_MaxLateralSpan, so the
                // sharing has a reach rather than being free.
                supportPairs.push_back({ i, j, 1u });
                supportPairs.push_back({ j, i, 1u });
            }
        };

        // Normal x normal, through the grid. The bound reaches the largest NORMAL
        // partner, so no query is widened by a piece that is not in this pass.
        for (u32 i = 0; i < nodeCount; ++i)
        {
            if (isOversized[i])
                continue;
            const f32 queryRadius = extentRadii[i] + maxNormalRadius + marginReach;
            grid.ForEachInRadius(Centers[i], queryRadius,
                                 [&](u32 j, const glm::vec3&, f32)
                                 {
                                     if (j <= i || isOversized[j])
                                         return; // each pair once; oversized handled below
                                     considerPair(i, j);
                                 });
        }

        // Anything involving an oversized piece, linearly. An oversized/oversized
        // pair is visited from the lower index only, so it still lands once.
        for (u32 o : oversized)
        {
            for (u32 j = 0; j < nodeCount; ++j)
            {
                if (j == o || (isOversized[j] && j < o))
                    continue;
                considerPair(std::min(o, j), std::max(o, j));
            }
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

        // Scope, and check for anchors PER ISLAND. Summing anchors across every
        // scoped island would let one anchored structure vouch for an unanchored
        // one solved alongside it, so the unanchored one would collapse in
        // silence — which is precisely the authoring mistake the warning exists
        // to name.
        m_Queue.clear();
        for (u32 island : m_Islands)
        {
            u32 islandAlive = 0;
            u32 islandAnchors = 0;
            // Keyed over EVERY member, dead ones included, so the latch does not
            // drift to a new key each time the collapse takes another piece.
            u64 islandKey = ~0ull;
            for (u32 k = IslandNodeOffsets[island]; k < IslandNodeOffsets[island + 1]; ++k)
            {
                const u32 i = IslandNodes[k];
                islandKey = std::min(islandKey, static_cast<u64>(NodeIDs[i]));
                if (!Alive[i])
                    continue;
                m_ScopeStamp[i] = m_Stamp;
                ++islandAlive;
                ++LastSolveVisitedNodes;
                if (Anchor[i])
                {
                    ++islandAnchors;
                    m_CostStamp[i] = m_Stamp;
                    m_Cost[i] = 0u;
                    m_Queue.push_back(i);
                }
            }

            if (islandAlive > 0 && islandAnchors == 0 && m_NoAnchorWarned.insert(islandKey).second)
            {
                // Loud and countable: an unanchored structure is an authoring
                // mistake, and the whole-island collapse below is a consequence
                // of it, not a solver bug.
                OLO_CORE_WARN("StructuralGraph: a structure of {} pieces (lowest id {}) has no "
                              "StructuralNodeComponent::m_Anchor left; every remaining piece is unsupported and "
                              "will collapse",
                              islandAlive, islandKey);
            }
        }
        if (LastSolveVisitedNodes == 0)
            return;

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
