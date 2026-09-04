// OLO_TEST_LAYER: L1
// =============================================================================
// LightmapAtlasPagingTest.cpp
//
// The multi-page lightmap atlas (issue #868). All CPU-only (no GL, no ECS).
//
// THE MEASUREMENT THE ISSUE ASKED FOR, AND WHAT IT ACTUALLY FOUND. #868 was
// written on the premise that an over-budget scene "degrades region sizes —
// halving them until everything fits, and dropping entities only as a last
// resort", and floated descoping the work if that degradation stayed graceful.
// Measured (`MeasureSinglePageOverloadDropsEntitiesRatherThanDegrading`, at the
// default 1024 atlas, 8 texels/m, uniform 4 m cubes wanting 128px each):
//
//   load          pages  baked  skipped  region sizes
//   1x (64)         1      64      0     all 128px
//   2x (128)        1      64     64     all 128px
//   4x (256)        1      64    192     all 128px
//
// Nothing degrades. The overflow entities are DROPPED — they get no baked GI at
// all. The degrade loop cannot fire here, and the reason is structural rather
// than a bug: regions are packed largest-first into a buddy quadtree, so at the
// moment a request of size S fails, every allocation already placed is >= S and
// aligned, which means there is no free block SMALLER than S to fall back to
// either. The atlas is simply full. The loop stays as a safety net for any
// future ordering that could leave sub-S holes, but on today's ordering it is
// unreachable.
//
// So the descope option the issue offered is closed, and closed harder than it
// expected: the pre-#868 failure mode on a 2x scene is not "coarser GI", it is
// "half the scene has no baked GI".
//
// What the rest of the file pins:
//   1. The measurement above (so the baseline can never silently change and
//      make the paging tests vacuous).
//   2. PAGING removes the loss entirely while the budget lasts: every entity
//      keeps its DESIRED region size and nothing is skipped.
//   3. THE BUDGET IS A CEILING, not "as many pages as needed": once it is
//      exhausted the pre-#868 behaviour resumes exactly.
//   4. Determinism (an explicit acceptance bullet): two Prepare() runs over the
//      same inputs agree on every region AND every page assignment.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Baking/LightmapBaker.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/LightmapAsset.h"
#include "OloEngine/Renderer/LightmapPageEncoding.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // `count` entities sharing ONE unit-cube MeshSource, each uniformly
        // scaled to `edge` metres and spread along X so no two occupy the same
        // space.
        //
        // Sharing the MeshSource is deliberate and is what keeps these tests
        // cheap: LightmapUnwrap::Generate is idempotent, so a shared source
        // unwraps ONCE instead of `count` times (xatlas is the dominant cost
        // here, and the largest case packs 256 entities). Entities sharing a
        // mesh is a real authoring shape, and it changes nothing this file
        // measures — region size comes from world-space AREA, so the per-entity
        // transform still drives the packing.
        [[nodiscard]] std::vector<LightmapBakeInput> MakeCubeField(u32 count, f32 edge)
        {
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            Ref<MeshSource> shared = cube ? cube->GetMeshSource() : nullptr;

            std::vector<LightmapBakeInput> inputs;
            inputs.reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                LightmapBakeInput input;
                input.EntityUUID = 0x1000u + i;
                input.Mesh = shared;
                input.WorldTransform = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i) * edge * 2.0f, 0.0f, 0.0f)) *
                                       glm::scale(glm::mat4(1.0f), glm::vec3(edge));
                inputs.push_back(input);
            }
            return inputs;
        }

        // The shape an InstancedMeshComponent produces (issue #867): ONE entity
        // UUID and `count` distinct sub-keys, each at its own world transform.
        // Before #867 the packing sort tie-broke on UUID alone, which is not a
        // total order over this list at all — std::sort may then place equal
        // elements however the implementation likes, and two runs of the same
        // bake can differ. That failure would have appeared ONLY on scenes with
        // instances, i.e. exactly the scenes #867 exists for.
        [[nodiscard]] std::vector<LightmapBakeInput> MakeInstanceField(u32 count, f32 edge)
        {
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            Ref<MeshSource> shared = cube ? cube->GetMeshSource() : nullptr;

            std::vector<LightmapBakeInput> inputs;
            inputs.reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                LightmapBakeInput input;
                input.EntityUUID = 0x5EED0000u; // one entity for the whole batch
                input.SubKey = static_cast<u64>(i) + 1u;
                input.Mesh = shared;
                input.WorldTransform =
                    glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i) * edge * 2.0f, 0.0f, 0.0f)) *
                    glm::scale(glm::mat4(1.0f), glm::vec3(edge));
                inputs.push_back(input);
            }
            return inputs;
        }

        // The region size Prepare() WOULD pick for one entity if the atlas were
        // unbounded — the same expression LightmapBaker uses, duplicated here on
        // purpose so the test measures against the intent rather than against
        // whatever the allocator happened to hand back.
        [[nodiscard]] u32 DesiredRegionSize(f32 edge, const LightmapBakeSettings& settings)
        {
            const f64 area = 6.0 * static_cast<f64>(edge) * static_cast<f64>(edge);
            const f64 desiredTexels = area * static_cast<f64>(settings.TexelsPerMeter) * static_cast<f64>(settings.TexelsPerMeter);
            u32 size = 1;
            while (static_cast<f64>(size) * size < desiredTexels)
            {
                size *= 2;
            }
            return std::clamp(size, settings.MinRegionSize, settings.AtlasSize);
        }

        struct PackingStats
        {
            std::map<u32, u32> SizeHistogram; // region size -> count
            u32 PageCount = 0;
            u32 Baked = 0;
            u32 Skipped = 0;
            u32 SmallestRegion = 0;
            u32 LargestRegion = 0;
        };

        [[nodiscard]] PackingStats Summarize(const LightmapBakePrepared& prepared)
        {
            PackingStats stats;
            stats.PageCount = prepared.PageCount;
            stats.Baked = prepared.BakedEntityCount;
            stats.Skipped = prepared.SkippedEntityCount;
            for (const LightmapAtlasRegion& region : prepared.Regions)
            {
                ++stats.SizeHistogram[region.Size];
                stats.SmallestRegion = stats.SmallestRegion == 0 ? region.Size : std::min(stats.SmallestRegion, region.Size);
                stats.LargestRegion = std::max(stats.LargestRegion, region.Size);
            }
            return stats;
        }

        void LogStats(const char* label, u32 desiredSize, const PackingStats& stats)
        {
            std::string histogram;
            for (const auto& [size, count] : stats.SizeHistogram)
            {
                histogram += "  " + std::to_string(size) + "px x" + std::to_string(count);
            }
            OLO_CORE_INFO("[#868 measurement] {}: desired {}px/entity, pages {}, baked {}, skipped {}, sizes:{}",
                          label, desiredSize, stats.PageCount, stats.Baked, stats.Skipped, histogram);
        }
    } // namespace

    // ── 1. The measurement (see the file header for the numbers) ───────────
    //
    // The budget is pinned to a single page, which IS the pre-#868 allocator —
    // the degrade-then-drop loop was kept, not replaced — so this doubles as
    // regression coverage for the behaviour paging falls back to.
    TEST(LightmapAtlasPagingTest, MeasureSinglePageOverloadDropsEntitiesRatherThanDegrading)
    {
        LightmapBakeSettings settings;
        settings.AtlasSize = 1024;
        settings.MinRegionSize = 8;
        settings.TexelsPerMeter = 8.0f;

        // A 4 m cube wants 6*16 m^2 * 64 texels/m^2 = 6144 texels -> 128px
        // (power-of-two ceiling). A 1024px page holds 64 of those exactly, so
        // 128 entities is a 2x overload and 256 is 4x.
        constexpr f32 kEdge = 4.0f;
        const u32 desired = DesiredRegionSize(kEdge, settings);
        ASSERT_EQ(desired, 128u) << "the overload factors below are derived from this";

        for (const u32 count : { 64u, 128u, 256u })
        {
            std::vector<LightmapBakeInput> inputs = MakeCubeField(count, kEdge);
            settings.MaxAtlasPages = 1;

            LightmapBakePrepared prepared;
            std::string error;
            ASSERT_TRUE(LightmapBaker::Prepare(inputs, settings, prepared, error)) << error;

            const PackingStats stats = Summarize(prepared);
            LogStats(("single page, " + std::to_string(count) + " entities").c_str(), desired, stats);

            EXPECT_EQ(stats.PageCount, 1u);
            // A 1024px page holds exactly 64 regions of 128px, whatever the
            // load — the packer never places more and never degrades.
            EXPECT_EQ(stats.Baked, 64u) << "at " << count << " entities";
            EXPECT_EQ(stats.Skipped, count - 64u) << "at " << count << " entities";
            EXPECT_EQ(stats.SmallestRegion, desired)
                << "at " << count << " entities: the surviving regions are NOT degraded — "
                                     "over-budget entities are dropped instead (see the file header)";
            EXPECT_EQ(stats.LargestRegion, desired) << "at " << count << " entities";
        }
    }

    // ── 2. Paging removes the degradation ──────────────────────────────────
    TEST(LightmapAtlasPagingTest, PagingKeepsDesiredRegionSizeUnderOverload)
    {
        LightmapBakeSettings settings;
        settings.AtlasSize = 1024;
        settings.MinRegionSize = 8;
        settings.TexelsPerMeter = 8.0f;
        settings.MaxAtlasPages = 4;

        constexpr f32 kEdge = 4.0f;
        const u32 desired = DesiredRegionSize(kEdge, settings);

        // 4x the single-page capacity, and exactly the 4-page budget.
        std::vector<LightmapBakeInput> inputs = MakeCubeField(256, kEdge);

        LightmapBakePrepared prepared;
        std::string error;
        ASSERT_TRUE(LightmapBaker::Prepare(inputs, settings, prepared, error)) << error;

        const PackingStats stats = Summarize(prepared);
        LogStats("4 pages, 256 entities", desired, stats);

        EXPECT_EQ(stats.PageCount, 4u);
        EXPECT_EQ(stats.Baked, 256u);
        EXPECT_EQ(stats.Skipped, 0u);
        EXPECT_EQ(stats.SmallestRegion, desired) << "no entity may degrade while page budget remains";
        EXPECT_EQ(stats.LargestRegion, desired);

        // Every entry must name a page inside the reported count, and the
        // region table must agree with it entity-for-entity.
        ASSERT_EQ(prepared.Entries.size(), prepared.Regions.size());
        for (sizet i = 0; i < prepared.Entries.size(); ++i)
        {
            EXPECT_LT(prepared.Entries[i].Page, prepared.PageCount);
            EXPECT_EQ(prepared.Entries[i].Page, prepared.Regions[i].Page);
        }
    }

    // ── 3. The budget is a ceiling, not "as many pages as needed" ──────────
    //
    // Past the ceiling the pre-#868 behaviour resumes exactly: the packer stops
    // opening pages and the remaining entities are dropped (not degraded — see
    // the file header for why the degrade loop cannot fire here).
    TEST(LightmapAtlasPagingTest, ExhaustedPageBudgetStopsOpeningPages)
    {
        LightmapBakeSettings settings;
        settings.AtlasSize = 1024;
        settings.MinRegionSize = 8;
        settings.TexelsPerMeter = 8.0f;
        settings.MaxAtlasPages = 2;

        constexpr f32 kEdge = 4.0f;
        const u32 desired = DesiredRegionSize(kEdge, settings);

        // 4x the single-page capacity against a 2-page budget.
        std::vector<LightmapBakeInput> inputs = MakeCubeField(256, kEdge);

        LightmapBakePrepared prepared;
        std::string error;
        ASSERT_TRUE(LightmapBaker::Prepare(inputs, settings, prepared, error)) << error;

        const PackingStats stats = Summarize(prepared);
        LogStats("2 pages (budget capped), 256 entities", desired, stats);

        EXPECT_EQ(stats.PageCount, 2u) << "the budget must bound the page count";
        EXPECT_EQ(stats.Baked, 128u) << "two 1024px pages hold exactly 2 x 64 regions of 128px";
        EXPECT_EQ(stats.Skipped, 128u) << "the rest must be dropped, not squeezed onto a third page";
        EXPECT_EQ(stats.SmallestRegion, desired) << "what IS baked keeps full density";
    }

    // ── Determinism: page assignment included ──────────────────────────────
    TEST(LightmapAtlasPagingTest, PageAssignmentIsDeterministic)
    {
        LightmapBakeSettings settings;
        settings.AtlasSize = 512;
        settings.MinRegionSize = 8;
        settings.TexelsPerMeter = 8.0f;
        settings.MaxAtlasPages = 4;

        LightmapBakePrepared first;
        LightmapBakePrepared second;
        std::string error;

        std::vector<LightmapBakeInput> inputsA = MakeCubeField(48, 4.0f);
        ASSERT_TRUE(LightmapBaker::Prepare(inputsA, settings, first, error)) << error;
        std::vector<LightmapBakeInput> inputsB = MakeCubeField(48, 4.0f);
        ASSERT_TRUE(LightmapBaker::Prepare(inputsB, settings, second, error)) << error;

        ASSERT_EQ(first.PageCount, second.PageCount);
        ASSERT_EQ(first.Entries.size(), second.Entries.size());
        ASSERT_EQ(first.Regions.size(), second.Regions.size());
        for (sizet i = 0; i < first.Entries.size(); ++i)
        {
            EXPECT_EQ(first.Entries[i].EntityUUID, second.Entries[i].EntityUUID) << "entry " << i;
            EXPECT_EQ(first.Entries[i].Page, second.Entries[i].Page) << "entry " << i;
            EXPECT_EQ(first.Regions[i].X, second.Regions[i].X) << "region " << i;
            EXPECT_EQ(first.Regions[i].Y, second.Regions[i].Y) << "region " << i;
            EXPECT_EQ(first.Regions[i].Size, second.Regions[i].Size) << "region " << i;
            EXPECT_EQ(first.Regions[i].Page, second.Regions[i].Page) << "region " << i;
        }
    }

    // ── Determinism holds when every input SHARES one entity UUID (issue #867)
    //
    // The instanced receiver's shape. The packing sort's tie-break is
    // (size desc, UUID asc, SubKey asc); drop the sub-key and the comparator
    // ties for every pair in this list, so std::sort is free to order them
    // differently between two runs — and the whole lightmap verification story
    // (bit-identical bakes, oracle re-derivation at a seed derived from the
    // texel's ADDRESS) rests on the layout being reproducible.
    //
    // Two separate assertions, because they fail differently: identical layouts
    // across runs is determinism, and distinct regions per sub-key is the thing
    // that makes N instances worth baking at all.
    TEST(LightmapAtlasPaging, OneEntityWithManySubKeysPacksDeterministically)
    {
        LightmapBakeSettings settings;
        settings.AtlasSize = 512;
        settings.MinRegionSize = 8;
        settings.TexelsPerMeter = 8.0f;
        settings.MaxAtlasPages = 4;

        LightmapBakePrepared first;
        LightmapBakePrepared second;
        std::string error;

        std::vector<LightmapBakeInput> inputsA = MakeInstanceField(48, 4.0f);
        ASSERT_TRUE(LightmapBaker::Prepare(inputsA, settings, first, error)) << error;
        std::vector<LightmapBakeInput> inputsB = MakeInstanceField(48, 4.0f);
        ASSERT_TRUE(LightmapBaker::Prepare(inputsB, settings, second, error)) << error;

        ASSERT_EQ(first.Entries.size(), 48u) << "every instance must get its own region";
        ASSERT_EQ(first.Entries.size(), second.Entries.size());
        for (sizet i = 0; i < first.Entries.size(); ++i)
        {
            EXPECT_EQ(first.Entries[i].EntityUUID, second.Entries[i].EntityUUID) << "entry " << i;
            EXPECT_EQ(first.Entries[i].SubKey, second.Entries[i].SubKey) << "entry " << i;
            EXPECT_EQ(first.Regions[i].X, second.Regions[i].X) << "region " << i;
            EXPECT_EQ(first.Regions[i].Y, second.Regions[i].Y) << "region " << i;
            EXPECT_EQ(first.Regions[i].Page, second.Regions[i].Page) << "region " << i;
        }

        // No two instances may share an atlas rect — that is the failure the
        // sub-key exists to prevent, and it renders as a batch lit from one
        // instance's charts rather than as anything obviously broken.
        std::set<std::tuple<u32, u32, u32>> occupied;
        for (const auto& region : first.Regions)
        {
            EXPECT_TRUE(occupied.emplace(region.Page, region.X, region.Y).second)
                << "two sub-keys landed on the same atlas rect";
        }
    }
    // ── The baked ASSET is page-shaped, and each page holds only its own
    // entities' coverage ─────────────────────────────────────────────────────
    //
    // Prepare() decides the layout; this is the half that could still get the
    // layout right and then write every texel into page 0. The world is
    // deliberately EMPTY — every path escapes, so each covered texel bakes to
    // black with alpha 1 and the test reads coverage, which is exactly the
    // channel that says "the bake wrote here". Cheap enough to run at 1 spp.
    TEST(LightmapAtlasPagingTest, BakedAssetCarriesOnePageOfCoveragePerEntity)
    {
        LightmapBakeSettings settings;
        settings.AtlasSize = 32;
        settings.MinRegionSize = 32; // one region per page
        settings.TexelsPerMeter = 8.0f;
        settings.MaxAtlasPages = 4;
        settings.SamplesPerTexel = 1;
        settings.MaxBounces = 1;
        settings.DilationPasses = 1;

        std::vector<LightmapBakeInput> inputs = MakeCubeField(3, 1.0f);

        LightmapBakePrepared prepared;
        std::string error;
        ASSERT_TRUE(LightmapBaker::Prepare(inputs, settings, prepared, error)) << error;
        ASSERT_EQ(prepared.PageCount, 3u) << "one 32px region per 32px page, three entities";

        PathTracing::ReferenceSceneBuilder builder;
        const PathTracing::ReferenceScene world = builder.Build(PathTracing::ReferenceSceneBuildOptions{});

        const LightmapBakeResult result = LightmapBaker::BakeTexels(prepared, world, settings);
        ASSERT_TRUE(result.Success) << result.Error;
        ASSERT_TRUE(result.Asset);

        EXPECT_EQ(result.Asset->GetPageCount(), 3u);
        EXPECT_EQ(result.Asset->GetTexelData().size(), result.Asset->GetExpectedTexelCount());
        EXPECT_TRUE(result.Asset->Validate()) << "a multi-page asset must validate";

        // Every page must carry coverage — a bake that wrote everything into
        // page 0 leaves pages 1..N-1 entirely alpha-0.
        const auto& texels = result.Asset->GetTexelData();
        const sizet pageFloats = static_cast<sizet>(settings.AtlasSize) * settings.AtlasSize * 4;
        for (u32 page = 0; page < result.Asset->GetPageCount(); ++page)
        {
            u32 covered = 0;
            for (sizet i = 3; i < pageFloats; i += 4)
            {
                if (texels[static_cast<sizet>(page) * pageFloats + i] > 0.0f)
                {
                    ++covered;
                }
            }
            EXPECT_GT(covered, 0u) << "page " << page << " has no baked texels at all";
        }
    }

    // ── The replacement for the removed `Page != 0` rejection ───────────────
    //
    // The old runtime guard refused any entry naming a page above 0 because
    // only page 0 was uploaded. Paging removes the reason, not the hazard: an
    // entry naming a page the asset does not have would sample ANOTHER page's
    // charts through a valid-looking region. That is now caught one level
    // earlier — LightmapAsset::Validate() rejects the asset outright, so
    // SceneLightmapRuntime::Resolve never sees it (and keeps a bounds check of
    // its own as a second line).
    TEST(LightmapAtlasPagingTest, AssetWithOutOfRangePageFailsValidation)
    {
        // Asset is non-copyable (RefCounted), so build it in place and return
        // only the verdict.
        const auto validates = [](u32 pageCount, u32 entryPage)
        {
            LightmapEntityEntry entry;
            entry.EntityUUID = 0x1234u;
            entry.Page = entryPage;
            entry.ScaleOffset = glm::vec4(0.5f, 0.5f, 0.0f, 0.0f);

            LightmapAsset asset;
            asset.SetDimensions(8, 8, pageCount);
            asset.AllocateTexels();
            asset.SetEntries(std::vector<LightmapEntityEntry>{ entry });
            return asset.Validate();
        };

        EXPECT_TRUE(validates(1, 0));
        EXPECT_TRUE(validates(4, 3)) << "page 3 of 4 is legal now — this is what #868 unlocks";
        EXPECT_FALSE(validates(1, 1)) << "page 1 of a 1-page asset must be rejected";
        EXPECT_FALSE(validates(4, 4)) << "page 4 of a 4-page asset must be rejected";
        EXPECT_FALSE(validates(4, kMaxLightmapPages));
    }
} // namespace OloEngine::Tests
