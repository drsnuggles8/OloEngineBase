// =============================================================================
// FoliageInstanceIdentityPropertyTests.cpp
//
// Pins the canonical foliage identity contract (issue #1230).
//
// Foliage instances used to BE their row index in a per-layer instance VBO that
// GenerateInstances rebuilt wholesale, so every regeneration renumbered every
// plant. FoliageInstanceRegistry gives each plant a stable id keyed on its
// placement slot instead. The failure mode of getting that wrong is silent —
// a stale or reused id looks identical in a screenshot and only surfaces later
// as wrong culling or wrong residency — so the contract is pinned on the CPU
// here rather than left to visual evidence.
//
// What each test pins, against the FOUR inputs the issue requires deterministic
// invalidation for:
//
//   regeneration        -> IdenticalRegenerationKeepsEveryId
//   terrain (sculpt)    -> SculptKeepsIdentityAndMovesPosition
//                          SteepeningRetiresOnlyTheCellsThatStopQualifying
//   layer editing       -> AttributeOnlyEditKeepsEveryId
//                          DensityChangeRetiresEveryIdInThatLayer
//                          DisablingALayerRetiresOnlyItsInstances
//   removal             -> RemovingALayerRetiresOnlyItsInstances
//                          ClearRetiresEverythingAndNeverReusesAnId
//
// plus the two structural properties the rest hangs off: ids are independent of
// the instance-buffer row order, and a terrain TRANSFORM change updates group
// world bounds without invalidating anything.
//
// Headless by construction: FoliagePlacement::GenerateLayer takes the raw
// height field, so the REAL generator runs with no GL context. The only thing
// this file substitutes for FoliageRenderer::GenerateInstances is the ~12-line
// driver loop below, which creates the GPU buffers production needs and a test
// cannot; keep Regenerate() the same shape as that loop.
//
// OLO_TEST_LAYER: L1
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Terrain/Foliage/FoliageInstanceRegistry.h"
#include "OloEngine/Terrain/Foliage/FoliagePlacement.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <set>
#include <vector>

using namespace OloEngine;

namespace
{
    constexpr u32 kResolution = 64;
    constexpr f32 kWorldSizeX = 64.0f;
    constexpr f32 kWorldSizeZ = 64.0f;
    constexpr f32 kHeightScale = 20.0f;

    /// A perfectly flat field at `height` (normalized [0, 1]). Every cell's
    /// normal is straight up, so the default 0-45 degree slope gate accepts all
    /// of them.
    [[nodiscard]] std::vector<f32> FlatField(f32 height)
    {
        return std::vector<f32>(static_cast<sizet>(kResolution) * kResolution, height);
    }

    /// Flat on the left, broken ground on the right whose normals tilt past the
    /// 45-degree slope gate, so those cells stop emitting.
    ///
    /// A per-texel zigzag rather than a long ramp: one texel spans
    /// kWorldSizeX / (kResolution - 1) ~= 1 world unit, so clearing 45 degrees
    /// needs more than 1 world unit of rise per texel — i.e. more than
    /// 1 / kHeightScale = 0.05 of NORMALIZED height. A ramp gentle enough to
    /// stay inside [0, 1] across half the field cannot get there (0.5 over 32
    /// units is ~17 degrees), which is exactly the mistake this comment exists
    /// to stop the next person repeating.
    [[nodiscard]] std::vector<f32> HalfSteepField()
    {
        std::vector<f32> heights(static_cast<sizet>(kResolution) * kResolution, 0.5f);
        for (u32 z = 0; z < kResolution; ++z)
        {
            for (u32 x = kResolution / 2; x < kResolution; ++x)
            {
                heights[static_cast<sizet>(z) * kResolution + x] = (x % 2 == 0) ? 0.2f : 0.8f;
            }
        }
        return heights;
    }

    [[nodiscard]] FoliageLayer MakeLayer(const char* name, f32 density = 0.25f)
    {
        FoliageLayer layer;
        layer.Name = name;
        layer.Density = density;
        layer.AlbedoPath = std::string("assets/textures/") + name + ".png";
        return layer;
    }

    /// The driver loop from FoliageRenderer::GenerateInstances, minus the GPU
    /// upload. Keep in step with it.
    void Regenerate(FoliageInstanceRegistry& registry,
                    const std::vector<FoliageLayer>& layers,
                    const std::vector<f32>& heights,
                    FoliageRepresentation representation = FoliageRepresentation::MeshCard,
                    bool impostorUnavailable = false,
                    bool reverseBufferOrder = false)
    {
        registry.BeginGeneration();

        std::vector<FoliagePlacement::Placement> placements;
        for (u32 layerIdx = 0; layerIdx < static_cast<u32>(layers.size()); ++layerIdx)
        {
            const auto& layer = layers[layerIdx];
            if (!layer.Enabled || layer.Density <= 0.0f)
            {
                continue;
            }

            FoliagePlacement::GenerateLayer(layer, layerIdx, heights, kResolution, nullptr,
                                            kWorldSizeX, kWorldSizeZ, kHeightScale, placements);

            registry.BeginLayer(layerIdx, layer,
                                FoliagePlacement::SeedForLayer(layerIdx),
                                FoliagePlacement::SpacingForDensity(layer.Density),
                                kWorldSizeX, kWorldSizeZ,
                                representation, impostorUnavailable);

            if (reverseBufferOrder)
            {
                std::ranges::reverse(placements);
            }
            for (u32 row = 0; row < static_cast<u32>(placements.size()); ++row)
            {
                registry.AddInstance(placements[row].m_CellX, placements[row].m_CellZ,
                                     placements[row].m_Row, row);
            }
            registry.EndLayer();
        }

        registry.EndGeneration();
    }

    [[nodiscard]] std::set<FoliageInstanceId> LiveIds(const FoliageInstanceRegistry& registry)
    {
        std::set<FoliageInstanceId> ids;
        for (const auto& record : registry.GetRecords())
        {
            ids.insert(record.m_Id);
        }
        return ids;
    }
} // namespace

namespace OloEngine::Tests
{
    // ── Regeneration ─────────────────────────────────────────────────────

    TEST(FoliageInstanceIdentity, IdenticalRegenerationKeepsEveryId)
    {
        FoliageInstanceRegistry registry;
        const auto heights = FlatField(0.5f);
        const std::vector<FoliageLayer> layers{ MakeLayer("Grass") };

        Regenerate(registry, layers, heights);
        ASSERT_GT(registry.GetRecords().size(), 100u) << "fixture must place enough plants to be meaningful";
        const auto before = LiveIds(registry);

        Regenerate(registry, layers, heights);

        EXPECT_EQ(LiveIds(registry), before);
        EXPECT_TRUE(registry.GetLastDelta().m_Added.empty());
        EXPECT_TRUE(registry.GetLastDelta().m_Retired.empty());
        EXPECT_EQ(registry.GetLastDelta().m_Survived, before.size());
    }

    // ── Terrain ──────────────────────────────────────────────────────────

    TEST(FoliageInstanceIdentity, SculptKeepsIdentityAndMovesPosition)
    {
        FoliageInstanceRegistry registry;
        const std::vector<FoliageLayer> layers{ MakeLayer("Grass") };

        Regenerate(registry, layers, FlatField(0.5f));
        const auto before = LiveIds(registry);
        const FoliageInstanceId sample = *before.begin();
        const f32 yBefore = registry.Find(sample)->m_Position.y;

        // A uniform lift: every cell still qualifies (the field stays flat), so
        // the same plants exist at a new height.
        Regenerate(registry, layers, FlatField(0.75f));

        EXPECT_EQ(LiveIds(registry), before) << "a sculpt that keeps a placement must keep its id";
        EXPECT_TRUE(registry.GetLastDelta().m_Retired.empty());

        const auto* after = registry.Find(sample);
        ASSERT_NE(after, nullptr);
        EXPECT_FLOAT_EQ(after->m_Position.y, 0.75f * kHeightScale);
        EXPECT_GT(after->m_Position.y, yBefore);
        // The record's bounds followed the plant rather than going stale.
        EXPECT_FLOAT_EQ(after->m_LocalBounds.Min.y, after->m_Position.y);
    }

    TEST(FoliageInstanceIdentity, SteepeningRetiresOnlyTheCellsThatStopQualifying)
    {
        FoliageInstanceRegistry registry;
        const std::vector<FoliageLayer> layers{ MakeLayer("Grass") };

        Regenerate(registry, layers, FlatField(0.5f));
        const auto before = LiveIds(registry);

        Regenerate(registry, layers, HalfSteepField());
        const auto after = LiveIds(registry);

        ASSERT_LT(after.size(), before.size()) << "the ramp must reject some cells";
        ASSERT_FALSE(after.empty()) << "the flat half must keep placing";

        // Survivors are a strict subset of the old set: nothing new was issued,
        // because no cell that was rejected before started qualifying.
        for (const auto id : after)
        {
            EXPECT_TRUE(before.contains(id)) << "id " << id << " should have survived, not been re-issued";
        }
        for (const auto id : registry.GetLastDelta().m_Retired)
        {
            EXPECT_TRUE(registry.IsRetired(id));
            EXPECT_FALSE(registry.IsLive(id));
        }
        EXPECT_EQ(before.size() - after.size(), registry.GetLastDelta().m_Retired.size());
    }

    TEST(FoliageInstanceIdentity, TerrainTransformUpdatesWorldBoundsWithoutInvalidating)
    {
        FoliageInstanceRegistry registry;
        const std::vector<FoliageLayer> layers{ MakeLayer("Grass") };
        Regenerate(registry, layers, FlatField(0.5f));

        const auto before = LiveIds(registry);
        const u64 generationBefore = registry.GetGeneration();
        ASSERT_FALSE(registry.GetGroups().empty());
        const BoundingBox worldBefore = registry.GetGroups()[0].m_WorldBounds;
        const BoundingBox localBefore = registry.GetGroups()[0].m_LocalBounds;

        const glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(1000.0f, 5.0f, -250.0f));
        registry.SetTerrainTransform(moved);

        EXPECT_EQ(LiveIds(registry), before) << "a terrain that moves takes its plants with it";
        EXPECT_EQ(registry.GetGeneration(), generationBefore) << "moving the terrain is not a reconcile";

        const auto& group = registry.GetGroups()[0];
        // Terrain-local truth is untouched; only the cached world bounds moved.
        EXPECT_FLOAT_EQ(group.m_LocalBounds.Min.x, localBefore.Min.x);
        EXPECT_FLOAT_EQ(group.m_WorldBounds.Min.x, worldBefore.Min.x + 1000.0f);
        EXPECT_FLOAT_EQ(group.m_WorldBounds.Min.z, worldBefore.Min.z - 250.0f);
    }

    // ── Layer editing ────────────────────────────────────────────────────

    TEST(FoliageInstanceIdentity, AttributeOnlyEditKeepsEveryId)
    {
        FoliageInstanceRegistry registry;
        const auto heights = FlatField(0.5f);
        std::vector<FoliageLayer> layers{ MakeLayer("Grass") };

        Regenerate(registry, layers, heights);
        const auto before = LiveIds(registry);

        // None of these touch the cell -> XZ mapping.
        layers[0].BaseColor = glm::vec3(0.9f, 0.1f, 0.2f);
        layers[0].WindStrength = 2.5f;
        layers[0].ViewDistance = 250.0f;
        Regenerate(registry, layers, heights);

        EXPECT_EQ(LiveIds(registry), before);
        EXPECT_TRUE(registry.GetLastDelta().m_Retired.empty());

        // The material association followed the edit rather than going stale.
        const auto* record = registry.Find(*before.begin());
        ASSERT_NE(record, nullptr);
        const auto* material = registry.GetMaterial(record->m_MaterialKey);
        ASSERT_NE(material, nullptr);
        EXPECT_FLOAT_EQ(material->m_BaseColor.r, 0.9f);
    }

    TEST(FoliageInstanceIdentity, DensityChangeRetiresEveryIdInThatLayer)
    {
        FoliageInstanceRegistry registry;
        const auto heights = FlatField(0.5f);
        std::vector<FoliageLayer> layers{ MakeLayer("Grass", 0.25f) };

        Regenerate(registry, layers, heights);
        const auto before = LiveIds(registry);

        // Density changes the grid spacing, so cell (ix, iz) is a different
        // place. These are different plants and must not inherit ids.
        layers[0].Density = 1.0f;
        Regenerate(registry, layers, heights);
        const auto after = LiveIds(registry);

        for (const auto id : after)
        {
            EXPECT_FALSE(before.contains(id)) << "id " << id << " was reused for a plant in a new place";
        }
        for (const auto id : before)
        {
            EXPECT_TRUE(registry.IsRetired(id));
        }
        EXPECT_EQ(registry.GetLastDelta().m_Survived, 0u);
        EXPECT_EQ(registry.GetLastDelta().m_Retired.size(), before.size());
    }

    TEST(FoliageInstanceIdentity, DisablingALayerRetiresOnlyItsInstances)
    {
        FoliageInstanceRegistry registry;
        const auto heights = FlatField(0.5f);
        std::vector<FoliageLayer> layers{ MakeLayer("Grass"), MakeLayer("Flowers") };

        Regenerate(registry, layers, heights);
        std::set<FoliageInstanceId> grassBefore;
        std::set<FoliageInstanceId> flowersBefore;
        for (const auto& record : registry.GetRecords())
        {
            (record.m_LayerIndex == 0 ? grassBefore : flowersBefore).insert(record.m_Id);
        }
        ASSERT_FALSE(grassBefore.empty());
        ASSERT_FALSE(flowersBefore.empty());

        layers[1].Enabled = false;
        Regenerate(registry, layers, heights);

        EXPECT_EQ(LiveIds(registry), grassBefore) << "the untouched layer must be unaffected";
        for (const auto id : flowersBefore)
        {
            EXPECT_TRUE(registry.IsRetired(id));
        }
    }

    // ── Removal ──────────────────────────────────────────────────────────

    TEST(FoliageInstanceIdentity, RemovingALayerRetiresOnlyItsInstances)
    {
        FoliageInstanceRegistry registry;
        const auto heights = FlatField(0.5f);
        std::vector<FoliageLayer> layers{ MakeLayer("Grass"), MakeLayer("Flowers") };

        Regenerate(registry, layers, heights);
        std::set<FoliageInstanceId> grassBefore;
        for (const auto& record : registry.GetRecords())
        {
            if (record.m_LayerIndex == 0)
            {
                grassBefore.insert(record.m_Id);
            }
        }
        const auto allBefore = LiveIds(registry);

        layers.pop_back();
        Regenerate(registry, layers, heights);

        EXPECT_EQ(LiveIds(registry), grassBefore);
        EXPECT_EQ(registry.GetLastDelta().m_Retired.size(), allBefore.size() - grassBefore.size());
    }

    TEST(FoliageInstanceIdentity, ClearRetiresEverythingAndNeverReusesAnId)
    {
        FoliageInstanceRegistry registry;
        const auto heights = FlatField(0.5f);
        const std::vector<FoliageLayer> layers{ MakeLayer("Grass") };

        Regenerate(registry, layers, heights);
        const auto before = LiveIds(registry);
        ASSERT_FALSE(before.empty());

        registry.Clear();
        EXPECT_TRUE(registry.GetRecords().empty());
        EXPECT_TRUE(registry.GetGroups().empty());
        for (const auto id : before)
        {
            EXPECT_TRUE(registry.IsRetired(id));
        }

        // Rebuilding the identical scene issues FRESH ids: the registry has no
        // way to know these are "the same" plants across a teardown, and
        // guessing would be the silent reuse the id scheme exists to prevent.
        Regenerate(registry, layers, heights);
        for (const auto id : LiveIds(registry))
        {
            EXPECT_FALSE(before.contains(id)) << "id " << id << " survived a Clear()";
        }
    }

    // ── Identity is not the buffer row ───────────────────────────────────

    TEST(FoliageInstanceIdentity, IdsAreIndependentOfInstanceBufferOrder)
    {
        FoliageInstanceRegistry registry;
        const auto heights = FlatField(0.5f);
        const std::vector<FoliageLayer> layers{ MakeLayer("Grass") };

        Regenerate(registry, layers, heights);
        const auto before = LiveIds(registry);
        const FoliageInstanceId firstRowBefore = registry.GetIdForBufferRow(0, 0);
        ASSERT_NE(firstRowBefore, kInvalidFoliageInstanceId);

        // Same plants, emitted into the buffer back to front.
        Regenerate(registry, layers, heights, FoliageRepresentation::MeshCard, false, /*reverseBufferOrder=*/true);

        EXPECT_EQ(LiveIds(registry), before) << "row order must not touch identity";
        EXPECT_TRUE(registry.GetLastDelta().m_Retired.empty());
        EXPECT_NE(registry.GetIdForBufferRow(0, 0), firstRowBefore)
            << "the fixture must actually have reordered the buffer";

        // The row -> id map still agrees with the records after the shuffle.
        for (const auto& record : registry.GetRecords())
        {
            EXPECT_EQ(registry.GetIdForBufferRow(record.m_LayerIndex, record.m_BufferIndex), record.m_Id);
        }
    }

    TEST(FoliageInstanceIdentity, PlacementKeyRoundTripsToItsId)
    {
        FoliageInstanceRegistry registry;
        Regenerate(registry, { MakeLayer("Grass") }, FlatField(0.5f));

        for (const auto& record : registry.GetRecords())
        {
            EXPECT_EQ(registry.FindByPlacement(record.m_Key), record.m_Id);
        }
        EXPECT_EQ(registry.FindByPlacement(FoliagePlacementKey{}), kInvalidFoliageInstanceId);
    }

    // ── Spatial groups ───────────────────────────────────────────────────

    TEST(FoliageInstanceIdentity, GroupsCoverEveryInstanceAndBoundEachOne)
    {
        FoliageInstanceRegistry registry;
        Regenerate(registry, { MakeLayer("Grass"), MakeLayer("Flowers") }, FlatField(0.5f));

        ASSERT_FALSE(registry.GetGroups().empty());

        sizet grouped = 0;
        for (const auto& group : registry.GetGroups())
        {
            grouped += group.m_Instances.size();
            EXPECT_EQ(group.m_RepresentedCount + group.m_UnsupportedCount, group.m_Instances.size());

            for (const auto id : group.m_Instances)
            {
                const auto* record = registry.Find(id);
                ASSERT_NE(record, nullptr);
                EXPECT_EQ(record->m_LayerIndex, group.m_LayerIndex) << "groups must not span layers";

                // The group's bounds actually contain the plant.
                EXPECT_LE(group.m_LocalBounds.Min.x, record->m_LocalBounds.Min.x);
                EXPECT_LE(group.m_LocalBounds.Min.y, record->m_LocalBounds.Min.y);
                EXPECT_LE(group.m_LocalBounds.Min.z, record->m_LocalBounds.Min.z);
                EXPECT_GE(group.m_LocalBounds.Max.x, record->m_LocalBounds.Max.x);
                EXPECT_GE(group.m_LocalBounds.Max.y, record->m_LocalBounds.Max.y);
                EXPECT_GE(group.m_LocalBounds.Max.z, record->m_LocalBounds.Max.z);
            }
        }
        EXPECT_EQ(grouped, registry.GetRecords().size()) << "every instance belongs to exactly one group";

        // And the record's own back-pointer agrees.
        for (const auto& record : registry.GetRecords())
        {
            ASSERT_LT(record.m_GroupIndex, registry.GetGroups().size());
            const auto& group = registry.GetGroups()[record.m_GroupIndex];
            EXPECT_NE(std::ranges::find(group.m_Instances, record.m_Id), group.m_Instances.end());
        }
    }

    TEST(FoliageInstanceIdentity, GroupQueryFindsTheGroupHoldingAKnownInstance)
    {
        FoliageInstanceRegistry registry;
        Regenerate(registry, { MakeLayer("Grass") }, FlatField(0.5f));
        ASSERT_FALSE(registry.GetRecords().empty());

        const auto& record = registry.GetRecords().front();
        const auto& owning = registry.GetGroups()[record.m_GroupIndex];

        const auto hits = registry.FindGroupsInWorldBounds(owning.m_WorldBounds);
        EXPECT_NE(std::ranges::find(hits, record.m_GroupIndex), hits.end());

        // A box far outside the terrain matches nothing.
        const BoundingBox elsewhere(glm::vec3(10000.0f), glm::vec3(10100.0f));
        EXPECT_TRUE(registry.FindGroupsInWorldBounds(elsewhere).empty());
    }

    // ── Representation census ────────────────────────────────────────────

    TEST(FoliageInstanceIdentity, CensusCountsRepresentationAndNamesUnsupportedVariants)
    {
        FoliageInstanceRegistry registry;
        const auto heights = FlatField(0.5f);
        const std::vector<FoliageLayer> layers{ MakeLayer("Grass") };

        Regenerate(registry, layers, heights, FoliageRepresentation::MeshCard);
        {
            const auto& census = registry.GetCensus();
            EXPECT_EQ(census.m_CanonicalInstances, registry.GetRecords().size());
            EXPECT_EQ(census.m_MeshCardInstances, census.m_CanonicalInstances);
            EXPECT_EQ(census.m_ImpostorInstances, 0u);
            EXPECT_EQ(census.m_UnsupportedInstances, 0u);
            EXPECT_EQ(census.m_UnsupportedVariants, 0u);
            EXPECT_EQ(census.m_SpatialGroups, registry.GetGroups().size());
        }

        Regenerate(registry, layers, heights, FoliageRepresentation::Impostor);
        {
            const auto& census = registry.GetCensus();
            EXPECT_EQ(census.m_ImpostorInstances, census.m_CanonicalInstances);
            EXPECT_EQ(census.m_MeshCardInstances, 0u);
        }

        // A layer that asked for an impostor and did not get one still draws as
        // a card, so its instances stay MeshCard — but the VARIANT is counted,
        // not silently dropped.
        Regenerate(registry, layers, heights, FoliageRepresentation::MeshCard, /*impostorUnavailable=*/true);
        {
            const auto& census = registry.GetCensus();
            EXPECT_EQ(census.m_MeshCardInstances, census.m_CanonicalInstances);
            EXPECT_EQ(census.m_UnsupportedVariants, 1u);
        }

        Regenerate(registry, layers, heights, FoliageRepresentation::Unsupported);
        {
            const auto& census = registry.GetCensus();
            EXPECT_EQ(census.m_UnsupportedInstances, census.m_CanonicalInstances);
            EXPECT_GT(census.m_UnsupportedInstances, 0u);
            for (const auto& group : registry.GetGroups())
            {
                EXPECT_EQ(group.m_RepresentedCount, 0u);
            }
        }
    }

    TEST(FoliageInstanceIdentity, MaterialsAreInternedPerDistinctDescriptor)
    {
        FoliageInstanceRegistry registry;
        const auto heights = FlatField(0.5f);

        // Two layers that differ only by name share nothing about placement but
        // DO have different albedo paths, so two descriptors.
        Regenerate(registry, { MakeLayer("Grass"), MakeLayer("Flowers") }, heights);
        EXPECT_EQ(registry.GetMaterials().size(), 2u);

        std::set<FoliageMaterialKey> keysByLayer[2];
        for (const auto& record : registry.GetRecords())
        {
            keysByLayer[record.m_LayerIndex].insert(record.m_MaterialKey);
        }
        EXPECT_EQ(keysByLayer[0].size(), 1u) << "one layer, one material";
        EXPECT_EQ(keysByLayer[1].size(), 1u);
        EXPECT_NE(*keysByLayer[0].begin(), *keysByLayer[1].begin());
    }
} // namespace OloEngine::Tests
