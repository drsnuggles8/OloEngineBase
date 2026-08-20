#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Baking/LightmapBaker.h"

#include "OloEngine/Renderer/Baking/LightmapUnwrap.h"
#include "OloEngine/Renderer/AtlasAllocator.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Task/ParallelFor.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace OloEngine
{
    namespace
    {
        struct EntityPlan
        {
            sizet InputIndex = 0;
            u32 RegionSize = 0;
            u32 AllocatorNode = AtlasAllocator::kInvalidNode;
            AtlasAllocator::Region Region{};
        };

        [[nodiscard]] bool IsCancelled(const std::atomic<bool>* cancelToken)
        {
            return cancelToken && cancelToken->load(std::memory_order_acquire);
        }

        void ReportProgress(std::atomic<f32>* progress, f32 value)
        {
            if (progress)
                progress->store(value, std::memory_order_release);
        }

        // World-space surface area of a mesh under a transform — the texel-density
        // driver for its atlas region.
        [[nodiscard]] f64 WorldSurfaceArea(const MeshSource& mesh, const glm::mat4& worldTransform)
        {
            const auto& vertices = mesh.GetVertices();
            const auto& indices = mesh.GetIndices();

            f64 area = 0.0;
            for (i32 i = 0; i + 2 < indices.Num(); i += 3)
            {
                const glm::vec3 p0 = glm::vec3(worldTransform * glm::vec4(vertices[static_cast<i32>(indices[i + 0])].Position, 1.0f));
                const glm::vec3 p1 = glm::vec3(worldTransform * glm::vec4(vertices[static_cast<i32>(indices[i + 1])].Position, 1.0f));
                const glm::vec3 p2 = glm::vec3(worldTransform * glm::vec4(vertices[static_cast<i32>(indices[i + 2])].Position, 1.0f));
                area += 0.5 * static_cast<f64>(glm::length(glm::cross(p1 - p0, p2 - p0)));
            }
            return area;
        }

        [[nodiscard]] u32 CeilPow2(u32 v)
        {
            return v <= 1 ? 1u : std::bit_ceil(v);
        }

        // Rasterizes one entity's triangles in atlas texel space, appending a
        // job per covered texel. Coverage is a texel-centre test; chart padding
        // plus post-bake dilation covers the conservative edge band, and
        // sub-texel triangles force their centroid texel so no chart bakes empty.
        void RasterizeEntity(const MeshSource& mesh, const glm::mat4& worldTransform,
                             const glm::vec4& scaleOffset, u32 atlasSize,
                             std::vector<LightmapTexelJob>& jobs, std::vector<u8>& texelClaimed)
        {
            const auto& vertices = mesh.GetVertices();
            const auto& indices = mesh.GetIndices();
            const auto& lightmapUVs = mesh.GetLightmapUVs();

            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(worldTransform)));
            const f32 atlasSizeF = static_cast<f32>(atlasSize);

            auto emitTexel = [&](u32 x, u32 y, const glm::vec3& pos, const glm::vec3& normal)
            {
                const sizet claimIndex = static_cast<sizet>(y) * atlasSize + x;
                if (texelClaimed[claimIndex] != 0)
                    return; // first-writer-wins keeps overlaps (chart padding rounding) deterministic
                texelClaimed[claimIndex] = 1;
                jobs.push_back(LightmapTexelJob{ x, y, pos, normal });
            };

            for (i32 i = 0; i + 2 < indices.Num(); i += 3)
            {
                const i32 i0 = static_cast<i32>(indices[i + 0]);
                const i32 i1 = static_cast<i32>(indices[i + 1]);
                const i32 i2 = static_cast<i32>(indices[i + 2]);

                // Atlas-space texel coordinates of the triangle's lightmap UVs
                const glm::vec2 a0 = (lightmapUVs[i0] * glm::vec2(scaleOffset.x, scaleOffset.y) + glm::vec2(scaleOffset.z, scaleOffset.w)) * atlasSizeF;
                const glm::vec2 a1 = (lightmapUVs[i1] * glm::vec2(scaleOffset.x, scaleOffset.y) + glm::vec2(scaleOffset.z, scaleOffset.w)) * atlasSizeF;
                const glm::vec2 a2 = (lightmapUVs[i2] * glm::vec2(scaleOffset.x, scaleOffset.y) + glm::vec2(scaleOffset.z, scaleOffset.w)) * atlasSizeF;

                const glm::vec3 w0 = glm::vec3(worldTransform * glm::vec4(vertices[i0].Position, 1.0f));
                const glm::vec3 w1 = glm::vec3(worldTransform * glm::vec4(vertices[i1].Position, 1.0f));
                const glm::vec3 w2 = glm::vec3(worldTransform * glm::vec4(vertices[i2].Position, 1.0f));

                const glm::vec3 n0 = normalMatrix * vertices[i0].Normal;
                const glm::vec3 n1 = normalMatrix * vertices[i1].Normal;
                const glm::vec3 n2 = normalMatrix * vertices[i2].Normal;

                const f32 denom = (a1.x - a0.x) * (a2.y - a0.y) - (a2.x - a0.x) * (a1.y - a0.y);
                constexpr f32 kDegenerateArea = 1e-8f;
                if (std::abs(denom) < kDegenerateArea)
                    continue; // zero-area chart triangle — nothing to cover

                const i32 minX = std::max(0, static_cast<i32>(std::floor(std::min({ a0.x, a1.x, a2.x }))));
                const i32 maxX = std::min(static_cast<i32>(atlasSize) - 1, static_cast<i32>(std::ceil(std::max({ a0.x, a1.x, a2.x }))));
                const i32 minY = std::max(0, static_cast<i32>(std::floor(std::min({ a0.y, a1.y, a2.y }))));
                const i32 maxY = std::min(static_cast<i32>(atlasSize) - 1, static_cast<i32>(std::ceil(std::max({ a0.y, a1.y, a2.y }))));
                if (minX > maxX || minY > maxY)
                    continue;

                bool coveredAnyTexel = false;
                for (i32 y = minY; y <= maxY; ++y)
                {
                    for (i32 x = minX; x <= maxX; ++x)
                    {
                        const glm::vec2 p(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f);
                        const f32 b1 = ((p.x - a0.x) * (a2.y - a0.y) - (a2.x - a0.x) * (p.y - a0.y)) / denom;
                        const f32 b2 = ((a1.x - a0.x) * (p.y - a0.y) - (p.x - a0.x) * (a1.y - a0.y)) / denom;
                        const f32 b0 = 1.0f - b1 - b2;
                        if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f)
                            continue;

                        const glm::vec3 pos = b0 * w0 + b1 * w1 + b2 * w2;
                        glm::vec3 normal = b0 * n0 + b1 * n1 + b2 * n2;
                        const f32 len = glm::length(normal);
                        if (len < 1e-6f)
                            continue;
                        normal /= len;

                        emitTexel(static_cast<u32>(x), static_cast<u32>(y), pos, normal);
                        coveredAnyTexel = true;
                    }
                }

                // A triangle smaller than a texel never passes the centre test;
                // without this its whole chart could bake empty and dilation
                // would have nothing to pull from.
                if (!coveredAnyTexel)
                {
                    const glm::vec2 centroid = (a0 + a1 + a2) / 3.0f;
                    const i32 cx = std::clamp(static_cast<i32>(centroid.x), 0, static_cast<i32>(atlasSize) - 1);
                    const i32 cy = std::clamp(static_cast<i32>(centroid.y), 0, static_cast<i32>(atlasSize) - 1);
                    const glm::vec3 pos = (w0 + w1 + w2) / 3.0f;
                    glm::vec3 normal = n0 + n1 + n2;
                    const f32 len = glm::length(normal);
                    if (len >= 1e-6f)
                        emitTexel(static_cast<u32>(cx), static_cast<u32>(cy), pos, normal / len);
                }
            }
        }

        // Pull-dilation: each pass fills empty texels bordering baked ones with
        // the average of their baked neighbours, so bilinear sampling at chart
        // edges never blends toward the atlas clear colour.
        void DilateAtlas(std::vector<f32>& texels, u32 atlasSize, u32 passes)
        {
            const sizet texelCount = static_cast<sizet>(atlasSize) * atlasSize;
            std::vector<f32> previous;
            for (u32 pass = 0; pass < passes; ++pass)
            {
                previous = texels;
                for (sizet t = 0; t < texelCount; ++t)
                {
                    if (previous[t * 4 + 3] > 0.0f)
                        continue;

                    const i32 x = static_cast<i32>(t % atlasSize);
                    const i32 y = static_cast<i32>(t / atlasSize);
                    glm::vec3 sum(0.0f);
                    i32 validNeighbours = 0;
                    for (i32 dy = -1; dy <= 1; ++dy)
                    {
                        for (i32 dx = -1; dx <= 1; ++dx)
                        {
                            if (dx == 0 && dy == 0)
                                continue;
                            const i32 nx = x + dx;
                            const i32 ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= static_cast<i32>(atlasSize) || ny >= static_cast<i32>(atlasSize))
                                continue;
                            const sizet n = static_cast<sizet>(ny) * atlasSize + nx;
                            if (previous[n * 4 + 3] > 0.0f)
                            {
                                sum += glm::vec3(previous[n * 4 + 0], previous[n * 4 + 1], previous[n * 4 + 2]);
                                ++validNeighbours;
                            }
                        }
                    }
                    if (validNeighbours > 0)
                    {
                        const glm::vec3 average = sum / static_cast<f32>(validNeighbours);
                        texels[t * 4 + 0] = average.x;
                        texels[t * 4 + 1] = average.y;
                        texels[t * 4 + 2] = average.z;
                        texels[t * 4 + 3] = 1.0f;
                    }
                }
            }
        }
    } // namespace

    bool LightmapBaker::Prepare(std::span<const LightmapBakeInput> entities,
                                const LightmapBakeSettings& settings,
                                LightmapBakePrepared& outPrepared,
                                std::string& outError)
    {
        OLO_PROFILE_FUNCTION();

        outPrepared = LightmapBakePrepared{};

        if (settings.AtlasSize == 0 || !std::has_single_bit(settings.AtlasSize) ||
            settings.MinRegionSize == 0 || !std::has_single_bit(settings.MinRegionSize) ||
            settings.MinRegionSize > settings.AtlasSize)
        {
            outError = "invalid atlas dimensions (power-of-two AtlasSize/MinRegionSize required)";
            return false;
        }
        if (entities.empty())
        {
            outError = "no static entities to bake";
            return false;
        }

        // ── 1. Unwrap: every distinct mesh gets a lightmap parameterization ──
        // Generate() is idempotent, so entities sharing a MeshSource unwrap once.
        LightmapUnwrapOptions unwrapOptions;
        unwrapOptions.Resolution = settings.UnwrapResolution;
        unwrapOptions.Padding = settings.UnwrapPadding;

        std::vector<bool> unwrapFailed(entities.size(), false);
        for (sizet e = 0; e < entities.size(); ++e)
        {
            const auto& input = entities[e];
            if (!input.Mesh || input.Mesh->GetVertices().IsEmpty())
            {
                unwrapFailed[e] = true;
                continue;
            }
            // Local non-const Ref copy: the span is const (the INPUT LIST is
            // immutable) but the unwrap intentionally mutates the referenced
            // mesh, and Ref<T> propagates const through operator->/operator*.
            Ref<MeshSource> mesh = input.Mesh;
            if (!LightmapUnwrap::Generate(*mesh, unwrapOptions))
            {
                OLO_CORE_WARN("LightmapBaker: unwrap failed for entity {:x}; it will not receive a lightmap",
                              input.EntityUUID);
                unwrapFailed[e] = true;
            }
        }

        // ── 2. Atlas regions: sized by world-space area, packed deterministically ──
        std::vector<EntityPlan> plans;
        plans.reserve(entities.size());
        for (sizet e = 0; e < entities.size(); ++e)
        {
            if (unwrapFailed[e])
                continue;
            const auto& input = entities[e];

            const f64 area = WorldSurfaceArea(*input.Mesh, input.WorldTransform);
            const f64 desiredTexels = area * static_cast<f64>(settings.TexelsPerMeter) * static_cast<f64>(settings.TexelsPerMeter);
            const u32 desiredSize = CeilPow2(static_cast<u32>(std::ceil(std::sqrt(std::max(desiredTexels, 1.0)))));

            EntityPlan plan;
            plan.InputIndex = e;
            plan.RegionSize = std::clamp(desiredSize, settings.MinRegionSize, settings.AtlasSize);
            plans.push_back(plan);
        }

        if (plans.empty())
        {
            outError = "no bakeable entities (every unwrap failed)";
            return false;
        }

        // Deterministic packing order: big regions first (packing quality), UUID
        // as the tie-break so two bakes of the same scene place identically.
        std::sort(plans.begin(), plans.end(), [&](const EntityPlan& a, const EntityPlan& b)
                  {
            if (a.RegionSize != b.RegionSize)
                return a.RegionSize > b.RegionSize;
            return entities[a.InputIndex].EntityUUID < entities[b.InputIndex].EntityUUID; });

        AtlasAllocator allocator(settings.AtlasSize, settings.MinRegionSize);
        for (auto& plan : plans)
        {
            u32 size = plan.RegionSize;
            // Degrade a region rather than dropping the entity: half the
            // resolution is a visibly better failure mode than no lightmap.
            while (size >= settings.MinRegionSize)
            {
                plan.AllocatorNode = allocator.Allocate(size);
                if (plan.AllocatorNode != AtlasAllocator::kInvalidNode)
                {
                    plan.RegionSize = size;
                    plan.Region = allocator.GetRegion(plan.AllocatorNode);
                    break;
                }
                size /= 2;
            }
            if (plan.AllocatorNode == AtlasAllocator::kInvalidNode)
            {
                OLO_CORE_WARN("LightmapBaker: atlas exhausted — entity {:x} gets no lightmap (raise AtlasSize or lower TexelsPerMeter)",
                              entities[plan.InputIndex].EntityUUID);
            }
        }

        // ── 3. Rasterize every placed entity's charts into texel jobs ──
        const sizet texelCount = static_cast<sizet>(settings.AtlasSize) * settings.AtlasSize;
        std::vector<u8> texelClaimed(texelCount, 0);

        for (const auto& plan : plans)
        {
            if (plan.AllocatorNode == AtlasAllocator::kInvalidNode)
            {
                ++outPrepared.SkippedEntityCount;
                continue;
            }

            const auto& input = entities[plan.InputIndex];
            const f32 atlasSizeF = static_cast<f32>(settings.AtlasSize);
            const glm::vec4 scaleOffset(static_cast<f32>(plan.RegionSize) / atlasSizeF,
                                        static_cast<f32>(plan.RegionSize) / atlasSizeF,
                                        static_cast<f32>(plan.Region.X) / atlasSizeF,
                                        static_cast<f32>(plan.Region.Y) / atlasSizeF);

            RasterizeEntity(*input.Mesh, input.WorldTransform, scaleOffset, settings.AtlasSize,
                            outPrepared.Jobs, texelClaimed);

            LightmapEntityEntry entry;
            entry.EntityUUID = input.EntityUUID;
            entry.Page = 0;
            entry.ScaleOffset = scaleOffset;
            outPrepared.Entries.push_back(entry);
            ++outPrepared.BakedEntityCount;
        }

        if (outPrepared.Jobs.empty())
        {
            outError = "rasterization produced no texels (are the meshes degenerate?)";
            return false;
        }

        outPrepared.AtlasSize = settings.AtlasSize;
        return true;
    }

    LightmapBakeResult LightmapBaker::BakeTexels(const LightmapBakePrepared& prepared,
                                                 const PathTracing::ReferenceScene& world,
                                                 const LightmapBakeSettings& settings,
                                                 std::atomic<f32>* progress,
                                                 const std::atomic<bool>* cancelToken)
    {
        OLO_PROFILE_FUNCTION();

        LightmapBakeResult result;
        result.BakedEntityCount = prepared.BakedEntityCount;
        result.SkippedEntityCount = prepared.SkippedEntityCount;

        if (settings.SamplesPerTexel == 0)
        {
            result.Error = "SamplesPerTexel must be non-zero";
            return result;
        }
        if (prepared.AtlasSize == 0 || prepared.Jobs.empty())
        {
            result.Error = "nothing prepared to bake";
            return result;
        }

        ReportProgress(progress, 0.0f);

        // ── The bake: one EstimateIrradiance per texel, parallel over jobs ──
        // Each texel's seed derives from its atlas coordinates, so the estimate
        // is independent of scheduling — the whole bake is deterministic.
        const sizet texelCount = static_cast<sizet>(prepared.AtlasSize) * prepared.AtlasSize;
        std::vector<f32> texels(texelCount * 4, 0.0f);

        PathTracing::PathTracerSettings tracerSettings;
        tracerSettings.SamplesPerPixel = settings.SamplesPerTexel;
        tracerSettings.MaxBounces = settings.MaxBounces;
        tracerSettings.RussianRouletteStartBounce = 0; // deterministic cost, lower variance (matches the DDGI parity fixtures)
        tracerSettings.Seed = settings.Seed;

        std::atomic<sizet> jobsDone{ 0 };
        std::atomic<bool> sawCancel{ false };

        ParallelFor(
            "LightmapBaker::BakeTexels",
            static_cast<i32>(prepared.Jobs.size()),
            16, // MinBatchSize: each job is SamplesPerTexel full paths
            [&](i32 jobIndex)
            {
                if (sawCancel.load(std::memory_order_relaxed))
                    return;
                if (IsCancelled(cancelToken))
                {
                    sawCancel.store(true, std::memory_order_relaxed);
                    return;
                }

                const LightmapTexelJob& job = prepared.Jobs[static_cast<sizet>(jobIndex)];
                const u32 texelSeed = PathTracing::MakePixelSeed(job.AtlasX, job.AtlasY, settings.Seed);
                const glm::vec3 irradiance = PathTracing::PathTracer::EstimateIrradiance(
                    world, job.WorldPos, job.WorldNormal, tracerSettings, texelSeed);

                const sizet t = (static_cast<sizet>(job.AtlasY) * prepared.AtlasSize + job.AtlasX) * 4;
                texels[t + 0] = irradiance.x;
                texels[t + 1] = irradiance.y;
                texels[t + 2] = irradiance.z;
                texels[t + 3] = 1.0f;

                const sizet done = jobsDone.fetch_add(1, std::memory_order_relaxed) + 1;
                if (progress && (done % 256 == 0))
                    ReportProgress(progress, 0.95f * static_cast<f32>(done) / static_cast<f32>(prepared.Jobs.size()));
            });

        if (sawCancel.load(std::memory_order_relaxed) || IsCancelled(cancelToken))
        {
            result.Error = "cancelled";
            return result;
        }
        ReportProgress(progress, 0.95f);

        // ── Dilation + asset assembly ──
        DilateAtlas(texels, prepared.AtlasSize, settings.DilationPasses);

        auto asset = Ref<LightmapAsset>::Create();
        asset->SetDimensions(prepared.AtlasSize, prepared.AtlasSize, 1);
        asset->SetBakeKey(settings.BakeKey);
        asset->SetTexelData(std::move(texels));
        asset->SetEntries(std::vector<LightmapEntityEntry>(prepared.Entries));

        result.Asset = asset;
        result.Success = true;
        ReportProgress(progress, 1.0f);
        return result;
    }

    LightmapBakeResult LightmapBaker::Bake(std::span<const LightmapBakeInput> entities,
                                           const PathTracing::ReferenceScene& world,
                                           const LightmapBakeSettings& settings,
                                           std::atomic<f32>* progress,
                                           const std::atomic<bool>* cancelToken)
    {
        LightmapBakeResult result;

        LightmapBakePrepared prepared;
        if (std::string error; !Prepare(entities, settings, prepared, error))
        {
            result.Error = std::move(error);
            return result;
        }
        if (IsCancelled(cancelToken))
        {
            result.Error = "cancelled";
            return result;
        }
        return BakeTexels(prepared, world, settings, progress, cancelToken);
    }
} // namespace OloEngine
