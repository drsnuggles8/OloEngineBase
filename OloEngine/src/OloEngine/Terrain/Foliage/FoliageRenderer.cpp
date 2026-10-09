#include "OloEnginePCH.h"
#include <limits>
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Terrain/Foliage/FoliageWind.h"
#include "OloEngine/Terrain/Foliage/FoliageAlphaCoverage.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include "OloEngine/Wind/WindSystem.h"
#include "FoliageRenderer.h"
#include "FoliageStreamingPayload.h"
#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/AlphaCoverageMips.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Project/ContentPath.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RayTracing/VegetationDiagnostics.h"
#include "OloEngine/Renderer/RayTracing/VegetationSurfaceCache.h"
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Terrain/Foliage/FoliagePlacement.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Impostor/ImpostorBaker.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/MaterialKind.h"
#include "OloEngine/Renderer/PBRModel.h"

#include <glm/gtc/constants.hpp>

#include <bit>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace OloEngine
{
    // THE CARD IS THE MESH'S BAKE ONLY WHEN THE MESH HAS ITS OWN TEXTURES.
    // Every imported plant carries its own (its .mtl), and its card is the
    // vegetation import's billboard of it, over the mesh's own frame. A mesh with
    // no material of its own is drawn with the layer's albedo, which is then a
    // texture for the mesh's UVs rather than a picture of the plant: drawn as a
    // bake, the procedural pine.obj with grass.png that the foliage fixtures
    // author turned from a 1 m strip into a 10 m grass tuft. So any part that
    // borrows the layer's albedo keeps the whole layer on the legacy card.
    f32 FoliageRenderer::MeshLayerCardLane(const LayerRenderData& data)
    {
        if (!data.CardUsesMeshBake)
            return 0.0f;
        return FoliageLod::CardNormalLane(data.CardNormalTilt);
    }

    void FoliageRenderer::QueueRayTracing(u64 owner, const glm::vec3& cameraPosition) const
    {
        if (!Renderer3D::WantsRayTracingVegetation())
            return;
        OLO_PERF_SCOPE_AUTO("Vegetation::Queue");
        auto& cache = Renderer3D::GetVegetationSurfaceCache();
        const auto field = WindSystem::GetGPUData();
        // sqrt(||A||_1 * ||A||_infinity) bounds the spectral norm and retains
        // norm 1 for an identity terrain (unlike the Frobenius bound).
        f32 maxColumn = 0.0f, maxRow = 0.0f;
        for (u32 axis = 0u; axis < 3u; ++axis)
        {
            f32 column = 0.0f, row = 0.0f;
            for (u32 lane = 0u; lane < 3u; ++lane)
            {
                column += std::abs(m_TerrainTransform[axis][lane]);
                row += std::abs(m_TerrainTransform[lane][axis]);
            }
            maxColumn = std::max(maxColumn, column);
            maxRow = std::max(maxRow, row);
        }
        const f32 transformNorm = std::sqrt(maxColumn * maxRow);
        const bool reflectionsReadVegetation = Renderer3D::GetPostProcessSettings().RayTracedReflection.Enabled;
        const auto& rayTracingScene = Renderer3D::GetRayTracingScene();
        // What one group of `plants` occupies, staged and as acceleration
        // structures: one BLAS per part, each over the group's whole vertex
        // stream, sized by the device (#1533). The cache budgets both.
        const auto geometryBytes = [](const LayerRenderData& layer, bool mesh, u64 plants) -> u64
        {
            return plants * ((mesh ? static_cast<u64>(layer.MeshVertexCount) : 4u) * sizeof(Vertex) +
                             (mesh ? static_cast<u64>(layer.MeshRayTracingIndices.Num()) : 6u) * sizeof(u32) + sizeof(FoliageInstanceData));
        };
        const auto accelerationBytes = [&](const LayerRenderData& layer, bool mesh, u64 plants) -> u64
        {
            const u64 vertices = plants * (mesh ? static_cast<u64>(layer.MeshVertexCount) : 4u);
            if (vertices > std::numeric_limits<u32>::max())
                return std::numeric_limits<u64>::max();
            if (!mesh)
                return rayTracingScene.EstimateDeformedBlasBytes(static_cast<u32>(vertices), static_cast<u32>(plants * 2u));
            u64 bytes = 0u;
            for (const auto& part : layer.MeshParts)
                if (part.IndexCount > 0u)
                    bytes += rayTracingScene.EstimateDeformedBlasBytes(static_cast<u32>(vertices),
                                                                       static_cast<u32>(plants * (part.IndexCount / 3u)));
            return bytes;
        };
        // How fast a layer's plants can move, world units a second.
        const auto velocityBound = [&](const LayerRenderData& layer, bool impostor) -> f32
        {
            const bool hierarchy = layer.WindWeights.x + layer.WindWeights.y + layer.WindWeights.z > 0.0f;
            f32 bound = RayTracing::VegetationPolicy::WindVelocityBound(
                layer.WindStrength, layer.WindSpeed, layer.WindWeights.y, layer.WindWeights.z,
                hierarchy, field.TimeAndFlags.y > 0.5f && (hierarchy || !impostor), field.DirectionAndSpeed.w,
                field.GustAndTurbulence.x, field.GustAndTurbulence.y, transformNorm);
            // A running actor moves a plant far faster than wind does,
            // and ProxyAgeLimit is error/velocity — so a bound that
            // ignored interaction would keep serving a snapshot taken
            // before the actor arrived. Reported by the field from its
            // own springs rather than estimated here.
            if (const f32 bendRate = FoliageInteractionField::GetMaximumBendRate();
                std::isfinite(bendRate) && std::isfinite(bound))
                bound += bendRate * layer.InteractionResponse * transformNorm;
            return bound;
        };
        // Where a casting group stops refreshing every frame (#1354, and
        // queueGroup's DetailedDistance below).
        const auto castingDetailedDistance = [](const LayerRenderData& layer, bool mesh)
        {
            return mesh ? std::max(50.0f, layer.MeshFadeStartDistance) : RayTracing::VegetationPolicy::DetailedCardDistance;
        };
        // One group of one layer's plants in one representation, described and
        // queued -- or refused, counted against that layer's completeness.
        const auto queueGroup = [&](const LayerRenderData& layer, u32 representation,
                                    std::span<const FoliageInstanceRecord* const> records, u64 estimatedBytes,
                                    u64 estimatedAccelerationBytes, f32 distanceToView)
        {
            const bool mesh = representation != 2u;
            const bool impostor = representation == 1u;
            if (!cache.HasQueueCapacity() || estimatedBytes > RayTracing::VegetationPolicy::GeometryBytes ||
                estimatedAccelerationBytes > RayTracing::VegetationPolicy::AccelerationStructureBytes)
            {
                using RayTracing::VegetationPressure;
                cache.Refuse(layer.CastShadows, estimatedAccelerationBytes > RayTracing::VegetationPolicy::AccelerationStructureBytes
                                                    ? VegetationPressure::AccelerationMemory
                                                : cache.GetQueuedGroups() >= RayTracing::VegetationPolicy::ResidentGroups
                                                    ? VegetationPressure::ResidentGroups
                                                    : VegetationPressure::GeometryMemory);
                return;
            }
            RayTracing::VegetationSurfaceInput input;
            input.Owner = owner;
            input.FirstPlantId = records[0]->m_Id;
            input.Rest = mesh ? layer.MeshVBO : layer.QuadVBO;
            input.VertexCount = mesh ? layer.MeshVertexCount : 4u;
            input.WorldTransform = m_TerrainTransform;
            // A reflection-only layer is a SNAPSHOT everywhere (#1533): its wind is
            // refreshed within the proxies' error deadline, not every frame, which
            // is what lets the lawn's near field fit the per-frame budget at all.
            input.DetailedDistance = !layer.CastShadows ? 0.0f : castingDetailedDistance(layer, mesh);
            input.DistanceToView = distanceToView;
            const u32 partCount = mesh ? static_cast<u32>(layer.MeshParts.Num()) : 1u;
            bool validParts = true;
            for (u32 part = 0u; part < partCount; ++part)
            {
                RayTracing::VegetationSurfacePart surface;
                surface.Slot = part;
                if (mesh)
                {
                    const auto& range = layer.MeshParts[part];
                    if (range.IndexCount == 0u)
                        continue;
                    if (static_cast<u64>(range.BaseIndex) + range.IndexCount > layer.MeshRayTracingIndices.Num())
                    {
                        validParts = false;
                        break;
                    }
                    surface.Indices.Append(layer.MeshRayTracingIndices.GetData() + range.BaseIndex,
                                           static_cast<i32>(range.IndexCount));
                }
                else
                    surface.Indices = { 0u, 1u, 2u, 2u, 3u, 0u };
                surface.Material.m_BaseColorFactor = glm::vec4(layer.BaseColor, 1.0f);
                surface.Material.m_RoughnessFactor = layer.LeafRoughness;
                surface.Material.m_AlphaMode = std::to_underlying(AlphaMode::Mask);
                surface.Material.m_AlphaCutoff = layer.AlphaCutoff;
                surface.Material.m_ClosureVersion = std::to_underlying(PBRModel::ClosureV2);
                surface.Material.m_MaterialKind = std::to_underlying(MaterialKind::Foliage);
                surface.Material.m_Flags = GPUSceneMaterialFlagTwoSided | GPUSceneMaterialFlagDepthTest;
                const auto albedo = mesh && layer.MeshParts[part].Albedo ? layer.MeshParts[part].Albedo : layer.AlbedoTexture;
                if (albedo && albedo->IsLoaded())
                {
                    surface.Material.m_Albedo.m_Handle = albedo->GetRHIHandle();
                    surface.Material.m_Albedo.m_HeapOffset = HeapBinding::ResolveRecordTextureOffset(
                                                                 albedo->GetRHIHandle(), HeapBinding::MaterialTexture2DSampler())
                                                                 .Value;
                }
                input.Parts.Add(std::move(surface));
            }
            if (!validParts)
            {
                cache.Refuse(layer.CastShadows, RayTracing::VegetationPressure::InvalidContent);
                return;
            }
            // The rows' content key (#1533): the registry generation advances on
            // any change to a plant, so it, the plant ids and the layer values
            // written into each row name the rows exactly. A group the cache
            // already holds under it is sent without rows.
            u64 contentKey = 0x9e3779b97f4a7c15ull;
            const auto mixKey = [&contentKey](u64 value)
            {
                contentKey = (contentKey ^ value) * 0x100000001b3ull;
                contentKey ^= contentKey >> 29u;
            };
            mixKey(m_Registry.GetGeneration());
            mixKey(RHI::HashKey(input.Rest->GetRHIHandle()));
            mixKey(input.VertexCount);
            mixKey(std::bit_cast<u32>(layer.BaseColor.x) | (static_cast<u64>(std::bit_cast<u32>(layer.BaseColor.y)) << 32u));
            mixKey(std::bit_cast<u32>(layer.BaseColor.z) | (static_cast<u64>(std::bit_cast<u32>(layer.AlphaCutoff)) << 32u));
            // And the plants' identity without their order (#1354), which the
            // cache reads to tell a regroup from a representation change.
            u64 plantSet = 0u;
            for (const FoliageInstanceRecord* record : records)
            {
                mixKey(record->m_Id);
                plantSet += RayTracing::VegetationPlantTerm(record->m_Id);
            }
            input.ContentKey = contentKey != 0u ? contentKey : 1u;
            input.PlantSetSum = plantSet != 0u ? plantSet : 1u;
            input.ContentGeneration = m_Registry.GetGeneration();
            if (cache.HoldsContent(owner, input.FirstPlantId, input.Rest, input.ContentKey))
                input.HeldPlantCount = static_cast<u32>(records.size());
            else
            {
                input.Rows.Reserve(static_cast<i32>(records.size()));
                for (const FoliageInstanceRecord* plantRecord : records)
                {
                    const auto& record = *plantRecord;
                    input.Rows.Add({ glm::vec4(record.m_Position, record.m_Scale),
                                     glm::vec4(record.m_Rotation, record.m_Height, 1.0f, FoliageWindPhase(record.m_Id)),
                                     glm::vec4(layer.BaseColor, layer.AlphaCutoff) });
                }
            }
            auto& wind = input.Wind;
            wind.Time = m_Time;
            wind.PrevTime = m_PrevTime;
            wind.WindStrength = layer.WindStrength;
            wind.WindSpeed = layer.WindSpeed;
            wind.WindWeights = layer.WindWeights;
            wind.WindDirection = field.DirectionAndSpeed;
            wind.WindGust = field.GustAndTurbulence;
            wind.WindClock = field.TimeAndFlags;
            wind.WindFlags = glm::vec4(Renderer3D::GetRenderOrigin(), field.TimeAndFlags.y);
            wind.WindHistoryValid = WindSystem::HasStableParameters() ? 1.0f : 0.0f;
            wind.MeshParams.x = mesh ? 1.0f : 0.0f;
            wind.MeshParams.z = impostor ? 1.0f : 0.0f;
            // A mesh layer's card is the mesh's bake (#1533): sized like
            // the mesh, with the mesh's mean normal (the lane). Kept at the
            // plant's own yaw here, unlike the raster card: a proxy turned
            // to the main view would be neither a ray-space silhouette nor
            // a cacheable one.
            wind.MeshParams.w = mesh ? 0.0f : MeshLayerCardLane(layer);
            wind.ImpostorParams1.x = impostor ? 1.0f : 0.0f;
            // The ray-traced vegetation representation (issue #1240)
            // takes the same influence set as the raster passes, so a
            // plant a character is standing on casts a bent ray-traced
            // shadow too. It is a SNAPSHOT: the cache's refresh key
            // deliberately excludes time (see VegetationSurfaceCache),
            // so interaction reaches RT on the same cadence wind phase
            // does, not per frame. The velocity bound below is what
            // keeps that cadence honest.
            ApplyFoliageInteraction(wind, layer.InteractionResponse);
            input.HistoryContinuous = WindSystem::HasStableParameters() && m_Time >= m_PrevTime;
            input.CastShadows = layer.CastShadows;
            input.AccelerationBytes = estimatedAccelerationBytes;
            input.VelocityBound = velocityBound(layer, impostor);
            cache.Queue(std::move(input));
        };
        // What the cached split was taken from, besides the camera: every
        // layer field that decides a plant's group, tier or cost.
        u64 layersKey = 0xcbf29ce484222325ull;
        const auto mixLayers = [&layersKey](u64 value)
        {
            layersKey = (layersKey ^ value) * 0x100000001b3ull;
            layersKey ^= layersKey >> 29u;
        };
        for (const auto& layer : m_Layers)
        {
            mixLayers(layer.CastShadows ? 1u : 0u);
            mixLayers(std::bit_cast<u32>(layer.ViewDistance) | (static_cast<u64>(std::bit_cast<u32>(layer.MeshViewDistance)) << 32u));
            mixLayers((layer.UseImpostor ? 1u : 0u) | (layer.Impostor.IsValid() ? 2u : 0u));
            mixLayers(layer.MeshVBO ? RHI::HashKey(layer.MeshVBO->GetRHIHandle()) : 0u);
            mixLayers(layer.QuadVBO ? RHI::HashKey(layer.QuadVBO->GetRHIHandle()) : 0u);
            mixLayers(layer.MeshVertexCount | (static_cast<u64>(layer.MeshRayTracingIndices.Num()) << 32u));
            for (const auto& part : layer.MeshParts)
                mixLayers(part.IndexCount);
        }
        const bool splitReused = reflectionsReadVegetation && m_ReflectionSplit.Valid &&
                                 m_ReflectionSplit.RecordsEpoch == m_Registry.GetRecordsEpoch() &&
                                 m_ReflectionSplit.LayersKey == layersKey &&
                                 Math::BitwiseEqual(m_ReflectionSplit.Terrain, m_TerrainTransform) &&
                                 glm::length(cameraPosition - m_ReflectionSplit.Camera) < kReflectionSplitTolerance;
        auto& reflectionCandidates = m_ReflectionSplit.Candidates;
        auto& reflectionRecords = m_ReflectionSplit.Records;
        if (!splitReused)
        {
            reflectionCandidates.Reset();
            reflectionRecords.Reset();
        }
        // The casting groups, planned before any is queued (#1354): a caster
        // that does not fit is no longer refused on its own, taking every
        // ray-traced effect with it, while a complete cheaper tier would fit.
        struct CastingCandidate
        {
            u32 LayerIndex = 0u;
            u32 Representation = 0u;
            RayTracing::VegetationPolicy::CastingGroupCost Cost;
            /// The registry cell the group was sliced from: the hysteresis key.
            u32 Cell = 0u;
            f32 Distance = 0.0f;
            u64 FirstId = 0u;
            u32 FirstRecord = 0u;
            u32 RecordCount = 0u;
        };
        TArray<CastingCandidate> castingCandidates;
        // The wind clock's step since the last plan, which sets how often a
        // proxy refreshes. The cache times a layer on the clock its wind is
        // evaluated on: the wind field's for a legacy single-weight layer
        // (VegetationSurfaceCache's WindTime), the scene animation clock
        // otherwise.
        // A clock that did not move since the last plan (a second view this
        // frame, a paused scene) keeps the last real step: a zero step would
        // charge no refresh and flip every group to the mesh until it moves.
        const auto stepSince = [](f32 now, f32 last, f32& kept) -> f32
        {
            if (last >= 0.0f && std::isfinite(now) && now > last)
                kept = now - last;
            return kept;
        };
        const f32 animationStep = stepSince(m_Time, m_PlanAnimationTime, m_PlanAnimationStep);
        const f32 fieldStep = stepSince(field.TimeAndFlags.x, m_PlanFieldTime, m_PlanFieldStep);
        m_PlanAnimationTime = m_Time;
        m_PlanFieldTime = field.TimeAndFlags.x;
        const auto frameSecondsFor = [&](const LayerRenderData& layer, bool impostor) -> f32
        {
            const bool legacyField = layer.WindWeights.x + layer.WindWeights.y + layer.WindWeights.z <= 0.0f &&
                                     field.TimeAndFlags.y > 0.5f && !impostor;
            return legacyField ? fieldStep : animationStep;
        };
        // Per layer, once: each tier's velocity bound and clock step.
        struct LayerRates
        {
            f32 MeshVelocity = 0.0f;
            f32 ImpostorVelocity = 0.0f;
            f32 MeshStep = 0.0f;
            f32 ImpostorStep = 0.0f;
        };
        TArray<LayerRates> layerRates;
        layerRates.Reserve(m_Layers.Num());
        f32 frameSeconds = 0.0f;
        for (const auto& layer : m_Layers)
        {
            const LayerRates rates{ velocityBound(layer, false), velocityBound(layer, true), frameSecondsFor(layer, false),
                                    frameSecondsFor(layer, true) };
            frameSeconds = std::max({ frameSeconds, rates.MeshStep, rates.ImpostorStep });
            layerRates.Add(rates);
        }
        const bool forceDetailed = RayTracing::VegetationDiagnostics::GetForceDetailed();
        TArray<const FoliageInstanceRecord*> castingRecords;
        // Keyed by registry cell, which a camera move does not re-slice.
        const auto castingKey = [](u32 layerIndex, u32 cell)
        { return RayTracing::VegetationPlantTerm(static_cast<u64>(cell) ^ (static_cast<u64>(layerIndex) << 48u)); };
        // A tier's memory and the full refresh of it, in the backend's units:
        // one build per part, each over the group's whole vertex stream.
        const auto groupCost = [&](const LayerRenderData& layer, bool mesh, u64 plants) -> RayTracing::VegetationPolicy::GroupCost
        {
            RayTracing::VegetationPolicy::GroupCost cost;
            cost.GeometryBytes = geometryBytes(layer, mesh, plants);
            cost.AccelerationBytes = accelerationBytes(layer, mesh, plants);
            if (!mesh)
            {
                cost.RefreshVertices = 4u * plants;
                cost.RefreshTriangles = 2u * plants;
                cost.RefreshBuilds = 1u;
                return cost;
            }
            for (const auto& part : layer.MeshParts)
            {
                if (part.IndexCount == 0u)
                    continue;
                cost.RefreshVertices += plants * layer.MeshVertexCount;
                cost.RefreshTriangles += plants * (part.IndexCount / 3u);
                ++cost.RefreshBuilds;
            }
            return cost;
        };
        u32 cell = 0u;
        for (const auto& group : m_Registry.GetGroups())
        {
            const u32 groupCell = cell++;
            if (group.m_LayerIndex >= m_Layers.Num())
                continue;
            const auto& layer = m_Layers[group.m_LayerIndex];
            if (!RayTracing::VegetationPolicy::TracesLayer(layer.CastShadows, reflectionsReadVegetation))
                continue; // no ray this frame reads a layer that casts no shadow
            if (layer.UseImpostor && layer.Impostor.IsValid() && !layer.MeshVBO)
            {
                // An atlas without its source mesh cannot produce a ray-space
                // canopy. Count refusal and retain the whole raster tier.
                cache.Refuse(layer.CastShadows, RayTracing::VegetationPressure::InvalidContent);
                continue;
            }
            if (!layer.CastShadows && splitReused)
                continue; // its groups are in the cached split
            TArray<const FoliageInstanceRecord*> meshRecords, impostorRecords, cardRecords;
            TArray<f32> meshDistances, impostorDistances, cardDistances;
            // RebuildGroups keeps these in ascending canonical ID for us, with
            // each one's record index alongside: no lookup per plant.
            const auto& allRecords = m_Registry.GetRecords();
            for (const u32 index : group.m_RecordIndices)
            {
                const auto* record = &allRecords[static_cast<i32>(index)];
                const glm::vec3 worldRoot = glm::vec3(m_TerrainTransform * glm::vec4(record->m_Position, 1.0f));
                const f32 distance = glm::length(worldRoot - cameraPosition);
                if (distance > layer.ViewDistance)
                    continue;
                // Retain the authored canopy for an octahedral far LOD: a
                // single main-view-facing card is not a ray-space silhouette.
                const bool mesh = layer.MeshVBO && (!layer.MeshParts.IsEmpty()) &&
                                  ((layer.UseImpostor && layer.Impostor.IsValid()) || distance <= layer.MeshViewDistance);
                const bool impostor = mesh && layer.UseImpostor && layer.Impostor.IsValid() &&
                                      distance > layer.MeshViewDistance;
                (impostor ? impostorRecords : (mesh ? meshRecords : cardRecords)).Add(record);
                (impostor ? impostorDistances : (mesh ? meshDistances : cardDistances)).Add(distance);
            }
            for (const u32 representation : { 0u, 1u, 2u })
            {
                const bool mesh = representation != 2u;
                const bool impostor = representation == 1u;
                const auto& records = impostor ? impostorRecords : (mesh ? meshRecords : cardRecords);
                const auto& distances = impostor ? impostorDistances : (mesh ? meshDistances : cardDistances);
                const u32 plantsPerGroup = mesh ? RayTracing::VegetationPolicy::PlantsPerGroup : RayTracing::VegetationPolicy::CardPlantsPerGroup;
                for (sizet first = 0u; first < records.Num(); first += plantsPerGroup)
                {
                    const sizet count = std::min(records.Num() - first, static_cast<sizet>(plantsPerGroup));
                    const std::span<const FoliageInstanceRecord* const> slice{ records.GetData() + first, count };
                    const f32 nearest = *std::min_element(distances.GetData() + first, distances.GetData() + first + count);
                    const u64 estimatedBytes = geometryBytes(layer, mesh, count);
                    if (!layer.CastShadows)
                    {
                        // Reflection-only: a card group, with a mesh tier in
                        // the mesh distance, admitted nearest first, below.
                        ReflectionCandidate candidate;
                        candidate.LayerIndex = group.m_LayerIndex;
                        candidate.MeshRepresentation = representation;
                        candidate.Cost.CardGeometryBytes = geometryBytes(layer, false, count);
                        candidate.Cost.CardAccelerationBytes = accelerationBytes(layer, false, count);
                        if (mesh)
                        {
                            candidate.Cost.MeshGeometryBytes = estimatedBytes;
                            candidate.Cost.MeshAccelerationBytes = accelerationBytes(layer, true, count);
                        }
                        candidate.FirstId = slice.front()->m_Id;
                        candidate.FirstRecord = static_cast<u32>(reflectionRecords.Num());
                        candidate.RecordCount = static_cast<u32>(slice.size());
                        reflectionRecords.Append(slice.data(), static_cast<i32>(slice.size()));
                        candidate.Distance = nearest;
                        reflectionCandidates.Add(std::move(candidate));
                        continue;
                    }
                    // A caster's complete lower-cost tier is its card: the
                    // raster's own LOD past the mesh distance. An impostor
                    // layer has none (its far LOD is the atlas, and a card
                    // from the layer's albedo is not this plant).
                    CastingCandidate candidate;
                    candidate.LayerIndex = group.m_LayerIndex;
                    candidate.Representation = representation;
                    candidate.Cost.Requested = groupCost(layer, mesh, count);
                    candidate.Cost.HasFallback = representation == 0u && layer.QuadVBO && !(layer.UseImpostor && layer.Impostor.IsValid());
                    if (candidate.Cost.HasFallback)
                        candidate.Cost.Fallback = groupCost(layer, false, count);
                    candidate.FirstId = slice.front()->m_Id;
                    candidate.Cell = groupCell;
                    candidate.Cost.WasRequested = m_CastingRequested.contains(castingKey(group.m_LayerIndex, groupCell));
                    // The steady refresh demand of each tier (RefreshRate).
                    const LayerRates& rates = layerRates[static_cast<i32>(group.m_LayerIndex)];
                    candidate.Cost.RequestedRate = RayTracing::VegetationPolicy::RefreshRate(
                        forceDetailed || nearest <= castingDetailedDistance(layer, mesh), impostor ? rates.ImpostorVelocity : rates.MeshVelocity,
                        impostor ? rates.ImpostorStep : rates.MeshStep);
                    candidate.Cost.FallbackRate = RayTracing::VegetationPolicy::RefreshRate(
                        forceDetailed || nearest <= castingDetailedDistance(layer, false), rates.MeshVelocity, rates.MeshStep);
                    candidate.Distance = nearest;
                    candidate.FirstRecord = static_cast<u32>(castingRecords.Num());
                    candidate.RecordCount = static_cast<u32>(slice.size());
                    castingRecords.Append(slice.data(), static_cast<i32>(slice.size()));
                    castingCandidates.Add(std::move(candidate));
                }
            }
        }

        // THE CASTING GROUPS, nearest first (VegetationPolicy::ChooseCastingTiers):
        // every one at its cheapest complete tier, then the nearest at their
        // requested tier while that fits, memory and refresh demand alike.
        // The room is what the shared cache has left: another terrain's
        // foliage queued this frame has already spent part of it.
        {
            OLO_PERF_SCOPE_AUTO("Vegetation::QueueCasters");
            using RayTracing::VegetationPolicy;
            std::ranges::sort(castingCandidates, [](const CastingCandidate& a, const CastingCandidate& b)
                              { return std::make_pair(a.Distance, a.FirstId) < std::make_pair(b.Distance, b.FirstId); });
            TArray<VegetationPolicy::CastingGroupCost> costs;
            TArray<VegetationPolicy::CastingTier> tiers;
            costs.Reserve(castingCandidates.Num());
            for (const CastingCandidate& candidate : castingCandidates)
            {
                costs.Add(candidate.Cost);
                tiers.Add(VegetationPolicy::CastingTier::Out);
            }
            VegetationPolicy::PlanRoom room = VegetationPolicy::PlanRoom::Scaled(RayTracing::VegetationDiagnostics::GetBudgetDivisor());
            const auto& held = cache.GetStats();
            room.Take(cache.GetStagedBytes(), cache.GetStagedAccelerationBytes(), cache.GetQueuedGroups(), held.CastingDemandVertices,
                      held.CastingDemandTriangles, held.CastingDemandBuilds);
            const VegetationPolicy::CastingPlan plan = VegetationPolicy::ChooseCastingTiers(
                std::span<const VegetationPolicy::CastingGroupCost>{ costs.GetData(), static_cast<sizet>(costs.Num()) },
                std::span<VegetationPolicy::CastingTier>{ tiers.GetData(), static_cast<sizet>(tiers.Num()) }, room);
            m_CastingRequested.clear();
            f32 nearestFallback = 0.0f;
            bool anyFallback = false;
            for (i32 i = 0; i < castingCandidates.Num(); ++i)
            {
                const CastingCandidate& candidate = castingCandidates[i];
                const auto& layer = m_Layers[candidate.LayerIndex];
                const std::span<const FoliageInstanceRecord* const> records{ castingRecords.GetData() + candidate.FirstRecord,
                                                                             candidate.RecordCount };
                switch (tiers[i])
                {
                    case VegetationPolicy::CastingTier::Requested:
                        if (candidate.Cost.HasFallback)
                            m_CastingRequested.insert(castingKey(candidate.LayerIndex, candidate.Cell));
                        queueGroup(layer, candidate.Representation, records, candidate.Cost.Requested.GeometryBytes,
                                   candidate.Cost.Requested.AccelerationBytes, candidate.Distance);
                        break;
                    case VegetationPolicy::CastingTier::Fallback:
                        if (!anyFallback)
                            nearestFallback = candidate.Distance; // nearest first: the first is the nearest
                        anyFallback = true;
                        queueGroup(layer, 2u, records, candidate.Cost.Fallback.GeometryBytes, candidate.Cost.Fallback.AccelerationBytes,
                                   candidate.Distance);
                        break;
                    case VegetationPolicy::CastingTier::Out:
                        cache.Refuse(true, plan.Pressure);
                        break;
                }
            }
            cache.CountCastingPlan(plan, nearestFallback, frameSeconds);
        }

        // THE REFLECTION-ONLY GROUPS, nearest first: cards while they fit what
        // the casting layers left, then the nearest upgraded to the mesh
        // (VegetationPolicy::ChooseReflectionTiers). The rest are left out of
        // the scene and counted, not refused, so the TLAS stays (#1533).
        OLO_PERF_SCOPE_AUTO("Vegetation::QueueAdmit");
        if (!splitReused)
        {
            std::ranges::sort(reflectionCandidates, [](const ReflectionCandidate& a, const ReflectionCandidate& b)
                              { return std::make_pair(a.Distance, a.FirstId) < std::make_pair(b.Distance, b.FirstId); });
            m_ReflectionSplit.Valid = reflectionsReadVegetation;
            m_ReflectionSplit.Camera = cameraPosition;
            m_ReflectionSplit.Terrain = m_TerrainTransform;
            m_ReflectionSplit.RecordsEpoch = m_Registry.GetRecordsEpoch();
            m_ReflectionSplit.LayersKey = layersKey;
        }
        using RayTracing::VegetationPolicy;
        TArray<VegetationPolicy::ReflectionGroupCost> costs;
        TArray<VegetationPolicy::ReflectionTier> tiers;
        costs.Reserve(reflectionCandidates.Num());
        for (const ReflectionCandidate& candidate : reflectionCandidates)
        {
            costs.Add(candidate.Cost);
            tiers.Add(VegetationPolicy::ReflectionTier::Out);
        }
        // The stress lever takes the same share of the reflection room.
        const u32 divisor = RayTracing::VegetationDiagnostics::GetBudgetDivisor();
        const auto withheld = [divisor](u64 limit) -> u64
        { return limit - limit / divisor; };
        VegetationPolicy::ChooseReflectionTiers(std::span<const VegetationPolicy::ReflectionGroupCost>{ costs.GetData(), static_cast<sizet>(costs.Num()) },
                                                cache.GetStagedBytes() + withheld(VegetationPolicy::GeometryBytes),
                                                cache.GetStagedAccelerationBytes() + withheld(VegetationPolicy::AccelerationStructureBytes),
                                                std::span<VegetationPolicy::ReflectionTier>{ tiers.GetData(), static_cast<sizet>(tiers.Num()) },
                                                cache.GetQueuedGroups() + static_cast<u32>(withheld(VegetationPolicy::ResidentGroups)));
        f32 reach = 0.0f;
        f32 detailReach = 0.0f;
        u32 groupsLeftOut = 0u;
        u32 plantsLeftOut = 0u;
        for (i32 i = 0; i < reflectionCandidates.Num(); ++i)
        {
            const ReflectionCandidate& candidate = reflectionCandidates[i];
            const std::span<const FoliageInstanceRecord* const> records{ reflectionRecords.GetData() + candidate.FirstRecord,
                                                                         candidate.RecordCount };
            const auto& layer = m_Layers[candidate.LayerIndex];
            switch (tiers[i])
            {
                case VegetationPolicy::ReflectionTier::Mesh:
                    queueGroup(layer, candidate.MeshRepresentation, records, candidate.Cost.MeshGeometryBytes,
                               candidate.Cost.MeshAccelerationBytes, candidate.Distance);
                    reach = std::max(reach, candidate.Distance);
                    detailReach = std::max(detailReach, candidate.Distance);
                    break;
                case VegetationPolicy::ReflectionTier::Card:
                    queueGroup(layer, 2u, records, candidate.Cost.CardGeometryBytes, candidate.Cost.CardAccelerationBytes,
                               candidate.Distance);
                    reach = std::max(reach, candidate.Distance);
                    break;
                case VegetationPolicy::ReflectionTier::Out:
                    ++groupsLeftOut;
                    plantsLeftOut += candidate.RecordCount;
                    break;
            }
        }
        if (!reflectionCandidates.IsEmpty())
            cache.CountBeyondReflectionBudget(groupsLeftOut, plantsLeftOut, reach, detailReach);
    }

    FoliageRenderer::~FoliageRenderer()
    {
        m_StreamingMemoryReporter.Reset();
        m_StreamingLoads.Shutdown();
        for (auto& [index, state] : m_StreamingLayers)
            DropStreamingLayer(state);
        RepresentationStreaming::Get().Release(m_StreamingPinnedKey);
        if (m_StreamingStats.CanonicalCpuBytes)
            OLO_TRACK_DEALLOC(&m_Registry);
        if (m_StreamingStats.PreparedCpuBytes)
            OLO_TRACK_DEALLOC(&m_StreamingLoads);
        for (auto& layer : m_Layers)
            ImpostorBaker::Free(layer.Impostor);
    }

    void FoliageRenderer::BuildQuadGeometry(LayerRenderData& data) const
    {
        // Billboard quad: 4 vertices, centered at bottom.
        // Positions in local space, billboard rotation handled in shader.
        //
        // The layout is the ENGINE's Vertex (position, normal, texcoord) rather
        // than the old 20-byte {position, texcoord}: since #1233 the same
        // vertex stage draws this card AND an authored plant mesh, and one
        // stream layout for both is what keeps the beauty, G-Buffer and shadow
        // programs from each needing a card variant and a mesh variant to drift
        // apart. The card's normal is +Y — exactly the constant the vertex
        // stage used to hard-code — so the card renders bit-identically.
        const Vertex quadVertices[] = {
            { { -0.5f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } }, // bottom-left
            { { 0.5f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },  // bottom-right
            { { 0.5f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },  // top-right
            { { -0.5f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } }, // top-left
        };

        u32 indices[] = { 0, 1, 2, 2, 3, 0 };

        data.QuadVBO = VertexBuffer::Create(quadVertices, static_cast<u32>(sizeof(quadVertices)));
        data.QuadVBO->SetLayout(Vertex::GetLayout());

        data.IBO = IndexBuffer::Create(indices, 6);
        data.IndexCount = 6;
    }

    namespace
    {
        // A plant mesh as FoliageRenderer draws it: ONE vertex/index buffer,
        // each submesh a plain [BaseIndex, IndexCount) range with its own
        // albedo, plus the surfaces the alpha-coverage diagnostic samples and
        // the real bounds. Extracted ONCE, here, for both consumers — the near
        // mesh (BuildMeshGeometry) and the impostor bake (UpdateImpostorAtlas)
        // — because the impostor has to be made of exactly the parts and
        // materials of the mesh it replaces. Before #1399 the bake drew
        // Model::GetMesh(0)'s vertex array with the layer's BILLBOARD texture:
        // on a cold import that vertex array is the trunk alone, and the
        // billboard painted over the pine's atlas UVs passed 12% of its surface.
        struct PlantGeometry
        {
            Ref<MeshSource> Source; // owns the vertices
            TArray<u32> Indices;
            TArray<FoliageLayerDrawPart> Parts;
            TArray<TArray<glm::vec2>> PartSurfaceUVs;
            BoundingBox Box;
        };

        // nullptr on success, otherwise why the model yields nothing drawable.
        [[nodiscard]] const char* ExtractPlantGeometry(const Model& model, PlantGeometry& out)
        {
            OLO_PROFILE_FUNCTION();

            // The combined source's indices are already GLOBAL:
            // Submesh::m_BaseVertex describes the submesh's vertex RANGE, it is
            // not an offset still to be applied (AssimpMeshExporter and
            // MeshCookingFactory subtract it to get back to local indices).
            // Adding it again pushed every submesh past the first onto the
            // wrong vertices or past the end of the buffer — a shipped pine drew
            // its bark on shifted triangles and its canopy out of range (found
            // by the #1399 coverage measurement, which sampled those triangles
            // and got nothing back).
            out.Source = model.CreateCombinedMeshSource();
            if (!out.Source || out.Source->GetVertices().Num() == 0 || out.Source->GetIndices().Num() == 0)
                return "it loaded but carries no geometry";

            const auto& srcVertices = out.Source->GetVertices();
            const auto& srcIndices = out.Source->GetIndices();
            const auto& submeshes = out.Source->GetSubmeshes();
            out.Indices.Reserve(srcIndices.Num());

            const sizet submeshCount = submeshes.Num() > 0 ? static_cast<sizet>(submeshes.Num()) : 1;
            for (sizet i = 0; i < submeshCount; ++i)
            {
                FoliageLayerDrawPart part;
                part.BaseIndex = static_cast<u32>(out.Indices.Num());

                if (submeshes.Num() > 0)
                {
                    const auto& sub = submeshes[static_cast<i32>(i)];
                    for (u32 k = 0; k < sub.m_IndexCount; ++k)
                    {
                        const u32 srcSlot = sub.m_BaseIndex + k;
                        if (srcSlot >= static_cast<u32>(srcIndices.Num()))
                            break;
                        out.Indices.Add(srcIndices[static_cast<i32>(srcSlot)]);
                    }
                    part.IndexCount = static_cast<u32>(out.Indices.Num()) - part.BaseIndex;
                    // Per-submesh material assignment, through the submesh's OWN
                    // material index — NOT the loop index.
                    //
                    // Model::m_Materials holds one entry per UNIQUE aiMaterial
                    // (ProcessMesh dedups through m_MaterialIndexMap), while
                    // CreateCombinedMeshSource emits one submesh per mesh. Those
                    // two counts only coincide when every submesh has a distinct
                    // material, so indexing by submesh ordinal silently picks
                    // the wrong material the moment a plant reuses one — e.g. a
                    // tree whose trunk and branches share bark. Model.cpp
                    // carries the same warning from #629, where rebuilding the
                    // array per-mesh made warm and cold loads resolve different
                    // materials.
                    //
                    // UINT32_MAX is the "no material resolved" sentinel
                    // (Model.cpp sets it when the lookup misses); the bounds
                    // check covers it the same way every other consumer in
                    // Model.cpp does.
                    if (sub.m_MaterialIndex < static_cast<u32>(model.GetMaterialCount()))
                    {
                        if (const Ref<Material>& material = model.GetMaterial(sub.m_MaterialIndex); material)
                        {
                            part.Albedo = material->GetAlbedoMap();
                        }
                    }
                }
                else
                {
                    for (i32 k = 0; k < srcIndices.Num(); ++k)
                        out.Indices.Add(srcIndices[k]);
                    part.IndexCount = static_cast<u32>(out.Indices.Num());
                }

                if (part.IndexCount > 0)
                    out.Parts.Add(std::move(part));
            }

            if (out.Parts.IsEmpty() || out.Indices.IsEmpty())
                return "it produced no drawable submesh range";

            // The surfaces the alpha-coverage diagnostic measures (issue #1399),
            // one per part, sampled here because this is where the CPU vertices
            // are in hand.
            const std::span<const Vertex> vertexSpan(srcVertices.GetData(), static_cast<sizet>(srcVertices.Num()));
            for (const auto& part : out.Parts)
            {
                out.PartSurfaceUVs.Add(FoliageAlphaCoverage::SampleSurfaceUVs(
                    vertexSpan, std::span<const u32>(out.Indices.GetData() + part.BaseIndex, part.IndexCount)));
            }

            // Measured from the vertices, not from MeshSource::GetBoundingBox():
            // that field is populated on a fresh assimp import and comes back
            // empty on the warm .omesh cache path, so reading it made the bound
            // — and the impostor's framing — depend on whether the mesh had been
            // imported before in this process. A foliage mesh is small, so
            // measuring is cheaper than trusting.
            // Braces, not parentheses: `BoundingBox box(glm::vec3(a), glm::vec3(b))`
            // is a function declaration, not a variable (most vexing parse).
            out.Box = BoundingBox{ glm::vec3(std::numeric_limits<f32>::max()),
                                   glm::vec3(std::numeric_limits<f32>::lowest()) };
            for (i32 i = 0; i < srcVertices.Num(); ++i)
            {
                const glm::vec3& position = srcVertices[i].Position;
                out.Box.Min = glm::min(out.Box.Min, position);
                out.Box.Max = glm::max(out.Box.Max, position);
            }
            return nullptr;
        }

        // The elevation of a plant's mean FRONT-FACING normal (#1533): its face
        // normals flipped toward +Z, the card bake's eye (bake_billboard projects
        // along -Z), area-weighted, averaged and normalised. The far card faces
        // the eye and takes this as its normal's rise, so it is lit the way the
        // near plant is on average rather than as an upward-facing sheet.
        [[nodiscard]] f32 MeanFrontFacingNormalElevation(const TArray<Vertex>& vertices, const TArray<u32>& indices)
        {
            glm::dvec3 sum(0.0);
            for (i32 i = 0; i + 2 < indices.Num(); i += 3)
            {
                const u32 a = indices[i];
                const u32 b = indices[i + 1];
                const u32 c = indices[i + 2];
                if (a >= static_cast<u32>(vertices.Num()) || b >= static_cast<u32>(vertices.Num()) ||
                    c >= static_cast<u32>(vertices.Num()))
                    continue;
                const glm::dvec3 pa(vertices[static_cast<i32>(a)].Position);
                // The cross product's length is twice the face's area: the weight.
                glm::dvec3 face = glm::cross(glm::dvec3(vertices[static_cast<i32>(b)].Position) - pa,
                                             glm::dvec3(vertices[static_cast<i32>(c)].Position) - pa);
                if (face.z < 0.0)
                    face = -face;
                sum += face;
            }
            const f64 length = glm::length(sum);
            if (!(length > 0.0) || !std::isfinite(length))
                return 0.0f;
            return static_cast<f32>(std::clamp(sum.y / length, -0.95, 0.95));
        }
    } // namespace

    bool FoliageRenderer::BuildMeshGeometry(LayerRenderData& data, const FoliageLayer& layer) const
    {
        OLO_PROFILE_FUNCTION();
        // Eager authoring uploads use the same owner as streamed detail. This
        // preserves physical retiring accounting when streaming is enabled live.
        RendererMemoryOwnerScope detailOwner("Foliage streaming detail", MemoryLifetime::Asset);

        data.MeshVAO = nullptr;
        data.MeshVBO = nullptr;
        data.MeshIBO = nullptr;
        data.MeshParts.Reset();
        data.MeshPartSurfaceUVs.Reset();
        data.AlphaCoverageDirty = true;
        data.MeshRayTracingIndices.Reset();
        data.MeshModel = nullptr;
        data.MeshVertexCount = 0;
        data.MeshIndexCount = 0;
        data.MeshBounds = BoundingBox{};
        data.MeshGeometryPath.Empty();
        data.BoundsProfile = FoliageBoundsProfile{};
        data.CardNormalTilt = 0.0f;
        data.CardUsesMeshBake = false;

        // Project-relative for project content, working-directory-relative for
        // engine content (#1496). An unresolvable path is logged by the resolver
        // and takes the same did-not-load branch as a file Assimp rejects.
        const std::filesystem::path meshFile = ResolveContentPath(layer.MeshPath.ToView());
        Ref<Model> model = meshFile.empty() ? Ref<Model>{} : Ref<Model>::Create(meshFile.generic_string());
        if (!model || model->GetMeshCount() == 0)
        {
            OLO_CORE_ERROR("FoliageRenderer: layer '{}' asks for the authored mesh '{}' and it did not load. The "
                           "layer draws its flat card at ALL distances instead; the census counts the variant as "
                           "unavailable. Fix the path or clear UseAuthoredMesh.",
                           layer.Name.ToView(), layer.MeshPath.ToView());
            return false;
        }

        // ONE private vertex/index buffer for the whole plant, each submesh a
        // plain [BaseIndex, IndexCount) range of it, so the per-submesh draws
        // differ only in a first-index offset and a texture — no base-vertex
        // plumbing through the command packet, and one vertex array.
        PlantGeometry plant;
        if (const char* failure = ExtractPlantGeometry(*model, plant))
        {
            OLO_CORE_ERROR("FoliageRenderer: layer '{}' mesh '{}': {}. Drawing the flat card instead.",
                           layer.Name.ToView(), layer.MeshPath.ToView(), failure);
            return false;
        }

        const auto& srcVertices = plant.Source->GetVertices();
        data.MeshParts = std::move(plant.Parts);
        data.MeshPartSurfaceUVs = std::move(plant.PartSurfaceUVs);
        data.MeshVBO = VertexBuffer::Create(srcVertices.GetData(),
                                            static_cast<u32>(srcVertices.Num() * sizeof(Vertex)));
        data.MeshVBO->SetLayout(Vertex::GetLayout());
        data.MeshIBO = IndexBuffer::Create(plant.Indices.GetData(), static_cast<u32>(plant.Indices.Num()));
        data.MeshRayTracingIndices = plant.Indices;
        data.MeshVertexCount = static_cast<u32>(srcVertices.Num());
        data.MeshIndexCount = static_cast<u32>(plant.Indices.Num());
        data.MeshBounds = plant.Box;
        data.MeshModel = model;
        data.MeshGeometryPath = layer.MeshPath;
        data.CardNormalTilt = MeanFrontFacingNormalElevation(srcVertices, plant.Indices);
        data.CardUsesMeshBake = std::ranges::all_of(data.MeshParts, [](const auto& part)
                                                    { return static_cast<bool>(part.Albedo); });

        // Conservative bounds from the REAL geometry (issue #1233, second
        // criterion). A quad's box is not a pine's: the canopy is wider than
        // 0.5 and the trunk can start below the origin, and culling against the
        // quad's box pops the tree out at the screen edge. The horizontal
        // half-extent is the largest XZ radius of the source AABB's corners, so
        // it holds for ANY of the per-instance Y rotations.
        const BoundingBox& box = plant.Box;

        const f32 radiusXZ = std::max(std::max(std::abs(box.Min.x), std::abs(box.Max.x)),
                                      std::max(std::abs(box.Min.z), std::abs(box.Max.z)));
        // The profile keeps the CARD terms as well, because a layer with a mesh
        // still draws its card past MeshViewDistance — the bound has to hold
        // for both shapes, not just the near one.
        data.BoundsProfile = FoliageBoundsProfile{};
        data.BoundsProfile.m_HalfExtentXZHeightScaled = std::max(radiusXZ * glm::root_two<f32>(), 1e-3f);
        data.BoundsProfile.m_MinY = std::min(box.Min.y, 0.0f);
        data.BoundsProfile.m_MaxY = std::max(box.Max.y, 1.0f);

        // The authoring convention both this path and the impostor bake assume:
        // base at the origin, unit height. Neither rescales — a mesh authored at
        // some other size is drawn at the wrong size in BOTH, consistently — so
        // say so rather than let the author discover it as "my tree is tiny".
        constexpr f32 kUnitTolerance = 0.05f;
        if (std::abs(box.Max.y - 1.0f) > kUnitTolerance || std::abs(box.Min.y) > kUnitTolerance)
        {
            OLO_CORE_WARN("FoliageRenderer: layer '{}' mesh '{}' spans y in [{:.3f}, {:.3f}], not the base-at-origin "
                          "unit height ([0, 1]) foliage authoring assumes. It is scaled by the instance's "
                          "height * scale as-is, so every plant is drawn {:.2f}x the authored height — near mesh and "
                          "far impostor alike. Re-author the mesh or compensate with MinHeight/MaxHeight.",
                          layer.Name.ToView(), layer.MeshPath.ToView(), box.Min.y, box.Max.y,
                          std::max(box.Max.y - std::min(box.Min.y, 0.0f), 1e-3f));
        }

        OLO_CORE_INFO("FoliageRenderer: layer '{}' authored mesh '{}' ready — {} vertices, {} indices, {} submesh(es), "
                      "{:.1f} KiB geometry",
                      layer.Name.ToView(), layer.MeshPath.ToView(), data.MeshVertexCount, data.MeshIndexCount, data.MeshParts.Num(),
                      static_cast<f32>(data.MeshVertexCount * sizeof(Vertex) + data.MeshIndexCount * sizeof(u32)) / 1024.0f);
        return true;
    }

    namespace
    {
        std::atomic<u64> s_FoliageStreamingKey{ 0xf100000000000001ull };

        [[nodiscard]] u64 FoliageTextureBytes(const Ref<Texture2D>& texture)
        {
            if (!texture)
                return 0u;
            const auto& spec = texture->GetSpecification();
            return RendererMemoryFormat::ImageBytes(spec.Format, spec.Width, spec.Height, texture->GetMipLevelCount(), 1u, spec.Samples).value_or(0u);
        }

        [[nodiscard]] u64 FoliageDetailPhysicalLiveBytes()
        {
            const auto report = RendererMemoryTracker::GetInstance().BuildReport();
            for (const auto& owner : report.Owners)
                if (owner.Owner.ToView() == "Foliage streaming detail")
                    return owner.GpuLiveBytes;
            return 0u;
        }

    } // namespace

    FoliageRenderer::FoliageRenderer()
        : m_StreamingPinnedKey(s_FoliageStreamingKey.fetch_add(1u, std::memory_order_relaxed)),
          m_StreamingMemoryReporter([this](TArray<MemoryCapacityRow>& rows)
                                    {
                                       const auto append = [&](const char* category, u64 bytes, bool gpu)
                                       {
                                           MemoryCapacityRow row;
                                           row.Owner = "FoliageRenderer";
                                           row.Category = category;
                                           row.Lifetime = MemoryLifetime::Asset;
                                           row.IsGpu = gpu;
                                           row.CapacityBytes = bytes;
                                           row.ActiveDemandBytes = bytes;
                                           rows.Add(std::move(row));
                                       };
                                       append("Pinned card, material, atlas and instance floor", m_StreamingStats.PinnedGpuBytes, true);
                                       append("Optional authored plant representations", m_StreamingStats.OptionalGpuBytes, true);
                                       append("Canonical instance and spatial-group arrays (CPU)", m_StreamingStats.CanonicalCpuBytes, false);
                                       append("Prepared optional representation payloads (CPU)", m_StreamingStats.PreparedCpuBytes, false);
                                       u64 metadataBytes = 0;
                                       for (const auto& layer : m_Layers)
                                       {
                                           metadataBytes += layer.MeshRayTracingIndices.GetAllocatedSize() + layer.MeshParts.GetAllocatedSize() +
                                                            layer.MeshPartSurfaceUVs.GetAllocatedSize() + layer.ImpostorPartSurfaceUVs.GetAllocatedSize() +
                                                            layer.ImpostorPartTextures.GetAllocatedSize() + layer.ImpostorPartCoverage.GetAllocatedSize();
                                           for (const auto& part : layer.MeshParts)
                                               metadataBytes += part.AlbedoSourcePath.GetAllocatedSize();
                                           for (const auto& uvs : layer.MeshPartSurfaceUVs)
                                               metadataBytes += uvs.GetAllocatedSize();
                                           for (const auto& uvs : layer.ImpostorPartSurfaceUVs)
                                               metadataBytes += uvs.GetAllocatedSize();
                                           for (const auto& path : layer.ImpostorPartTextures)
                                               metadataBytes += path.GetAllocatedSize();
                                       }
                                       append("Mesh and pinned impostor index, surface and coverage metadata (CPU)", metadataBytes, false); })
    {
        m_StreamingLoads.SetStagingBudget(64u * 1024u * 1024u);
    }

    void FoliageRenderer::DropStreamingLayer(FStreamingLayer& state)
    {
        (void)m_StreamingLoads.Cancel(state.Key);
        m_StreamingLoads.ClearFailure(state.Key);
        state.Ready.Reset();
        if (state.StagingTicket)
            m_StreamingLoads.ReleaseStaging(state.StagingTicket);
        RepresentationStreaming::Get().Release(state.Key);
        state.StagingTicket = 0;
    }

    void FoliageRenderer::EvictStreamingMesh(LayerRenderData& data)
    {
        // Vertex arrays retain their streams. Drop EVERY view's alias before
        // releasing backing, which the resource classes retire on their queues.
        data.MeshVAO.Reset();
        for (auto& view : data.CullViews)
        {
            view.MeshVAO.Reset();
            view.Active = false;
        }
        data.MeshVBO.Reset();
        data.MeshIBO.Reset();
        data.MeshModel.Reset();
        data.MeshParts.Empty();
        data.MeshRayTracingIndices.Empty();
        data.MeshPartSurfaceUVs.Empty();
        data.MeshVertexCount = 0;
        data.MeshIndexCount = 0;
        data.MeshBounds = BoundingBox{};
        data.AlphaCoverageDirty = true;
        m_ReflectionSplit.Valid = false;
        m_MainViewCulled = false;
        // BoundsProfile, CardNormalTilt and CardUsesMeshBake describe the pinned
        // card too and intentionally survive. Identity and instances survive.
    }

    void FoliageRenderer::SetStreamingEnabled(bool enabled)
    {
        if (m_StreamingEnabled == enabled)
            return;
        m_StreamingEnabled = enabled;
        m_StreamingModeChanged = true;
        if (enabled)
        {
            // The private raster streams already own the drawable geometry and
            // MeshParts owns its textures. Drop the eager Model's duplicate GPU
            // geometry before adopting detail into bounded residency.
            auto& tracker = RendererMemoryTracker::GetInstance();
            const auto retagBuffer = [&tracker](const auto& buffer)
            {
                if (buffer)
                    (void)tracker.ReattributeBackingResource(RHI::HashKey(buffer->GetRHIHandle()), "Foliage streaming detail", MemoryLifetime::Asset);
            };
            const auto retagVertexArrayStreams = [&retagBuffer](const Ref<VertexArray>& vao)
            {
                if (!vao)
                    return;
                // Includes the bone, lightmap UV and constant UV stub streams.
                for (const auto& buffer : vao->GetVertexBuffers())
                    retagBuffer(buffer);
                retagBuffer(vao->GetIndexBuffer());
            };
            for (auto& layer : m_Layers)
            {
                // These streams and material textures survive Model.Reset and
                // are adopted below. Their later retirement belongs to detail
                // too, including a refused or distant adoption.
                retagBuffer(layer.MeshVBO);
                retagBuffer(layer.MeshIBO);
                for (const auto& part : layer.MeshParts)
                    retagBuffer(part.Albedo);
                if (layer.MeshModel)
                {
                    for (const auto& mesh : layer.MeshModel->GetMeshes())
                    {
                        if (!mesh)
                            continue;
                        const auto source = mesh->GetMeshSource();
                        if (!source)
                            continue;
                        // MeshSource stamps its own eager owner. Transfer only
                        // this model's exact backing before Reset retires it;
                        // repeated handles are harmless, and aliases do not move.
                        if (source->HasVertexBuffer())
                            retagBuffer(source->GetVertexBuffer());
                        if (source->IsBuilt() && source->HasVertexBuffer())
                            retagBuffer(source->GetIndexBuffer());
                        if (source->HasBoneInfluenceBuffer())
                            retagBuffer(source->GetBoneInfluenceBuffer());
                        retagVertexArrayStreams(source->GetVertexArray());
                        retagVertexArrayStreams(source->GetShadowVertexArray());
                    }
                }
                layer.MeshModel.Reset();
            }
        }
        if (!enabled)
        {
            for (auto& [index, state] : m_StreamingLayers)
                DropStreamingLayer(state);
            m_StreamingLayers.clear();
            RepresentationStreaming::Get().Release(m_StreamingPinnedKey);
            // Restore the synchronous authoring mode on the next reconcile.
            for (auto& layer : m_Layers)
                if (layer.MeshRequested && !layer.MeshVBO)
                    layer.MeshGeometryPath.Empty();
        }
    }

    bool FoliageRenderer::UploadStreamingMesh(LayerRenderData& data, FStreamingLayer& state, const FoliageLayer& layer)
    {
        if (!state.Ready)
            return false;
        auto payloadOwner = state.Ready.As<FFoliageStreamingPayload>();
        const auto& payload = payloadOwner;
        u64 textureWorkingBytes = 0;
        for (const auto& part : payload->Parts)
            textureWorkingBytes = std::max(textureWorkingBytes, part.AlbedoDecodeBytes);
        const auto textureStaging = textureWorkingBytes ? FFoliageTextureStaging::Reserve(textureWorkingBytes) : Ref<FFoliageTextureStaging>{};
        if (textureWorkingBytes && !textureStaging)
            return false; // no GPU allocation or decode precedes this admission
        FRepresentationDescriptor descriptor = state.Descriptor;
        descriptor.CpuBytes = FAssetByteSize::Actual(payload->GetCpuBytes());
        descriptor.GpuBytes = std::max(payload->GetGpuBytes(), state.Descriptor.GpuBytes);
        descriptor.UploadBytes = payload->GetUploadBytes(layer.AlphaCutoff);
        if (!RepresentationStreaming::Get().TryAdmit(state.Key, descriptor))
            return false;
        const u64 detailBefore = FoliageDetailPhysicalLiveBytes();
        const auto uploadStart = std::chrono::steady_clock::now();
        RendererMemoryOwnerScope detailOwner("Foliage streaming detail", MemoryLifetime::Asset);
        data.MeshVBO = VertexBuffer::Create(payload->Vertices.GetData(), static_cast<u32>(payload->Vertices.Num() * sizeof(Vertex)));
        data.MeshVBO->SetLayout(Vertex::GetLayout());
        // The legacy upload API takes mutable storage, but both backends copy
        // the source without modifying it. Keep the prepared payload immutable.
        data.MeshIBO = IndexBuffer::Create(const_cast<u32*>(payload->Indices.GetData()), static_cast<u32>(payload->Indices.Num()));
        data.MeshVertexCount = static_cast<u32>(payload->Vertices.Num());
        data.MeshIndexCount = static_cast<u32>(payload->Indices.Num());
        data.MeshRayTracingIndices = payload->Indices;
        data.MeshParts.Empty();
        data.MeshPartSurfaceUVs.Empty();
        u64 uploadedBytes = static_cast<u64>(payload->Vertices.Num()) * sizeof(Vertex) + static_cast<u64>(payload->Indices.Num()) * sizeof(u32);
        for (const auto& source : payload->Parts)
        {
            FoliageLayerDrawPart part;
            part.BaseIndex = source.BaseIndex;
            part.IndexCount = source.IndexCount;
            auto surfaceUVs = FoliageAlphaCoverage::SampleSurfaceUVs(
                { payload->Vertices.GetData(), static_cast<sizet>(payload->Vertices.Num()) },
                { payload->Indices.GetData() + source.BaseIndex, source.IndexCount });
            if (!source.AlbedoPath.IsEmpty())
            {
                const auto image = DecodeFoliageStreamingAlbedo(source, textureStaging);
                if (!image)
                {
                    EvictStreamingMesh(data);
                    RepresentationStreaming::Get().Release(state.Key);
                    state.UploadFailed = true;
                    state.Ready.Reset();
                    // payload remains alive through this function; release its
                    // reservation only after dropping our local immutable view.
                    m_StreamingStats.UploadedBytes += uploadedBytes;
                    RepresentationStreaming::Get().RecordUpload(uploadedBytes, static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                                                    std::chrono::steady_clock::now() - uploadStart)
                                                                                                    .count()));
                    return false;
                }
                TextureSpecification spec;
                spec.Width = image->Width;
                spec.Height = image->Height;
                spec.Format = ImageFormat::RGBA8;
                spec.SRGB = true;
                part.Albedo = Texture2D::Create(spec);
                // SetData copies to backend staging; it never writes source bytes.
                part.Albedo->SetData(const_cast<u8*>(image->Pixels.GetData()), static_cast<u32>(image->Pixels.Num()));
                // Configure coverage mips while the decode/upload working-set
                // lease is held; the later reconcile sees an unchanged cutoff.
                part.Albedo->SetAlphaCoverageCutoff(layer.AlphaCutoff);
                part.AlbedoSourcePath = source.AlbedoPath;
                for (const auto& uv : surfaceUVs)
                    part.SampledAlphaCoverage.Add(image->AlphaAt(uv));
                uploadedBytes += source.AlbedoUploadBytes;
                if (AlphaCoverageMips::SanitizeCutoff(layer.AlphaCutoff) > 0.0f)
                    uploadedBytes += FoliageTextureBytes(part.Albedo); // second base plus CPU-built effective mips
            }
            data.MeshParts.Add(std::move(part));
            data.MeshPartSurfaceUVs.Add(std::move(surfaceUVs));
        }
        data.CardUsesMeshBake = std::ranges::all_of(data.MeshParts, [](const auto& part)
                                                    { return part.Albedo && part.Albedo->IsLoaded(); });
        data.CardNormalTilt = MeanFrontFacingNormalElevation(payload->Vertices, payload->Indices);
        const auto& box = payload->Bounds;
        data.MeshBounds = box;
        const f32 radius = std::max({ std::abs(box.Min.x), std::abs(box.Max.x), std::abs(box.Min.z), std::abs(box.Max.z) });
        data.BoundsProfile.m_HalfExtentXZHeightScaled = std::max(data.BoundsProfile.m_HalfExtentXZHeightScaled, radius * glm::root_two<f32>());
        data.BoundsProfile.m_MinY = std::min(data.BoundsProfile.m_MinY, box.Min.y);
        data.BoundsProfile.m_MaxY = std::max(data.BoundsProfile.m_MaxY, box.Max.y);
        data.AlphaCoverageDirty = true;
        // Bake the pinned atlas from this SAME held preparation and geometry.
        if (layer.UseImpostor)
        {
            RendererMemoryOwnerScope floorOwner("Foliage pinned representations", MemoryLifetime::Asset);
            UpdateImpostorAtlas(data, layer);
        }
        RebuildVertexArrays(data);
        const u64 detailAfter = FoliageDetailPhysicalLiveBytes();
        descriptor.GpuBytes = detailAfter >= detailBefore ? detailAfter - detailBefore : descriptor.GpuBytes;
        auto& budget = RepresentationStreaming::Get();
        budget.Release(state.Key);
        FRepresentationDescriptor physical = descriptor;
        physical.UploadBytes = 0; // the upload has already spent this frame's allowance
        if (!budget.TryAdmit(state.Key, physical))
        {
            state.Descriptor = descriptor; // remember the measured requirement for future admission
            EvictStreamingMesh(data);
            ++m_StreamingStats.Evictions;
            m_StreamingStats.UploadedBytes += uploadedBytes;
            budget.RecordUpload(uploadedBytes, static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                    std::chrono::steady_clock::now() - uploadStart)
                                                                    .count()));
            return false;
        }
        if (state.WasResident)
            ++m_StreamingStats.Reloads;
        state.WasResident = true;
        state.Descriptor = descriptor;
        m_StreamingStats.UploadedBytes += uploadedBytes;
        RepresentationStreaming::Get().RecordUpload(uploadedBytes, static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                                        std::chrono::steady_clock::now() - uploadStart)
                                                                                        .count()));
        state.Ready.Reset();
        payloadOwner.Reset(); // release our final consumer alias before returning its charge
        m_StreamingLoads.ReleaseStaging(state.StagingTicket);
        state.StagingTicket = 0;
        m_WindHistory.Reset();
        m_ReflectionSplit.Valid = false;
        return true;
    }

    bool FoliageRenderer::UpdateStreamingAlphaCutoffs(LayerRenderData& data, FStreamingLayer& state, const FoliageLayer& layer)
    {
        auto& budget = RepresentationStreaming::Get();
        const f32 cutoff = AlphaCoverageMips::SanitizeCutoff(layer.AlphaCutoff);
        bool changed = false;
        for (const auto& part : data.MeshParts)
        {
            Ref<Texture2D> texture = part.Albedo;
            if (!texture || Math::BitwiseEqual(texture->GetAlphaCoverageCutoff(), cutoff))
                continue;
            const auto& spec = texture->GetSpecification();
            const u64 baseBytes = static_cast<u64>(spec.Width) * spec.Height * 4u;
            const auto staging = FFoliageTextureStaging::Reserve(baseBytes * 8u + 65536u);
            if (!staging)
                continue;
            // The old image remains charged in this key. Admit replacement
            // backing and conservative transfer before readback/recreation.
            FRepresentationDescriptor transient = state.Descriptor;
            const u64 replacementBytes = baseBytes * 4u + 8192u;
            if (transient.GpuBytes > std::numeric_limits<u64>::max() - replacementBytes)
                continue;
            transient.GpuBytes += replacementBytes;
            transient.UploadBytes = cutoff > 0.0f ? baseBytes * 3u : baseBytes;
            if (!budget.TryAdmit(state.Key, transient))
                continue;
            const u64 before = FoliageDetailPhysicalLiveBytes();
            const auto start = std::chrono::steady_clock::now();
            RendererMemoryOwnerScope owner("Foliage streaming detail", MemoryLifetime::Asset);
            const f32 previousCutoff = texture->GetAlphaCoverageCutoff();
            texture->SetAlphaCoverageCutoff(cutoff);
            changed |= !Math::BitwiseEqual(previousCutoff, texture->GetAlphaCoverageCutoff());
            const u64 transferred = cutoff > 0.0f ? FoliageTextureBytes(texture) : baseBytes;
            m_StreamingStats.UploadedBytes += transferred;
            budget.RecordUpload(transferred, static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()));
            const u64 after = FoliageDetailPhysicalLiveBytes();
            const u64 resident = after >= before ? state.Descriptor.GpuBytes + after - before : state.Descriptor.GpuBytes - std::min(state.Descriptor.GpuBytes, before - after);
            budget.Release(state.Key);
            FRepresentationDescriptor physical = state.Descriptor;
            physical.GpuBytes = resident;
            physical.UploadBytes = 0;
            if (!budget.TryAdmit(state.Key, physical))
            {
                EvictStreamingMesh(data);
                ++m_StreamingStats.Evictions;
                m_WindHistory.Reset();
                return true;
            }
            state.Descriptor.GpuBytes = resident;
        }
        return changed;
    }

    bool FoliageRenderer::UpdateStreamingResidency(const TArray<FoliageLayer>& layers, const glm::vec3& cameraPosition)
    {
        bool changed = std::exchange(m_StreamingModeChanged, false);
        m_StreamingLoads.ReapAbandoned();
        TArray<FCompletedRepresentationLoad> completed;
        m_StreamingLoads.RetrieveCompleted(completed);
        for (auto& load : completed)
        {
            auto found = std::ranges::find_if(m_StreamingLayers, [&](const auto& pair)
                                              { return pair.second.Key == load.Key; });
            if (found == m_StreamingLayers.end() || !load.Payload)
            {
                load.Payload.Reset();
                m_StreamingLoads.ReleaseStaging(load.StagingTicket);
                continue;
            }
            found->second.Ready = std::move(load.Payload);
            found->second.StagingTicket = load.StagingTicket;
        }
        if (!m_StreamingEnabled)
        {
            RefreshStreamingStats();
            return changed;
        }
        bool inactiveDetailEvicted = false;
        for (u32 i = 0; i < static_cast<u32>(m_Layers.Num()); ++i)
        {
            auto& data = m_Layers[static_cast<i32>(i)];
            const bool active = i < static_cast<u32>(layers.Num()) && layers[static_cast<i32>(i)].Enabled &&
                                layers[static_cast<i32>(i)].UseAuthoredMesh && !layers[static_cast<i32>(i)].MeshPath.IsEmpty() &&
                                data.InstanceCount > 0;
            if (active)
                continue;
            const bool hasPrivateDetail = data.MeshVAO || data.MeshVBO || data.MeshIBO || data.MeshModel || !data.MeshParts.IsEmpty() ||
                                          std::ranges::any_of(data.CullViews, [](const auto& view)
                                                              { return static_cast<bool>(view.MeshVAO); });
            if (!hasPrivateDetail)
                continue;
            // Eager generation builds detail before placement, so a layer
            // rejected by its habitat can own GPU resources without ever
            // receiving a streaming state. Sweep the actual private holders
            // too; state-only pruning would leave that detail unbudgeted.
            EvictStreamingMesh(data);
            ++m_StreamingStats.Evictions;
            inactiveDetailEvicted = true;
        }
        if (inactiveDetailEvicted)
        {
            m_WindHistory.Reset();
            changed = true;
        }
        for (auto it = m_StreamingLayers.begin(); it != m_StreamingLayers.end();)
        {
            if (it->first >= static_cast<u32>(layers.Num()) || it->first >= static_cast<u32>(m_Layers.Num()) ||
                !layers[static_cast<i32>(it->first)].Enabled || !layers[static_cast<i32>(it->first)].UseAuthoredMesh ||
                m_Layers[static_cast<i32>(it->first)].InstanceCount == 0 ||
                it->second.Path != layers[static_cast<i32>(it->first)].MeshPath)
            {
                if (it->first < static_cast<u32>(m_Layers.Num()))
                    EvictStreamingMesh(m_Layers[static_cast<i32>(it->first)]);
                DropStreamingLayer(it->second);
                it = m_StreamingLayers.erase(it);
                changed = true;
            }
            else
                ++it;
        }
        for (u32 i = 0; i < static_cast<u32>(std::min(layers.Num(), m_Layers.Num())); ++i)
        {
            const auto& layer = layers[static_cast<i32>(i)];
            auto& data = m_Layers[static_cast<i32>(i)];
            if (!layer.Enabled || !layer.UseAuthoredMesh || layer.MeshPath.IsEmpty() || data.InstanceCount == 0)
                continue;
            auto& state = m_StreamingLayers[i];
            if (state.Key == 0)
            {
                state.Key = s_FoliageStreamingKey.fetch_add(1u, std::memory_order_relaxed);
                state.Path = layer.MeshPath;
            }
            f32 nearest = std::numeric_limits<f32>::max();
            for (const auto& group : m_Registry.GetGroups())
            {
                if (group.m_LayerIndex != i)
                    continue;
                const glm::vec3 closest = glm::clamp(cameraPosition, group.m_WorldBounds.Min, group.m_WorldBounds.Max);
                nearest = std::min(nearest, glm::length(cameraPosition - closest));
            }
            const f32 distance = std::isfinite(layer.MeshViewDistance) ? std::max(layer.MeshViewDistance, 0.0f) : 30.0f;
            const bool wanted = nearest <= distance * (data.MeshVBO ? 1.1f : 1.0f);
            auto& budget = RepresentationStreaming::Get();
            const bool pressure = budget.IsUnderPressure();
            if (!wanted || pressure)
            {
                if (data.MeshVBO)
                {
                    EvictStreamingMesh(data);
                    budget.Release(state.Key);
                    ++m_StreamingStats.Evictions;
                    m_WindHistory.Reset();
                    changed = true;
                }
                (void)m_StreamingLoads.Cancel(state.Key);
                state.Ready.Reset();
                if (state.StagingTicket)
                    m_StreamingLoads.ReleaseStaging(std::exchange(state.StagingTicket, 0u));
                continue;
            }
            if (data.MeshVBO)
            {
                // Adopt pre-existing authoring geometry when streaming is enabled
                // live. Refuse it if the optional budget cannot hold it.
                if (!budget.IsResident(state.Key))
                {
                    TArray<u64> backingKeys;
                    backingKeys.Add(RHI::HashKey(data.MeshVBO->GetRHIHandle()));
                    if (data.MeshIBO)
                        backingKeys.Add(RHI::HashKey(data.MeshIBO->GetRHIHandle()));
                    for (const auto& part : data.MeshParts)
                        if (part.Albedo)
                            backingKeys.Add(RHI::HashKey(part.Albedo->GetRHIHandle()));
                    const auto physicalBytes = RendererMemoryTracker::GetInstance().GetLiveBackingGpuBytes(
                        std::span<const u64>(backingKeys.GetData(), static_cast<sizet>(backingKeys.Num())));
                    if (physicalBytes)
                        state.Descriptor.GpuBytes = *physicalBytes;
                    // A logical-size fallback would hide missing or padded
                    // backing accounting. Unknown detail stays on pinned cards.
                    if (!physicalBytes || !budget.TryAdmit(state.Key, state.Descriptor))
                    {
                        EvictStreamingMesh(data);
                        ++m_StreamingStats.Evictions;
                        changed = true;
                    }
                    else
                        state.WasResident = true;
                }
                if (data.MeshVBO)
                    changed |= UpdateStreamingAlphaCutoffs(data, state, layer);
                continue;
            }
            if (state.Ready)
            {
                changed |= UploadStreamingMesh(data, state, layer);
                if (state.UploadFailed && state.StagingTicket)
                    m_StreamingLoads.ReleaseStaging(std::exchange(state.StagingTicket, 0u));
                continue;
            }
            if (state.UploadFailed || m_StreamingLoads.IsPending(state.Key) || m_StreamingLoads.HasFailed(state.Key))
                continue;
            const auto file = ResolveContentPath(layer.MeshPath.ToView());
            std::error_code error;
            const u64 fileBytes = file.empty() ? 0u : std::filesystem::file_size(file, error);
            if (file.empty() || error || fileBytes > std::numeric_limits<u64>::max() / 16u)
                continue;
            // Source size estimates admission. Preparation reports actual OBJ,
            // material and image-probe reads through the payload's IOStats.
            state.Descriptor.DiskBytes = FAssetByteSize::Estimate(fileBytes);
            state.Descriptor.CpuBytes = FAssetByteSize::Estimate(std::max<u64>(1024u * 1024u, fileBytes * 16u));
            (void)m_StreamingLoads.Request(state.Key, state.Descriptor, [file]() -> Ref<FRepresentationPayload>
                                           { return LoadFoliageStreamingPayload(file); });
        }
        RefreshStreamingStats();
        return changed;
    }

    void FoliageRenderer::RefreshStreamingStats()
    {
        const u64 previouslyTrackedCpu = m_StreamingStats.CanonicalCpuBytes;
        const u64 previouslyPreparedCpu = m_StreamingStats.PreparedCpuBytes;
        m_StreamingStats.PinnedGpuBytes = 0;
        m_StreamingStats.OptionalGpuBytes = 0;
        m_StreamingStats.CanonicalCpuBytes = m_Registry.GetRecords().GetAllocatedSize() + m_Registry.GetGroups().GetAllocatedSize();
        m_StreamingStats.ResidentLayers = 0;
        m_StreamingStats.PendingLayers = 0;
        m_StreamingStats.FallbackLayers = 0;
        m_StreamingStats.PreparedCpuBytes = 0;
        for (const auto& group : m_Registry.GetGroups())
            m_StreamingStats.CanonicalCpuBytes += group.m_Instances.GetAllocatedSize() + group.m_RecordIndices.GetAllocatedSize();
        u32 layerIndex = 0;
        for (const auto& data : m_Layers)
        {
            const auto streamed = m_StreamingLayers.find(layerIndex++);
            m_StreamingStats.PinnedGpuBytes += static_cast<u64>(data.InstanceCapacity) * sizeof(FoliageInstanceData);
            if (data.QuadVBO)
                m_StreamingStats.PinnedGpuBytes += 4u * sizeof(Vertex) + 6u * sizeof(u32);
            m_StreamingStats.PinnedGpuBytes += FoliageTextureBytes(data.AlbedoTexture) + FoliageTextureBytes(data.LeafNormalTexture) +
                                               FoliageTextureBytes(data.LeafRoughnessTexture) + FoliageTextureBytes(data.LeafThicknessTexture) +
                                               FoliageTextureBytes(data.Impostor.Albedo) + FoliageTextureBytes(data.Impostor.NormalDepth);
            if (data.CullLayer.LayerBuffer)
                m_StreamingStats.PinnedGpuBytes += data.CullLayer.LayerBuffer->GetSize();
            for (const auto& view : data.CullViews)
            {
                if (view.Resources.Compacted)
                    m_StreamingStats.PinnedGpuBytes += static_cast<u64>(view.Resources.Capacity) * sizeof(FoliageInstanceData);
                if (view.Resources.State)
                    m_StreamingStats.PinnedGpuBytes += view.Resources.State->GetSize();
                if (view.Resources.DrawArgs)
                    m_StreamingStats.PinnedGpuBytes += view.Resources.DrawArgs->GetSize();
            }
            if (data.MeshVBO)
            {
                ++m_StreamingStats.ResidentLayers;
                if (m_StreamingEnabled && streamed != m_StreamingLayers.end() && RepresentationStreaming::Get().IsResident(streamed->second.Key))
                    m_StreamingStats.OptionalGpuBytes += streamed->second.Descriptor.GpuBytes;
                else
                {
                    m_StreamingStats.OptionalGpuBytes += static_cast<u64>(data.MeshVertexCount) * sizeof(Vertex) +
                                                         static_cast<u64>(data.MeshIndexCount) * sizeof(u32);
                    for (const auto& part : data.MeshParts)
                        m_StreamingStats.OptionalGpuBytes += FoliageTextureBytes(part.Albedo);
                }
            }
            else if (data.MeshRequested && data.InstanceCount > 0)
                ++m_StreamingStats.FallbackLayers;
        }
        for (const auto& [index, state] : m_StreamingLayers)
        {
            if (state.Ready || m_StreamingLoads.IsPending(state.Key))
                ++m_StreamingStats.PendingLayers;
            if (state.Ready)
                m_StreamingStats.PreparedCpuBytes += state.Ready->GetCpuBytes();
        }
        m_StreamingStats.PendingCpuBytes = m_StreamingLoads.GetStats().StagingBytes;
        RendererMemoryOwnerScope owner("Foliage canonical arrays", MemoryLifetime::Asset);
        if (previouslyTrackedCpu != m_StreamingStats.CanonicalCpuBytes)
        {
            if (previouslyTrackedCpu)
                OLO_TRACK_DEALLOC(&m_Registry);
            if (m_StreamingStats.CanonicalCpuBytes)
                OLO_TRACK_CPU_ALLOC(&m_Registry, m_StreamingStats.CanonicalCpuBytes, RendererMemoryTracker::ResourceType::Other, "Foliage canonical arrays");
        }
        if (previouslyPreparedCpu != m_StreamingStats.PreparedCpuBytes)
        {
            if (previouslyPreparedCpu)
                OLO_TRACK_DEALLOC(&m_StreamingLoads);
            if (m_StreamingStats.PreparedCpuBytes)
                OLO_TRACK_CPU_ALLOC(&m_StreamingLoads, m_StreamingStats.PreparedCpuBytes, RendererMemoryTracker::ResourceType::Other, "Foliage prepared payloads");
        }
        auto& budget = RepresentationStreaming::Get();
        budget.Release(m_StreamingPinnedKey);
        if (m_StreamingEnabled)
        {
            FRepresentationDescriptor floor;
            floor.GpuBytes = m_StreamingStats.PinnedGpuBytes;
            (void)budget.TryAdmit(m_StreamingPinnedKey, floor, true);
        }
    }

    void FoliageRenderer::RebuildVertexArrays(LayerRenderData& data) const
    {
        // The instance stream has to be bound into EVERY vertex array the layer
        // draws from, and a capacity grow replaces that buffer — so the arrays
        // are rebuilt from the surviving geometry buffers rather than each call
        // site remembering to re-add it to both.
        if (data.QuadVBO && data.IBO)
        {
            data.VAO = VertexArray::Create();
            data.VAO->AddVertexBuffer(data.QuadVBO);
            data.VAO->SetIndexBuffer(data.IBO);
            if (data.InstanceVBO)
                data.VAO->AddInstanceBuffer(data.InstanceVBO);
        }

        if (data.MeshVBO && data.MeshIBO)
        {
            data.MeshVAO = VertexArray::Create();
            data.MeshVAO->AddVertexBuffer(data.MeshVBO);
            data.MeshVAO->SetIndexBuffer(data.MeshIBO);
            if (data.InstanceVBO)
                data.MeshVAO->AddInstanceBuffer(data.InstanceVBO);
        }

        // The GPU cull's arrays stream the SAME geometry over a different
        // instance buffer, so a geometry change invalidates them too (issue
        // #1235). Rebuilding here rather than at each call site is the same
        // argument this function was split out for.
        for (u32 slot = 0; slot < FoliageGPUCuller::kViewSlotCount; ++slot)
            RebuildCulledVertexArrays(data, slot);
    }

    void FoliageRenderer::RebuildCulledVertexArrays(LayerRenderData& data, u32 slot) const
    {
        auto& view = data.CullViews[slot];
        view.CardVAO.Reset();
        view.MeshVAO.Reset();

        const Ref<VertexBuffer>& compacted = view.Resources.Compacted;
        if (!compacted)
            return;

        if (data.QuadVBO && data.IBO)
        {
            view.CardVAO = VertexArray::Create();
            view.CardVAO->AddVertexBuffer(data.QuadVBO);
            view.CardVAO->SetIndexBuffer(data.IBO);
            view.CardVAO->AddInstanceBuffer(compacted);
        }
        if (data.MeshVBO && data.MeshIBO)
        {
            view.MeshVAO = VertexArray::Create();
            view.MeshVAO->AddVertexBuffer(data.MeshVBO);
            view.MeshVAO->SetIndexBuffer(data.MeshIBO);
            view.MeshVAO->AddInstanceBuffer(compacted);
        }
    }

    void FoliageRenderer::UploadInstances(LayerRenderData& data, const TArray<FoliageInstanceData>& instances)
    {
        if (instances.IsEmpty())
        {
            data.InstanceCount = 0;
            return;
        }

        auto requiredCount = static_cast<u32>(instances.Num());
        auto dataSize = static_cast<u32>(instances.Num() * sizeof(FoliageInstanceData));

        const bool grew = data.InstanceVBO && data.InstanceCapacity < requiredCount;
        if (!data.InstanceVBO || grew)
        {
            if (grew)
            {
                OLO_CORE_INFO("FoliageRenderer: instance VBO GROW {} -> {} instances", data.InstanceCapacity,
                              requiredCount * 2);
                data.InstanceCapacity = requiredCount * 2;
            }
            else
            {
                OLO_CORE_INFO("FoliageRenderer: instance VBO create ({} instances)", requiredCount);
                data.InstanceCapacity = std::max(requiredCount, 256u);
            }

            u32 allocSize = data.InstanceCapacity * static_cast<u32>(sizeof(FoliageInstanceData));
            data.InstanceVBO = VertexBuffer::Create(allocSize);
            data.InstanceVBO->SetLayout({
                { ShaderDataType::Float4, "a_PositionScale" },
                { ShaderDataType::Float4, "a_RotationHeight" },
                { ShaderDataType::Float4, "a_ColorAlpha" },
            });
            // A new instance buffer has to reach EVERY vertex array the layer
            // draws from — the card's and, since #1233, the authored mesh's.
            // Rebuilding them both also avoids the duplicate attribute bindings
            // that re-adding an instance buffer to a live array would leave.
            RebuildVertexArrays(data);
        }

        data.InstanceVBO->SetData({ instances.GetData(), dataSize });
        data.InstanceCount = requiredCount;
    }

    namespace
    {
        // The two FoliageUBO lanes, from the sanitised parameters. ONE
        // definition of the packing (issue #1237): the beauty submission path,
        // the shadow path, the direct Render() path and the cull state header
        // all take their lanes from here, so a reordering cannot leave one
        // site reading `end` where another writes `minFraction`.
        [[nodiscard]] glm::vec4 FoliageLodTransition0(const FoliageLod::Params& lod)
        {
            return glm::vec4(FoliageLod::PackFlags(lod), lod.Start, lod.End, lod.MinFraction);
        }

        [[nodiscard]] glm::vec4 FoliageLodTransition1(const FoliageLod::Params& lod)
        {
            return glm::vec4(lod.FadeFraction, lod.MaxScale, lod.TransitionSpread, lod.Hysteresis);
        }
    } // namespace

    void FoliageRenderer::EnumerateLayerDraws(const LayerRenderData& data, TArray<LayerDraw>& out) const
    {
        out.Reset();
        if (data.InstanceCount == 0)
            return;

        const bool meshDrawable = data.MeshVAO && !data.MeshParts.IsEmpty() && data.MeshViewDistance > 0.0f;
        // The card is the mesh's bake whenever a textured mesh LOADED (#1533),
        // whatever its hand-over distance: the card texture was baked from it
        // either way (MeshLayerCardLane).
        const f32 cardLane = MeshLayerCardLane(data);
        const f32 handoverStart = meshDrawable ? data.MeshFadeStartDistance : 0.0f;
        const f32 handoverEnd = meshDrawable ? data.MeshViewDistance : 0.0f;

        // Near field: the authored plant mesh, one draw per submesh so a plant
        // whose bark and leaves are different materials renders as authored
        // (issue #1233, first criterion).
        if (meshDrawable)
        {
            for (const auto& part : data.MeshParts)
            {
                LayerDraw draw;
                draw.VAO = data.MeshVAO;
                draw.BaseIndex = part.BaseIndex;
                draw.IndexCount = part.IndexCount;
                draw.Albedo = part.Albedo ? part.Albedo : data.AlbedoTexture;
                draw.IsAuthoredMesh = true;
                draw.HandoverStart = handoverStart;
                draw.HandoverEnd = handoverEnd;
                draw.CardNormalLane = cardLane;
                draw.FadeStart = data.FadeStartDistance;
                draw.ViewDistance = data.ViewDistance;
                draw.LodTransition0 = FoliageLodTransition0(data.Lod);
                draw.LodTransition1 = FoliageLodTransition1(data.Lod);
                out.Add(std::move(draw));
            }
        }

        // Far field: the flat card, which the impostor path also rides. It
        // carries the SAME hand-over band as the mesh draws above, and keeps
        // exactly the pixels they do not. With no mesh the band is zero-width
        // and this is the single draw the layer has always emitted.
        if (data.VAO && data.IndexCount > 0)
        {
            LayerDraw draw;
            draw.VAO = data.VAO;
            draw.BaseIndex = 0;
            draw.IndexCount = data.IndexCount;
            draw.Albedo = data.AlbedoTexture;
            draw.IsAuthoredMesh = false;
            draw.HandoverStart = handoverStart;
            draw.HandoverEnd = handoverEnd;
            draw.CardNormalLane = cardLane;
            draw.FadeStart = data.FadeStartDistance;
            draw.ViewDistance = data.ViewDistance;
            draw.LodTransition0 = FoliageLodTransition0(data.Lod);
            draw.LodTransition1 = FoliageLodTransition1(data.Lod);
            out.Add(std::move(draw));
        }
    }

    void FoliageRenderer::GenerateInstances(
        const TArray<FoliageLayer>& layers,
        const TerrainData& terrainData,
        const TerrainMaterial* material,
        f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();
        RendererMemoryOwnerScope foliageOwner("Foliage pinned representations", MemoryLifetime::Asset);

        m_WindHistory.Reset();

        // A shrinking layer list drops the trailing LayerRenderData entries
        // below — free their impostor VRAM budget claims first, or resize()
        // destroying them silently leaks the claims for the rest of the
        // process (issue #718; ImpostorAtlas has no destructor of its own).
        for (sizet i = layers.Num(); i < m_Layers.Num(); ++i)
            ImpostorBaker::Free(m_Layers[i].Impostor);
        m_Layers.SetNum(layers.Num(), EAllowShrinking::No);

        // Canonical identity (issue #1230). Everything live becomes a candidate
        // for survival; a placement this pass does not re-emit — because its
        // layer vanished, was disabled, or its cell stopped qualifying — retires
        // at EndGeneration and its id is never handed to another plant.
        m_Registry.BeginGeneration(layers);

        // One CPU/GPU height sync for the whole generation rather than two per
        // grid cell, which is what going through TerrainData::GetHeightAt and
        // GetNormalAt cost (each calls SyncFromGPU).
        const TArray<f32>& heights = terrainData.GetHeightData();
        const u32 heightResolution = terrainData.GetResolution();

        TArray<FoliagePlacement::Placement> placements;
        TArray<FoliageInstanceData> instances;

        for (sizet layerIdx = 0; layerIdx < layers.Num(); ++layerIdx)
        {
            const auto& layer = layers[layerIdx];
            auto& renderData = m_Layers[layerIdx];

            if (!layer.Enabled || layer.Density <= 0.0f)
            {
                // Draws nothing, so it owns no canonical instances. No
                // BeginLayer means EndGeneration retires whatever it had;
                // re-enabling the layer issues FRESH ids rather than reviving
                // the old ones, which is the deterministic answer and never a
                // silent reuse.
                renderData.InstanceCount = 0;
                // Nor does it draw any texture, so it has no coverage to
                // report — the inspector would otherwise keep judging the
                // textures it drew before it was switched off.
                renderData.AlphaCoverage.Reset();
                renderData.AlphaCoverageDirty = true;
                continue;
            }

            // Geometry. The card is always built — it is what covers the
            // distance band and what a layer with no authored mesh draws
            // everywhere. The authored mesh (issue #1233) is built beside it,
            // never instead of it, and only re-imported when the path changes.
            bool geometryChanged = false;
            if (!renderData.QuadVBO)
            {
                BuildQuadGeometry(renderData);
                geometryChanged = true;
            }

            const bool meshRequested = layer.UseAuthoredMesh && !layer.MeshPath.IsEmpty();
            if (!meshRequested)
            {
                if (renderData.MeshVBO || !renderData.MeshGeometryPath.IsEmpty())
                {
                    renderData.MeshVAO = nullptr;
                    renderData.MeshVBO = nullptr;
                    renderData.MeshIBO = nullptr;
                    renderData.MeshParts.Reset();
                    renderData.MeshPartSurfaceUVs.Reset();
                    renderData.AlphaCoverageDirty = true;
                    renderData.MeshModel = nullptr;
                    renderData.MeshVertexCount = 0;
                    renderData.MeshIndexCount = 0;
                    renderData.MeshBounds = BoundingBox{};
                    renderData.MeshGeometryPath.Empty();
                    renderData.CardUsesMeshBake = false;
                    renderData.CardNormalTilt = 0.0f;
                    renderData.BoundsProfile = FoliageBoundsProfile{};
                    geometryChanged = true;
                }
            }
            else if (renderData.MeshGeometryPath != layer.MeshPath)
            {
                if (!m_StreamingEnabled)
                    BuildMeshGeometry(renderData, layer);
                else
                {
                    EvictStreamingMesh(renderData);
                    renderData.BoundsProfile = FoliageBoundsProfile{};
                    renderData.CardUsesMeshBake = false;
                    renderData.CardNormalTilt = 0.0f;
                }
                // Recorded even when the import FAILED, so a broken path is
                // reported once per edit rather than re-imported and re-logged
                // on every regeneration.
                renderData.MeshGeometryPath = layer.MeshPath;
                geometryChanged = true;
            }
            renderData.MeshRequested = meshRequested;

            if (geometryChanged || !renderData.VAO)
            {
                RebuildVertexArrays(renderData);
            }

            // Store layer render properties
            renderData.ViewDistance = layer.ViewDistance;
            renderData.FadeStartDistance = layer.FadeStartDistance;
            renderData.WindStrength = layer.WindStrength;
            renderData.WindSpeed = layer.WindSpeed;
            renderData.WindWeights = SanitizeFoliageWind(layer.WindStiffness, layer.WindBranchWeight, layer.WindLeafWeight, layer.WindDebugDisplacement);
            // Sanitised here, once, rather than at each of the three UBO-fill
            // sites: this number scales a displacement AND derives the bound
            // that displacement is padded into, so the two must come from the
            // same value or a layer can bend further than its own AABB.
            renderData.InteractionResponse =
                std::isfinite(layer.InteractionResponse) ? std::clamp(layer.InteractionResponse, 0.0f, 8.0f) : 1.0f;
            renderData.BaseColor = layer.BaseColor;
            renderData.AlphaCutoff = layer.AlphaCutoff;
            renderData.CastShadows = layer.CastShadows;

            // LOD transitions + coverage-preserving density (issue #1237).
            // Sanitised HERE, once, for the same reason InteractionResponse
            // above is: these numbers reach a smoothstep and a reciprocal
            // square root in four shader stages AND the cull kernel's drop
            // test, and the cull may only remove plants the draw would have
            // drawn transparent — which is only true while both sides read the
            // same sanitised value.
            {
                FoliageLod::Params authored;
                authored.Enabled = layer.UseDensityLod;
                authored.StochasticCoverage = layer.LodStochasticCoverage;
                authored.Start = layer.DensityLodStartDistance;
                authored.End = layer.DensityLodEndDistance;
                authored.MinFraction = layer.DensityLodMinFraction;
                authored.FadeFraction = layer.DensityLodFadeFraction;
                authored.MaxScale = layer.DensityLodMaxScale;
                authored.TransitionSpread = layer.LodTransitionSpread;
                authored.Hysteresis = layer.LodHysteresis;
                renderData.Lod = FoliageLod::Sanitise(authored);
            }

            // Near-field hand-over band (issue #1233). Sanitised here rather
            // than trusted: these reach a smoothstep in the vertex and fragment
            // stages, where a NaN or an inverted band silently drops the layer.
            const bool meshDrawable = meshRequested && renderData.MeshVBO && !renderData.MeshParts.IsEmpty();
            if (meshDrawable)
            {
                renderData.MeshViewDistance = std::isfinite(layer.MeshViewDistance)
                                                  ? std::max(layer.MeshViewDistance, 0.0f)
                                                  : 30.0f;
                renderData.MeshFadeStartDistance = std::isfinite(layer.MeshFadeStartDistance)
                                                       ? std::clamp(layer.MeshFadeStartDistance, 0.0f,
                                                                    renderData.MeshViewDistance)
                                                       : std::min(22.0f, renderData.MeshViewDistance);
            }
            else
            {
                // No mesh: the card covers everything, exactly as before #1233.
                renderData.MeshViewDistance = 0.0f;
                renderData.MeshFadeStartDistance = 0.0f;
            }

            // Load the albedo — foliage albedo is authored colour and needs
            // sRGB->linear conversion on sample. Keyed on the PATH, the same
            // rule as the leaf maps below: it used to load only while the Ref
            // was null, so editing Albedo Path in the inspector kept drawing
            // the first texture forever — and re-baked the impostor with that
            // old texture while recording the new path as baked.
            if (renderData.LoadedAlbedoPath != layer.AlbedoPath)
            {
                renderData.LoadedAlbedoPath = layer.AlbedoPath;
                // An albedo path that resolves to no file (logged by the resolver)
                // draws with no albedo texture, never a read against the cwd.
                const std::filesystem::path albedoFile =
                    layer.AlbedoPath.IsEmpty() ? std::filesystem::path{} : ResolveContentPath(layer.AlbedoPath.ToView());
                renderData.AlbedoTexture = albedoFile.empty()
                                               ? nullptr
                                               : Texture2D::Create(albedoFile.generic_string(), /*srgb=*/true);
                renderData.AlphaCoverageDirty = true;
            }

            // Every albedo this layer draws is alpha-tested at the LAYER's
            // cutoff, the card's and each mesh part's alike, so each gets the
            // mip chain that keeps its passing fraction at every level (issue
            // #1441). A plain box chain thins a cutout with distance. A no-op
            // when the cutoff is unchanged, so this costs nothing per
            // regeneration; editing the cutoff rebuilds the chains.
            if (renderData.AlbedoTexture)
                renderData.AlbedoTexture->SetAlphaCoverageCutoff(layer.AlphaCutoff);
            for (const auto& part : renderData.MeshParts)
            {
                // By value: Ref<T> propagates the constness of the handle.
                if (Ref<Texture2D> partAlbedo = part.Albedo)
                {
                    if (!m_StreamingEnabled)
                        partAlbedo->SetAlphaCoverageCutoff(layer.AlphaCutoff);
                }
            }

            // ── The leaf material (issue #1234) ─────────────────────────────
            // All three maps are LINEAR data, not authored colour: a tangent
            // normal, a roughness and a thickness. sRGB-decoding any of them
            // would bend the normals and darken the roughness by a gamma
            // nobody could see in the inspector.
            //
            // Keyed on the PATH: a changed path re-opens, an unchanged one does
            // not, and CLEARING the path drops the Ref so the map bitfield goes
            // back to "not authored" instead of leaving the last texture bound.
            const auto loadLeafMap = [](const FString& path, FString& loadedPath,
                                        Ref<Texture2D>& texture)
            {
                // The PATH is the cache key, and it is recorded even when the
                // load FAILS — same rule as the authored mesh above, for the
                // same reason: a broken path is re-opened and re-logged on
                // every regeneration otherwise. Clearing the path in the
                // inspector drops the Ref, so the map bitfield goes back to
                // "not authored" rather than leaving the last texture bound.
                if (loadedPath == path)
                    return;
                loadedPath = path;
                const std::filesystem::path file =
                    path.IsEmpty() ? std::filesystem::path{} : ResolveContentPath(path.ToView());
                texture = file.empty() ? nullptr : Texture2D::Create(file.generic_string(), /*srgb=*/false);

                // Texture2D::Create NEVER RETURNS NULL — a file that will not
                // open still yields a Ref, and IsLoaded() is the only thing
                // that says so. Dropping the Ref here is what makes the failure
                // behave as "no map authored" (the shader uses the layer's
                // constant, because the map bitfield is derived from the
                // HANDLE) instead of sampling whatever the failed texture
                // object contains. Never a silent fallback: a leaf shaded by a
                // normal the author did not write is worse than one shaded by
                // the constant they did.
                if (texture && !texture->IsLoaded())
                {
                    OLO_CORE_WARN("FoliageRenderer - leaf map '{}' could not be loaded; the layer shades with its "
                                  "authored constant instead of the map.",
                                  path.ToView());
                    texture = nullptr;
                }
            };
            loadLeafMap(layer.NormalMapPath, renderData.LoadedNormalPath, renderData.LeafNormalTexture);
            loadLeafMap(layer.RoughnessMapPath, renderData.LoadedRoughnessPath, renderData.LeafRoughnessTexture);
            loadLeafMap(layer.ThicknessMapPath, renderData.LoadedThicknessPath, renderData.LeafThicknessTexture);

            // The scalars, sanitised here as well as at both deserializers —
            // the inspector writes straight into the component, so a value
            // typed into the editor never passes through either of those.
            // Every one of these reaches a pow() exponent, a normalize() or a
            // clamp bound in the shared shader evaluation.
            const auto finiteOr = [](f32 v, f32 fallback, f32 lo, f32 hi)
            { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; };
            renderData.LeafRoughness = finiteOr(layer.Roughness, 0.8f, 0.02f, 1.0f);
            renderData.LeafNormalStrength = finiteOr(layer.NormalStrength, 1.0f, 0.0f, 4.0f);
            renderData.LeafThickness = finiteOr(layer.Thickness, 0.5f, 0.0f, 1.0f);
            renderData.LeafTransmissionStrength = finiteOr(layer.TransmissionStrength, 0.0f, 0.0f, 8.0f);
            renderData.LeafTransmissionColor =
                glm::vec3(finiteOr(layer.TransmissionColor.x, 0.42f, 0.0f, 1.0f),
                          finiteOr(layer.TransmissionColor.y, 0.62f, 0.0f, 1.0f),
                          finiteOr(layer.TransmissionColor.z, 0.18f, 0.0f, 1.0f));
            renderData.LeafTransmissionDistortion = finiteOr(layer.TransmissionDistortion, 0.35f, 0.0f, 1.0f);
            renderData.LeafTransmissionPower = finiteOr(layer.TransmissionPower, 4.0f, 1.0f, 64.0f);
            renderData.LeafTransmissionWrap = finiteOr(layer.TransmissionWrap, 0.5f, 0.0f, 1.0f);
            renderData.LeafTransmissionAmbient = finiteOr(layer.TransmissionAmbient, 0.35f, 0.0f, 4.0f);

            // Octahedral impostor LOD (issue #433): store the per-layer params and
            // bake/re-bake the atlas from the layer mesh if needed.
            renderData.UseImpostor = layer.UseImpostor;
            renderData.ImpostorStartDistance = layer.ImpostorStartDistance;
            renderData.ImpostorTransitionBand = layer.ImpostorTransitionBand;
            UpdateImpostorAtlas(renderData, layer);

            FoliagePlacement::GenerateLayer(layer, static_cast<u32>(layerIdx), heights, heightResolution,
                                            material, worldSizeX, worldSizeZ, heightScale, placements);

            // Explicit representation metadata, not a flag a consumer has to
            // re-derive. A layer that asked for an authored mesh or an impostor
            // and got neither still draws as a flat card, so its instances are
            // MeshCard — but the VARIANT it authored is unavailable, and that is
            // counted rather than left to the one-off log line.
            const bool impostorRequested = layer.UseImpostor;
            const bool impostorAvailable = impostorRequested && renderData.Impostor.IsValid();

            // Judged against exactly what EnumerateLayerDraws will draw: the
            // mesh only while it has a band to cover, the impostor in place of
            // the flat card when there is one.
            UpdateAlphaCoverage(renderData, layer, meshDrawable && renderData.MeshViewDistance > 0.0f,
                                impostorAvailable);
            if (impostorAvailable)
            {
                if (!m_ImpostorDepthShader)
                    m_ImpostorDepthShader = Shader::Create("assets/shaders/Foliage_Impostor_Depth.glsl");
                const f32 radius = FoliageImpostorBoundsRadius(renderData.Impostor.Radius);
                renderData.BoundsProfile.m_HalfExtentXZHeightScaled = std::max(renderData.BoundsProfile.m_HalfExtentXZHeightScaled, radius);
                renderData.BoundsProfile.m_MinY = std::min(renderData.BoundsProfile.m_MinY, 0.5f - radius);
                renderData.BoundsProfile.m_MaxY = std::max(renderData.BoundsProfile.m_MaxY, 0.5f + radius);
            }
            // The NEAR field names the representation, because that is what the
            // instance's bounds and its material assignment are derived from:
            // an authored-mesh plant still hands over to a card or an impostor
            // at distance, and reporting it as a card would hide the geometry
            // that actually costs and actually bounds (issue #1233).
            FoliageRepresentation representation = FoliageRepresentation::Unsupported;
            if (meshDrawable)
            {
                representation = FoliageRepresentation::AuthoredMesh;
            }
            else if (impostorAvailable)
            {
                representation = FoliageRepresentation::Impostor;
            }
            else if (renderData.VAO)
            {
                representation = FoliageRepresentation::MeshCard;
            }

            // Either authored variant asked for and not delivered counts, and
            // BuildMeshGeometry / UpdateImpostorAtlas have already said which,
            // loudly, in the log.
            const bool variantUnavailable = (impostorRequested && !impostorAvailable) ||
                                            (meshRequested && !meshDrawable);

            renderData.BoundsProfile.m_WindDisplacement = FoliageWindMaximumDisplacement(layer.WindStrength, renderData.WindWeights, m_LegacyWindEnvelope);
            // Derived from the AUTHORED response, not from whatever the field
            // happens to hold this frame: the profile is hashed into every
            // instance's identity, so a bound that moved with a passing actor
            // would retire and re-issue the whole layer's ids as it walked past.
            renderData.BoundsProfile.m_InteractionDisplacement =
                FoliageInteractionMaximumDisplacement(renderData.InteractionResponse);

            // Coverage-preserving density GROWS the surviving plants (issue
            // #1237), and a plant is culled by its bound. Uncompensated, the
            // bound describes the authored size while the vertex stages draw up
            // to MaxScale times it, so a grown plant is rejected by a box
            // smaller than the plant and pops in at the edge of the frustum.
            //
            // Inflated by the CAP rather than by the distance-dependent factor,
            // for two reasons that both matter more than the tightness lost: a
            // bound has to be CONSERVATIVE, and it is hashed into every
            // instance's identity — a bound that moved with the camera would
            // retire and re-issue the whole layer's ids every frame, exactly
            // the trap the interaction displacement above documents.
            //
            // Applied here, after every other term, so the registry, the CPU
            // per-layer AABB and the GPU cull's oloFoliageInstanceBounds all
            // read the same inflated profile with no shader change at all.
            if (renderData.Lod.Enabled)
            {
                const f32 grow = std::max(renderData.Lod.MaxScale, 1.0f);
                renderData.BoundsProfile.m_HalfExtentXZ *= grow;
                renderData.BoundsProfile.m_HalfExtentXZHeightScaled *= grow;
                renderData.BoundsProfile.m_MinY *= grow;
                renderData.BoundsProfile.m_MaxY *= grow;
            }

            m_Registry.BeginLayer(static_cast<u32>(layerIdx), layer,
                                  FoliagePlacement::SeedForLayer(static_cast<u32>(layerIdx)),
                                  FoliagePlacement::SpacingForDensity(layer.Density),
                                  worldSizeX, worldSizeZ,
                                  representation, variantUnavailable, renderData.BoundsProfile);

            // The buffer row is assigned here and recorded as a PROJECTION of
            // the record. Identity comes from the placement cell, so a
            // regeneration that emits the same plants in a different order
            // leaves every id untouched.
            instances.Reset();
            instances.Reserve(placements.Num());
            for (const auto& placement : placements)
            {
                m_Registry.AddInstance(placement.m_CellX, placement.m_CellZ, placement.m_Row,
                                       static_cast<u32>(instances.Num()));
                auto row = placement.m_Row;
                row.RotationHeight.w = FoliageWindPhase(m_Registry.GetRecords().Last().m_Id);
                instances.Add(row);
            }
            m_Registry.EndLayer();

            // Compute bounding box from all instance positions (with height expansion)
            if (!instances.IsEmpty())
            {
                glm::vec3 bMin(std::numeric_limits<f32>::max());
                glm::vec3 bMax(std::numeric_limits<f32>::lowest());
                for (const auto& inst : instances)
                {
                    const glm::vec3 pos(inst.PositionScale.x, inst.PositionScale.y, inst.PositionScale.z);
                    // ONE bounds rule for the per-layer AABB and the registry's
                    // per-instance records — the same function, so a mesh that
                    // widens one cannot leave the other bounding a quad.
                    const BoundingBox instanceBox = FoliageInstanceBounds(
                        pos, inst.PositionScale.w, inst.RotationHeight.y, renderData.BoundsProfile);
                    bMin = glm::min(bMin, instanceBox.Min);
                    bMax = glm::max(bMax, instanceBox.Max);
                }
                renderData.Bounds = BoundingBox(bMin, bMax);
            }
            else
            {
                renderData.Bounds = BoundingBox(glm::vec3(0.0f), glm::vec3(0.0f));
            }

            UploadInstances(renderData, instances);
        }

        m_Registry.EndGeneration();
    }

    void FoliageRenderer::ClearInstances()
    {
        if (m_StreamingEnabled)
        {
            // Disabled and empty components bypass UpdateStreamingResidency.
            // Release their detail and reservations here; abandoned workers
            // keep their staging charge until their immutable payload is gone.
            m_StreamingLoads.ReapAbandoned();
            const bool hadDetail = !m_StreamingLayers.empty();
            for (auto& [index, state] : m_StreamingLayers)
                DropStreamingLayer(state);
            m_StreamingLayers.clear();
            for (auto& layer : m_Layers)
                EvictStreamingMesh(layer);
            if (hadDetail)
                m_WindHistory.Reset();
        }
        m_Registry.Clear();
        for (auto& layer : m_Layers)
        {
            layer.InstanceCount = 0;
            // The cull's per-layer data describes a generation that no longer
            // exists, and every view slot's compacted set describes plants that
            // are gone. Dropping the group/row tables forces a rebuild if the
            // layer ever repopulates; clearing Active is what stops a slot
            // replaying a stale compacted draw in the meantime.
            layer.CullLayer = {};
            for (auto& view : layer.CullViews)
            {
                view.Active = false;
            }
        }
        m_MainViewCulled = false;
        if (m_StreamingEnabled)
            RefreshStreamingStats();
    }

    void FoliageRenderer::Render(
        [[maybe_unused]] const Frustum& frustum,
        [[maybe_unused]] const glm::vec3& cameraPos,
        const Ref<Shader>& shader)
    {
        OLO_PROFILE_FUNCTION();

        if (!shader)
        {
            return;
        }

        shader->Bind();
        m_VisibleInstances = 0;

        // Foliage's per-blade transforms come from its own instance VBO
        // (a_PositionScale, a_RotationHeight) in TERRAIN-LOCAL space — the same
        // space the terrain mesh is authored in, because GenerateInstances
        // derives them straight from the heightfield (x/z in [0, WorldSize], y
        // the raw sampled height, no base offset). The shaders read `u_Model`
        // from the engine's ModelInstanceBuffer (binding 15). Upload the owning
        // terrain's render-relative model matrix once so it (a) overrides
        // whatever the previous DrawMesh wrote into the SSBO, (b) places the
        // plants on their island, and (c) shifts them into render-relative
        // space — camera-relative rendering (issue #429). The foliage shaders
        // add u_RenderOrigin back for the world-anchored wind field.
        //
        // This used to upload plain identity, on the stated belief that the
        // instance positions were already absolute world (issue #953). They are
        // not, and no island in Drift sits at the origin, so all six islands'
        // foliage was drawn in one heap over open water near (0,0,0) — read
        // from the boat as a swarm of dark specks hanging in the sky.
        if (auto instanceBuffer = Renderer3D::GetModelInstanceBuffer())
        {
            InstanceData bladeModel{};
            bladeModel.EntityID = -1;
            bladeModel.Transform = MakeModelRelative(m_TerrainTransform, Renderer3D::GetRenderOrigin());
            bladeModel.PrevTransform = bladeModel.Transform;
            const std::span<const InstanceData> one(&bladeModel, 1);
            instanceBuffer->Upload(one);
            instanceBuffer->Bind();
        }

        // The main view's position, made render-relative exactly as
        // CommandDispatch makes it for the camera UBO — the hand-over between a
        // plant's mesh and its card is measured from here in every pass.
        const glm::vec3 renderRelativeViewPos =
            MakePositionRelative(CommandDispatch::GetViewPosition(), Renderer3D::GetRenderOrigin());

        TArray<LayerDraw> draws;
        for (auto& layer : m_Layers)
        {
            if (layer.InstanceCount == 0)
                continue;

            EnumerateLayerDraws(layer, draws);
            for (const auto& draw : draws)
            {
                // Upload per-draw foliage UBO
                ShaderBindingLayout::FoliageUBO foliageUBOData{};
                foliageUBOData.Time = m_Time;
                foliageUBOData.WindStrength = layer.WindStrength;
                foliageUBOData.WindSpeed = layer.WindSpeed;
                foliageUBOData.WindWeights = layer.WindWeights;
                const auto wind = WindSystem::GetGPUData();
                foliageUBOData.WindDirection = wind.DirectionAndSpeed;
                foliageUBOData.WindGust = wind.GustAndTurbulence;
                foliageUBOData.WindClock = wind.TimeAndFlags;
                foliageUBOData.PrevMeshViewPos = glm::vec4(MakePositionRelative(Renderer3D::GetPreviousViewPosition(), Renderer3D::GetRenderOrigin()), 0.0f);
                foliageUBOData.WindFlags = glm::vec4(Renderer3D::GetRenderOrigin(), wind.TimeAndFlags.y);
                foliageUBOData.WindHistoryValid = WindSystem::HasStableParameters() ? 1.0f : 0.0f;
                foliageUBOData.ViewDistance = draw.ViewDistance;
                foliageUBOData.FadeStart = draw.FadeStart;
                foliageUBOData.AlphaCutoff = layer.AlphaCutoff;
                foliageUBOData.PrevTime = m_PrevTime;
                foliageUBOData.BaseColor = glm::vec4(layer.BaseColor, 0.0f);
                foliageUBOData.MeshParams = glm::vec4(draw.IsAuthoredMesh ? 1.0f : 0.0f,
                                                      draw.HandoverStart, draw.HandoverEnd, draw.CardNormalLane);
                foliageUBOData.MeshViewPos = glm::vec4(renderRelativeViewPos, 0.0f);
                // The #1237 lanes, from the draw the enumeration packed them on.
                foliageUBOData.LodTransition0 = draw.LodTransition0;
                foliageUBOData.LodTransition1 = draw.LodTransition1;
                // The interaction field (issue #1238). Read from the field
                // itself, exactly as the wind snapshot above is — the set is
                // global per frame, so only the layer's response is per draw.
                ApplyFoliageInteraction(foliageUBOData, layer.InteractionResponse);

                // The leaf material (issue #1234). Filled here so this path
                // stays coherent: a default-constructed FoliageUBO would upload
                // LeafSurface.x = 0 — a roughness of zero, i.e. a mirror leaf —
                // rather than the layer's authored value.
                //
                // NOT AT PARITY WITH THE COMMAND PATH, AND SAY SO. Render() has
                // no callers anywhere in the tree today — every foliage draw
                // goes through Renderer3D::DrawFoliageLayer and the command
                // bucket — and unlike that dispatch it binds neither the shadow
                // contract nor the global IBL trio. Reviving it means porting
                // those binds too, or a canopy here lights from whatever a
                // previous draw happened to leave on those slots.
                f32 leafMapFlags = 0.0f;
                if (layer.LeafNormalTexture)
                    leafMapFlags += 1.0f; // OLO_LEAF_MAP_NORMAL
                if (layer.LeafRoughnessTexture)
                    leafMapFlags += 2.0f; // OLO_LEAF_MAP_ROUGHNESS
                if (layer.LeafThicknessTexture)
                    leafMapFlags += 4.0f; // OLO_LEAF_MAP_THICKNESS
                foliageUBOData.LeafSurface = glm::vec4(layer.LeafRoughness, layer.LeafNormalStrength,
                                                       layer.LeafThickness, leafMapFlags);
                foliageUBOData.LeafTransmit =
                    glm::vec4(layer.LeafTransmissionColor * layer.LeafTransmissionStrength,
                              layer.LeafTransmissionStrength);
                foliageUBOData.LeafLobe =
                    glm::vec4(layer.LeafTransmissionDistortion, layer.LeafTransmissionPower,
                              layer.LeafTransmissionWrap, layer.LeafTransmissionAmbient);
                // This path never writes a G-Buffer, so no slot is interned for
                // it: the forward program carries the lobe in the two lanes
                // above and reads no slot. kFoliageLeafSlotNone says so.
                {
                    // The deferred pass's own ladder controls (issue #1336):
                    // .y IBL bound, .z IBL intensity, .w probe volume.
                    const glm::vec4 ladderControls = DeferredLightingPass::AmbientLadderControls();
                    foliageUBOData.LeafIds = glm::vec4(static_cast<f32>(kFoliageLeafSlotNone), ladderControls.x,
                                                       ladderControls.z, ladderControls.y);
                }

                // The leaf maps, through the SAME seam the albedo goes through
                // below — a direct Texture::Bind is invisible to the heap.
                if (layer.LeafNormalTexture)
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_NORMAL,
                                                     layer.LeafNormalTexture->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);
                if (layer.LeafRoughnessTexture)
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_ROUGHNESS,
                                                     layer.LeafRoughnessTexture->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);
                if (layer.LeafThicknessTexture)
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_METALLIC,
                                                     layer.LeafThicknessTexture->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);

                auto foliageUBO = Renderer3D::GetFoliageUBO();
                foliageUBO->SetData(&foliageUBOData, ShaderBindingLayout::FoliageUBO::GetSize());

                // Bind albedo texture. THROUGH THE SEAM, not Texture::Bind — a direct
                // bind is invisible to the heap, so a converted Foliage_Instance would
                // read an offset nobody staged (issue #691). Persistent: the
                // atlas is asset-owned and outlives the frame.
                if (draw.Albedo)
                {
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE,
                                                     draw.Albedo->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);
                }

                draw.VAO->Bind();
                HeapBinding::FlushOffsets();
                RenderCommand::DrawIndexedInstancedRaw(draw.VAO->GetRHIHandle(), draw.IndexCount,
                                                       draw.BaseIndex, layer.InstanceCount);
            }
            m_VisibleInstances += layer.InstanceCount;
        }
    }

    BoundingBox FoliageRenderer::GetShadowBounds() const
    {
        BoundingBox bounds = NoBounds;
        for (const auto& layer : m_Layers)
        {
            if (layer.InstanceCount == 0 || !layer.CastShadows)
                continue;
            const auto world = layer.Bounds.Transform(m_TerrainTransform);
            bounds = bounds.Min.x >= std::numeric_limits<f32>::max() ? world : bounds.Union(world);
        }
        return bounds;
    }

    void FoliageRenderer::RenderShadows(const Ref<Shader>& depthShader, f32 time, u32 shadowViewIndex,
                                        const Ref<Shader>& impostorDepthOverride,
                                        const std::function<void()>& afterProgramBind) const
    {
        OLO_PROFILE_FUNCTION();

        if (!depthShader)
        {
            return;
        }

        // This view's cull slot, filled by DispatchShadowViewCulling before the
        // region's parallel recording began. Nothing here writes it.
        const u32 shadowSlot = (shadowViewIndex < FoliageGPUCuller::kMaxShadowViews)
                                   ? FoliageGPUCuller::ShadowSlot(shadowViewIndex)
                                   : static_cast<u32>(FoliageGPUCuller::ViewSlot::Main);
        const bool shadowCulled = shadowViewIndex < FoliageGPUCuller::kMaxShadowViews;

        depthShader->Bind();

        // Same render-relative model pattern as Render() (issue #429): the depth
        // shader reads u_Model from the ModelInstanceBuffer and foliage's per-blade
        // data is TERRAIN-LOCAL (see Render), so go through the owning terrain's
        // transform and then -renderOrigin, to render the shadow caster in the
        // same render-relative space as the shifted lightVP. Must stay identical
        // to the main pass or the shadow detaches from the plant (issue #953).
        if (auto instanceBuffer = Renderer3D::GetModelInstanceBuffer())
        {
            InstanceData bladeModel{};
            bladeModel.EntityID = -1;
            bladeModel.Transform = MakeModelRelative(m_TerrainTransform, Renderer3D::GetRenderOrigin());
            bladeModel.PrevTransform = bladeModel.Transform;
            const std::span<const InstanceData> one(&bladeModel, 1);
            instanceBuffer->Upload(one);
            instanceBuffer->Bind();
        }

        // The SAME draw list the beauty pass walks (issue #1233, fourth
        // criterion): a shadow cast from a quad while the lit plant is a pine
        // passes every CPU test and reads downstream as a completely different
        // bug. EnumerateLayerDraws is the one place that decides.
        // See Render(): the MAIN view, not the shadow camera.
        const glm::vec3 renderRelativeViewPos =
            MakePositionRelative(CommandDispatch::GetViewPosition(), Renderer3D::GetRenderOrigin());

        TArray<LayerDraw> draws;
        for (auto& layer : m_Layers)
        {
            // A layer authored out of the shadow maps (FoliageLayer::CastShadows,
            // #1533) still receives; it only skips this pass.
            if (layer.InstanceCount == 0 || !layer.CastShadows)
                continue;

            EnumerateLayerDraws(layer, draws);
            const auto& shadowView = layer.CullViews[shadowSlot];
            u32 partIndex = 0;
            for (const auto& draw : draws)
            {
                const u32 part = partIndex++;
                const bool impostor = !draw.IsAuthoredMesh && layer.UseImpostor && layer.Impostor.IsValid();
                const auto& program = impostor ? (impostorDepthOverride ? impostorDepthOverride : m_ImpostorDepthShader) : depthShader;
                if (!program || !program->IsReady())
                    continue;
                program->Bind();
                if (afterProgramBind)
                    afterProgramBind();

                // Upload per-draw foliage UBO for depth pass
                ShaderBindingLayout::FoliageUBO foliageUBOData{};
                foliageUBOData.Time = m_Time;
                foliageUBOData.PrevTime = m_PrevTime;
                foliageUBOData.WindStrength = layer.WindStrength;
                foliageUBOData.WindSpeed = layer.WindSpeed;
                foliageUBOData.WindWeights = layer.WindWeights;
                const auto wind = WindSystem::GetGPUData();
                foliageUBOData.WindDirection = wind.DirectionAndSpeed;
                foliageUBOData.WindGust = wind.GustAndTurbulence;
                foliageUBOData.WindClock = wind.TimeAndFlags;
                foliageUBOData.PrevMeshViewPos = glm::vec4(MakePositionRelative(Renderer3D::GetPreviousViewPosition(), Renderer3D::GetRenderOrigin()), 0.0f);
                foliageUBOData.WindFlags = glm::vec4(Renderer3D::GetRenderOrigin(), wind.TimeAndFlags.y);
                foliageUBOData.WindHistoryValid = WindSystem::HasStableParameters() ? 1.0f : 0.0f;
                foliageUBOData.AlphaCutoff = layer.AlphaCutoff;
                foliageUBOData.MeshParams = glm::vec4(draw.IsAuthoredMesh ? 1.0f : 0.0f,
                                                      draw.HandoverStart, draw.HandoverEnd, draw.CardNormalLane);
                // The MAIN view's position, not this pass's camera — that one is
                // the light. Without it the shadow pass would pick the mesh where
                // the lit frame drew the card and the plant's shadow would be a
                // different shape than the plant (issue #1233, fourth criterion).
                foliageUBOData.MeshViewPos = glm::vec4(renderRelativeViewPos, 0.0f);
                // The #1237 lanes, from the draw the enumeration packed them on.
                foliageUBOData.LodTransition0 = draw.LodTransition0;
                foliageUBOData.LodTransition1 = draw.LodTransition1;
                // The SAME influence set the lit pass reads (issue #1238).
                // Without it a plant bends and its shadow does not, which reads
                // as a detached shadow rather than as a missing feature.
                ApplyFoliageInteraction(foliageUBOData, layer.InteractionResponse);
                if (impostor)
                {
                    foliageUBOData.ViewDistance = draw.ViewDistance;
                    foliageUBOData.FadeStart = draw.FadeStart;
                    foliageUBOData.ImpostorParams0 = glm::vec4(layer.Impostor.FramesPerAxis, layer.Impostor.Hemi ? 1.0f : 0.0f, layer.ImpostorStartDistance, layer.ImpostorTransitionBand);
                    foliageUBOData.ImpostorParams1 = glm::vec4(1.0f, layer.Impostor.Radius, 0.5f, 0.0f);
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE, layer.Impostor.Albedo->GetRHIHandle(), RHI::HeapSlotLifetime::Persistent);
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_SPECULAR, layer.Impostor.NormalDepth->GetRHIHandle(), RHI::HeapSlotLifetime::Persistent);
                }

                auto foliageUBO = Renderer3D::GetFoliageUBO();
                foliageUBO->SetData(&foliageUBOData, ShaderBindingLayout::FoliageUBO::GetSize());
                foliageUBO->Bind();

                // Bind albedo for alpha test in shadow pass (see the seam note above).
                if (!impostor)
                {
                    const auto albedo = draw.Albedo ? draw.Albedo : Renderer3D::GetWhiteTexture();
                    if (!albedo || !albedo->IsLoaded())
                        continue;
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE,
                                                     albedo->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);
                }

                // The compacted stream when this cascade culled, the full one
                // otherwise. Both are correct frames; only the second costs the
                // vertex work of every plant on the island.
                const Ref<VertexArray>& culledVAO = draw.IsAuthoredMesh ? shadowView.MeshVAO : shadowView.CardVAO;
                const bool indirect = shadowCulled && shadowView.Active && culledVAO &&
                                      shadowView.Resources.DrawArgs && part < shadowView.Resources.PartCount;

                const Ref<VertexArray>& boundVAO = indirect ? culledVAO : draw.VAO;
                // BindVertexArrayRaw, not VertexArray::Bind(): the latter is a
                // deliberate NO-OP on Vulkan (geometry reaches the shader by
                // device address, and the VAO-shaped state lives in the draw
                // path's tracker), so an indirect draw after it would take
                // whatever array was bound last. That showed up as
                // "'Foliage_Depth' STORAGE binding 63 has no published occupant"
                // -- the instance stream missing from a draw whose own array
                // carries one. The non-indirect call below passes the handle
                // explicitly and never had the problem; binding both the same
                // way is what keeps that from being a difference to remember.
                RenderCommand::BindVertexArrayRaw(boundVAO->GetRHIHandle());
                HeapBinding::FlushOffsets();
                if (indirect)
                {
                    RenderCommand::DrawBoundElementsIndirect(shadowView.Resources.DrawArgs->GetRHIHandle(),
                                                             RHI::PrimitiveTopology::TriangleList,
                                                             FoliageGPUCuller::DrawArgsOffset(part));
                }
                else
                {
                    RenderCommand::DrawIndexedInstancedRaw(boundVAO->GetRHIHandle(), draw.IndexCount,
                                                           draw.BaseIndex, layer.InstanceCount);
                }
            }
        }
    }

    u32 FoliageRenderer::GetTotalInstanceCount() const
    {
        u32 total = 0;
        for (const auto& layer : m_Layers)
        {
            total += layer.InstanceCount;
        }
        return total;
    }

    std::span<const FoliageAlphaCoverage::Entry> FoliageRenderer::GetAlphaCoverage(u32 layerIndex) const
    {
        if (layerIndex >= static_cast<u32>(m_Layers.Num()))
            return {};
        const auto& coverage = m_Layers[static_cast<i32>(layerIndex)].AlphaCoverage;
        return { coverage.GetData(), static_cast<sizet>(coverage.Num()) };
    }

    void FoliageRenderer::UpdateAlphaCoverage(LayerRenderData& data, const FoliageLayer& layer, bool meshDrawn,
                                              bool impostorDrawn)
    {
        namespace AC = FoliageAlphaCoverage;

        if (data.AlphaCoverageMeshDrawn != meshDrawn || data.AlphaCoverageImpostorDrawn != impostorDrawn)
            data.AlphaCoverageDirty = true;

        // Re-measured only when a texture, a surface or the set of drawn
        // representations moved. A cutoff change does not get here: the
        // histograms already answer every cutoff.
        if (data.AlphaCoverageDirty)
        {
            OLO_PROFILE_SCOPE("FoliageRenderer::UpdateAlphaCoverage - measure");
            data.AlphaCoverageDirty = false;
            data.AlphaCoverageMeshDrawn = meshDrawn;
            data.AlphaCoverageImpostorDrawn = impostorDrawn;
            data.AlphaCoverage.Reset();

            // Each texture's alpha, decoded ONCE per pass from the file it was
            // loaded from: a plant's parts often share one atlas, and a part
            // with no texture of its own draws the layer albedo. One that
            // loaded on the GPU but will not decode on the CPU (a cooked
            // block-compressed container) is recorded as NOT measured and said
            // so — never read as "fine".
            std::unordered_map<std::string, AC::AlphaPlane> decoded;
            const auto planeFor = [&decoded, &layer](std::string_view texturePath) -> const AC::AlphaPlane*
            {
                auto [it, inserted] = decoded.try_emplace(std::string(texturePath));
                if (inserted)
                {
                    std::string error;
                    const std::filesystem::path path = Texture2D::ResolveStoredSourcePath(texturePath);
                    if (path.empty())
                        error = "its source path does not resolve";
                    if (path.empty() || !AC::DecodeAlpha(path, it->second, error))
                    {
                        OLO_CORE_INFO("FoliageRenderer: layer '{}' - alpha coverage of '{}' was not measured ({}), "
                                      "so the AlphaCutoff plausibility check cannot vouch for it.",
                                      layer.Name.ToView(), texturePath, error);
                    }
                }
                return it->second.IsEmpty() ? nullptr : &it->second;
            };

            // A texture that did not load at all is not measured: Texture2D
            // already logged the failure, and the bake and the shadow pass
            // treat it as white, which passes everywhere.
            const auto drawable = [](const Ref<Texture2D>& texture)
            { return texture && texture->IsLoaded(); };
            const Ref<Texture2D>& albedo = data.AlbedoTexture;
            const std::string meshName = std::filesystem::path(layer.MeshPath.ToStdString()).filename().string();

            // The flat card — unless the impostor rides the card draw in its
            // place, in which case the layer albedo is never drawn flat.
            if (drawable(albedo) && !impostorDrawn)
            {
                AC::Entry entry;
                entry.Kind = AC::Role::Card;
                entry.Texture = layer.AlbedoPath;
                entry.Surface = "the card";
                if (const AC::AlphaPlane* plane = planeFor(albedo->GetPath()))
                {
                    entry.Coverage = AC::MeasureSheet(*plane);
                    entry.Measured = true;
                }
                data.AlphaCoverage.Add(std::move(entry));
            }

            // One part of a mesh, over its own surface samples.
            const auto measurePart = [&](AC::Role kind, i32 index, std::string_view texturePath,
                                         const TArray<TArray<glm::vec2>>& surfaces, const AC::Histogram* preparedCoverage = nullptr)
            {
                if (texturePath.empty())
                    return; // white: passes everywhere
                AC::Entry entry;
                entry.Kind = kind;
                entry.Texture = FString(texturePath);
                entry.Surface = FString(std::format("part {} of '{}'", index, meshName));
                const bool hasSurface = index < surfaces.Num() && !surfaces[index].IsEmpty();
                // Live adoption of old eager geometry may have no prepared
                // histogram. Leave that diagnostic explicitly unmeasured;
                // optional source files never bypass bounded preparation here.
                const bool mayDecode = !m_StreamingEnabled || (albedo && texturePath == albedo->GetPath());
                const AC::AlphaPlane* plane = (preparedCoverage && !preparedCoverage->IsEmpty()) || !hasSurface || !mayDecode ? nullptr : planeFor(texturePath);
                if (preparedCoverage && !preparedCoverage->IsEmpty())
                {
                    entry.Coverage = *preparedCoverage;
                    entry.Measured = true;
                }
                else if (plane && index < surfaces.Num() && !surfaces[index].IsEmpty())
                {
                    const auto& uvs = surfaces[index];
                    entry.Coverage = AC::MeasureAtUVs(*plane, { uvs.GetData(), static_cast<sizet>(uvs.Num()) });
                    entry.Measured = true;
                }
                data.AlphaCoverage.Add(std::move(entry));
            };

            // The near mesh, with the texture EnumerateLayerDraws binds for
            // each part: its own, else the layer albedo.
            if (meshDrawn)
            {
                for (i32 i = 0; i < data.MeshParts.Num(); ++i)
                {
                    const auto& part = data.MeshParts[i];
                    const Ref<Texture2D>& drawn = part.Albedo ? part.Albedo : albedo;
                    const std::string_view sourcePath = !part.AlbedoSourcePath.IsEmpty() ? part.AlbedoSourcePath.ToView() : (drawable(drawn) ? drawn->GetPath() : std::string_view{});
                    measurePart(AC::Role::AuthoredMesh, i, sourcePath,
                                data.MeshPartSurfaceUVs, &part.SampledAlphaCoverage);
                }
            }

            // The impostor bake, with the textures it was baked with. Only when
            // the near mesh is NOT drawn: the bake draws exactly the near mesh's
            // parts and textures, so with both drawn these entries would repeat
            // the ones above number for number.
            if (impostorDrawn && !meshDrawn)
            {
                for (i32 i = 0; i < data.ImpostorPartTextures.Num(); ++i)
                    measurePart(AC::Role::ImpostorBake, i, data.ImpostorPartTextures[i].ToView(),
                                data.ImpostorPartSurfaceUVs, i < data.ImpostorPartCoverage.Num() ? &data.ImpostorPartCoverage[i] : nullptr);
            }
        }

        // Judged on every regeneration at the CURRENT cutoff. The latch is
        // what keeps a slider dragged through an implausible range to one
        // line; it re-arms once the entry is plausible again, so dragging back
        // in warns again, and a re-measure starts every entry fresh.
        for (auto& entry : data.AlphaCoverage)
        {
            const std::string warning = AC::Describe(entry, layer.Name.ToView(), layer.AlphaCutoff);
            if (warning.empty())
            {
                entry.Warned = false;
                continue;
            }
            if (entry.Warned)
                continue;
            entry.Warned = true;
            OLO_CORE_WARN("{}", warning);
        }
    }

    void FoliageRenderer::UpdateImpostorAtlas(LayerRenderData& data, const FoliageLayer& layer)
    {
        OLO_PROFILE_FUNCTION();

        if (!layer.UseImpostor || layer.MeshPath.IsEmpty())
        {
            // Impostor turned off (or no mesh) — drop any stale atlas. Free
            // its VRAM budget claim (issue #718) before discarding it.
            ImpostorBaker::Free(data.Impostor);
            data.Impostor = ImpostorAtlas{};
            data.ImpostorBakedMeshPath.Empty();
            data.ImpostorBakedFrames = 0;
            data.ImpostorBakedResolution = 0;
            if (!data.ImpostorPartTextures.IsEmpty())
            {
                data.ImpostorPartTextures.Reset();
                data.ImpostorPartSurfaceUVs.Reset();
                data.ImpostorPartCoverage.Reset();
                data.AlphaCoverageDirty = true;
            }
            return;
        }

        const bool streamingGeometry = m_StreamingEnabled && layer.UseAuthoredMesh;
        if (streamingGeometry)
        {
            // CPU preparation is not GPU residency. Bake only this layer's
            // admitted geometry, including during later material/layout edits;
            // a sibling's ready payload for the same path cannot supply it.
            if (!data.MeshVBO || !data.MeshIBO || data.MeshParts.IsEmpty() || data.MeshGeometryPath != layer.MeshPath)
                return; // retain the pinned atlas/card while optional geometry is absent
            const f32 cutoff = AlphaCoverageMips::SanitizeCutoff(layer.AlphaCutoff);
            for (const auto& part : data.MeshParts)
                if (part.Albedo && !Math::BitwiseEqual(part.Albedo->GetAlphaCoverageCutoff(), cutoff))
                    return; // the admitted material mip update must finish before recording a new bake
        }

        // Re-bake only when anything the atlas is baked FROM changed: mesh, grid,
        // atlas resolution, layout, AND the material inputs (albedo texture path,
        // tint, alpha cutoff) — the bake bakes those in, so a tint/cutoff change
        // with the same mesh must still re-bake or the atlas goes stale.
        const bool upToDate = data.Impostor.IsValid() && data.ImpostorBakedMeshPath == layer.MeshPath && data.ImpostorBakedFrames == layer.ImpostorFramesPerAxis && data.ImpostorBakedResolution == layer.ImpostorAtlasResolution && data.ImpostorBakedHemi == layer.ImpostorHemiOctahedral && data.ImpostorBakedAlbedoPath == layer.AlbedoPath && Math::BitwiseEqual(data.ImpostorBakedBaseColor, layer.BaseColor) && Math::BitwiseEqual(data.ImpostorBakedAlphaCutoff, layer.AlphaCutoff);
        if (upToDate)
            return;

        // Reuse the copy the authored-mesh path already imported for this exact
        // path (issue #1233) rather than parsing the file a second time — the
        // bake and the near geometry are framed from the SAME source, which is
        // also what keeps the impostor card and the mesh the same tree.
        Ref<Model> owned;
        PlantGeometry plant;
        if (streamingGeometry)
        {
            plant.Parts = data.MeshParts;
            plant.PartSurfaceUVs = data.MeshPartSurfaceUVs;
            plant.Box = data.MeshBounds;
        }
        else if (!data.MeshModel || data.MeshGeometryPath != layer.MeshPath)
        {
            const std::filesystem::path meshFile = ResolveContentPath(layer.MeshPath.ToView());
            if (meshFile.empty())
            {
                OLO_CORE_WARN("FoliageRenderer: impostor layer '{}' mesh '{}' does not resolve — impostor disabled "
                              "for this layer",
                              layer.Name.ToView(), layer.MeshPath.ToView());
                ImpostorBaker::Free(data.Impostor);
                data.Impostor = ImpostorAtlas{};
                if (!data.ImpostorPartTextures.IsEmpty())
                {
                    data.ImpostorPartTextures.Reset();
                    data.ImpostorPartSurfaceUVs.Reset();
                    data.ImpostorPartCoverage.Reset();
                    data.AlphaCoverageDirty = true;
                }
                return;
            }
            owned = Ref<Model>::Create(meshFile.generic_string());
        }
        // The coverage diagnostic re-measures only when what is baked changes.
        // A bake that fails, or bakes nothing, leaves nothing to judge.
        const auto dropBakedParts = [&data]()
        {
            if (data.ImpostorPartTextures.IsEmpty())
                return;
            data.ImpostorPartTextures.Reset();
            data.ImpostorPartSurfaceUVs.Reset();
            data.ImpostorPartCoverage.Reset();
            data.AlphaCoverageDirty = true;
        };
        const Model* model = owned ? owned.Raw() : data.MeshModel.Raw();
        const char* failure = streamingGeometry ? nullptr : (!model || model->GetMeshCount() == 0 ? "it failed to load" : ExtractPlantGeometry(*model, plant));
        if (failure)
        {
            OLO_CORE_WARN("FoliageRenderer: impostor layer '{}' mesh '{}': {} — impostor disabled for this layer",
                          layer.Name.ToView(), layer.MeshPath.ToView(), failure);
            ImpostorBaker::Free(data.Impostor);
            data.Impostor = ImpostorAtlas{};
            dropBakedParts();
            return;
        }

        // Bake the plant the near mesh draws: every part, each with its OWN
        // material albedo and the layer albedo only where the mesh has none —
        // the rule EnumerateLayerDraws applies — tinted by BaseColor. Through a
        // private, uninstanced vertex array: the layer's MeshVAO carries the
        // instance stream, and an impostor-only layer never builds one. A
        // texture that failed to load bakes as white rather than as whatever
        // the failed texture object samples to.
        TArray<ImpostorBakePart> parts;
        parts.Reserve(plant.Parts.Num());
        for (const auto& part : plant.Parts)
        {
            const Ref<Texture2D>& albedo = part.Albedo ? part.Albedo : data.AlbedoTexture;
            parts.Add(ImpostorBakePart{ part.BaseIndex, part.IndexCount,
                                        albedo && albedo->IsLoaded() ? albedo : Ref<Texture2D>{} });
        }

        Ref<VertexBuffer> bakeVBO = data.MeshVBO;
        Ref<IndexBuffer> bakeIBO = data.MeshIBO;
        if (!streamingGeometry)
        {
            const auto& vertices = plant.Source->GetVertices();
            bakeVBO = VertexBuffer::Create(vertices.GetData(), static_cast<u32>(vertices.Num() * sizeof(Vertex)));
            bakeIBO = IndexBuffer::Create(plant.Indices.GetData(), static_cast<u32>(plant.Indices.Num()));
        }
        bakeVBO->SetLayout(Vertex::GetLayout());
        Ref<VertexArray> bakeVAO = VertexArray::Create();
        bakeVAO->AddVertexBuffer(bakeVBO);
        bakeVAO->SetIndexBuffer(bakeIBO);

        // Free the OUTGOING atlas's budget claim first — Bake() below reserves
        // a fresh one, and freeing after would either double-count briefly or,
        // worse, free the NEW claim if the assignment races the wrong way.
        ImpostorBaker::Free(data.Impostor);
        data.Impostor = ImpostorBaker::Bake(
            bakeVAO, plant.Box, std::span<const ImpostorBakePart>(parts.GetData(), static_cast<sizet>(parts.Num())),
            layer.BaseColor, layer.ImpostorFramesPerAxis, layer.ImpostorAtlasResolution,
            layer.ImpostorHemiOctahedral, layer.AlphaCutoff);

        if (!data.Impostor.IsValid())
        {
            dropBakedParts();
            return;
        }

        // What the atlas was baked from, for the alpha-coverage diagnostic.
        // Replaced — and the entries re-measured — only when the parts or
        // their textures changed. A cutoff or tint re-bake bakes the same
        // parts, and re-measuring would re-arm every warning latch, so a
        // slider dragged across an implausible range would warn on every
        // step. Kept and compared by texture PATH: an impostor-only layer
        // re-imports its model per bake, so its Texture2D objects are new each
        // time — and they are released with `parts` when this returns.
        TArray<FString> textures;
        textures.Reserve(parts.Num());
        for (i32 i = 0; i < parts.Num(); ++i)
            textures.Add(!plant.Parts[i].AlbedoSourcePath.IsEmpty() ? plant.Parts[i].AlbedoSourcePath : (parts[i].Albedo ? FString(parts[i].Albedo->GetPath()) : FString{}));
        const bool sameParts = data.ImpostorBakedMeshPath == layer.MeshPath &&
                               data.ImpostorPartTextures == textures &&
                               data.ImpostorPartSurfaceUVs.Num() == plant.PartSurfaceUVs.Num();
        if (!sameParts)
        {
            data.ImpostorPartTextures = std::move(textures);
            data.ImpostorPartSurfaceUVs = std::move(plant.PartSurfaceUVs);
            data.AlphaCoverageDirty = true;
        }
        data.ImpostorPartCoverage.Reset();
        for (const auto& part : plant.Parts)
            data.ImpostorPartCoverage.Add(part.SampledAlphaCoverage);

        data.ImpostorBakedMeshPath = layer.MeshPath;
        data.ImpostorBakedAlbedoPath = layer.AlbedoPath;
        data.ImpostorBakedBaseColor = layer.BaseColor;
        data.ImpostorBakedAlphaCutoff = layer.AlphaCutoff;
        data.ImpostorBakedFrames = layer.ImpostorFramesPerAxis;
        data.ImpostorBakedResolution = layer.ImpostorAtlasResolution;
        data.ImpostorBakedHemi = layer.ImpostorHemiOctahedral;
    }

    TArray<FoliageLayerDrawInfo> FoliageRenderer::GetActiveLayerDrawInfo() const
    {
        TArray<FoliageLayerDrawInfo> result;
        result.Reserve(m_Layers.Num());

        TArray<LayerDraw> draws;
        for (u32 layerIndex = 0; layerIndex < static_cast<u32>(m_Layers.Num()); ++layerIndex)
        {
            const auto& layer = m_Layers[layerIndex];
            if (layer.InstanceCount == 0)
            {
                continue;
            }

            // Same enumeration as Render / RenderShadows — see EnumerateLayerDraws.
            // Part i of the main view's indirect args block IS draw i of this
            // list, because CullForView built the args from this same call.
            EnumerateLayerDraws(layer, draws);
            const auto& mainView = layer.CullViews[static_cast<u32>(FoliageGPUCuller::ViewSlot::Main)];
            u32 partIndex = 0;
            for (const auto& draw : draws)
            {
                const u32 part = partIndex++;

                FoliageLayerDrawInfo info;
                info.LayerIndex = layerIndex;
                info.VertexArrayID = draw.VAO->GetRHIHandle();
                info.BaseIndex = draw.BaseIndex;
                info.IndexCount = draw.IndexCount;
                info.InstanceCount = layer.InstanceCount;

                // The compacted stream, when this frame's main-view cull
                // produced one. The vertex array is the slot's own — a draw
                // whose indirect command counts compacted instances but whose
                // array still streams the FULL buffer would draw the first N
                // generated plants rather than the N visible ones, which looks
                // almost right and is entirely wrong.
                if (mainView.Active)
                {
                    const Ref<VertexArray>& culledVAO = draw.IsAuthoredMesh ? mainView.MeshVAO : mainView.CardVAO;
                    if (culledVAO && mainView.Resources.DrawArgs && part < mainView.Resources.PartCount)
                    {
                        info.VertexArrayID = culledVAO->GetRHIHandle();
                        info.IndirectBufferID = mainView.Resources.DrawArgs->GetRHIHandle();
                        info.IndirectOffsetBytes = FoliageGPUCuller::DrawArgsOffset(part);
                    }
                }
                info.AlbedoTextureID = draw.Albedo ? draw.Albedo->GetRHIHandle() : RHI::NullResource;
                info.IsAuthoredMesh = draw.IsAuthoredMesh;
                info.MeshHandoverStartDistance = draw.HandoverStart;
                info.MeshHandoverEndDistance = draw.HandoverEnd;
                info.CardNormalLane = draw.CardNormalLane;
                info.ViewDistance = draw.ViewDistance;
                info.FadeStartDistance = draw.FadeStart;
                info.LodTransition0 = draw.LodTransition0;
                info.LodTransition1 = draw.LodTransition1;
                info.WindStrength = layer.WindStrength;
                info.WindSpeed = layer.WindSpeed;
                info.WindWeights = layer.WindWeights;
                info.InteractionResponse = layer.InteractionResponse;
                info.BaseColor = layer.BaseColor;
                info.AlphaCutoff = layer.AlphaCutoff;
                info.Bounds = layer.Bounds;

                // The leaf material (issue #1234), identical on every draw the
                // layer emits — see FoliageLayerDrawInfo for why that is the
                // point rather than a convenience.
                info.LeafNormalTextureID =
                    layer.LeafNormalTexture ? layer.LeafNormalTexture->GetRHIHandle() : RHI::NullResource;
                info.LeafRoughnessTextureID =
                    layer.LeafRoughnessTexture ? layer.LeafRoughnessTexture->GetRHIHandle() : RHI::NullResource;
                info.LeafThicknessTextureID =
                    layer.LeafThicknessTexture ? layer.LeafThicknessTexture->GetRHIHandle() : RHI::NullResource;
                info.LeafRoughness = layer.LeafRoughness;
                info.LeafNormalStrength = layer.LeafNormalStrength;
                info.LeafThickness = layer.LeafThickness;
                info.LeafTransmissionStrength = layer.LeafTransmissionStrength;
                info.LeafTransmissionColor = layer.LeafTransmissionColor;
                info.LeafTransmissionDistortion = layer.LeafTransmissionDistortion;
                info.LeafTransmissionPower = layer.LeafTransmissionPower;
                info.LeafTransmissionWrap = layer.LeafTransmissionWrap;
                info.LeafTransmissionAmbient = layer.LeafTransmissionAmbient;

                // Octahedral impostor (issue #433) — only when the atlas baked OK,
                // and only for the CARD draw: the impostor IS the far-field card,
                // so routing the near mesh through it would replace the geometry
                // this task exists to draw.
                if (!draw.IsAuthoredMesh && layer.UseImpostor && layer.Impostor.IsValid())
                {
                    info.UseImpostor = true;
                    info.ImpostorAlbedoAtlasID = layer.Impostor.Albedo->GetRHIHandle();
                    info.ImpostorNormalDepthAtlasID = layer.Impostor.NormalDepth->GetRHIHandle();
                    info.ImpostorFramesPerAxis = layer.Impostor.FramesPerAxis;
                    info.ImpostorHemi = layer.Impostor.Hemi;
                    info.ImpostorStartDistance = layer.ImpostorStartDistance;
                    info.ImpostorTransitionBand = layer.ImpostorTransitionBand;
                    info.ImpostorRadius = layer.Impostor.Radius;
                }

                result.Add(info);
            }
        }

        return result;
    }

    // ── GPU patch + instance culling (issue #1235) ────────────────────────────

    // The lever, not a private static: OLO_FOLIAGE_CPU_CULL is registered in
    // Core/DebugLevers.inl, which gets it an environment variable, a line in the
    // startup log and a LIVE setter through olo_debug_levers_set -- so the
    // dense-scene A/B can be measured inside one editor session instead of two.
    // Phrased as CPU-cull-on rather than GPU-cull-off because the shipped path
    // is the culled one and a lever should name the thing it turns ON.
    void FoliageRenderer::SetGPUCullingEnabled(bool enabled)
    {
        Levers::SetFoliageCpuCull(!enabled);
    }

    bool FoliageRenderer::IsGPUCullingEnabled()
    {
        return !Levers::FoliageCpuCull();
    }

    void FoliageRenderer::SetDebugCullCapacity(u32 entries)
    {
        m_Culler.SetDebugOutputCapacity(entries);
    }
    bool FoliageRenderer::ReadbackCull(u32 layerIndex, FoliageGPUCuller::ViewSlot slot,
                                       FoliageGPUCuller::Readback& out) const
    {
        out = {};
        if (layerIndex >= m_Layers.Num())
            return false;

        const auto& layer = m_Layers[layerIndex];
        const auto& view = layer.CullViews[static_cast<u32>(slot)];
        if (!view.Active)
            return false;

        return m_Culler.ReadbackResult(layer.CullLayer, view.Resources, out);
    }

    FoliageGPUCuller::ViewInputs FoliageRenderer::MakeCullInputs(const glm::mat4& worldViewProjection,
                                                                 const glm::vec3& mainViewWorldPosition) const
    {
        // Terrain-LOCAL, through the frame's render origin: the instance rows and
        // the group bounds are terrain-local (SetTerrainTransform's comment), and
        // evaluating there keeps the subtraction on small operands far from the
        // world origin (#429). The origin is the one the frame is actually
        // rendering with, not a recomputed guess, so the cull cannot disagree
        // with the draw about where things are.
        const glm::vec3 origin = Renderer3D::GetRenderOrigin();

        FoliageGPUCuller::ViewInputs inputs;
        inputs.ViewFrustum = Frustum(MakeObjectLocalViewProjection(worldViewProjection, m_TerrainTransform, origin));
        inputs.DistanceOrigin = MakeObjectLocalCameraPos(mainViewWorldPosition, m_TerrainTransform, origin);
        // Filled per layer by CullForView -- each layer fades out at its own
        // ViewDistance, and that authored number is the cutoff.
        inputs.MaxDistance = 0.0f;
        return inputs;
    }

    void FoliageRenderer::DispatchMainViewCulling(const glm::mat4& worldViewProjection,
                                                  const glm::vec3& viewWorldPosition)
    {
        OLO_PROFILE_FUNCTION();
        m_MainViewCulled = CullForView(static_cast<u32>(FoliageGPUCuller::ViewSlot::Main),
                                       MakeCullInputs(worldViewProjection, viewWorldPosition));
    }

    void FoliageRenderer::ResetShadowViewCulling()
    {
        for (auto& layer : m_Layers)
        {
            for (u32 i = 0; i < FoliageGPUCuller::kMaxShadowViews; ++i)
                layer.CullViews[FoliageGPUCuller::ShadowSlot(i)].Active = false;
        }
    }

    bool FoliageRenderer::DispatchShadowViewCulling(u32 shadowViewIndex, const FoliageGPUCuller::ViewInputs& cullInputs)
    {
        OLO_PROFILE_FUNCTION();

        if (shadowViewIndex >= FoliageGPUCuller::kMaxShadowViews)
        {
            // Bounded and named, not silent. The view still renders -- it just
            // draws every generated instance, which is what the pass did before
            // this feature existed.
            if (!m_WarnedShadowViewOverflow)
            {
                OLO_CORE_WARN("FoliageRenderer: shadow view {} is past the {} that get their own GPU cull — it "
                              "draws every generated instance instead. Further views not logged.",
                              shadowViewIndex, FoliageGPUCuller::kMaxShadowViews);
                m_WarnedShadowViewOverflow = true;
            }
            return false;
        }
        return CullForView(FoliageGPUCuller::ShadowSlot(shadowViewIndex), cullInputs);
    }

    bool FoliageRenderer::CullForView(u32 slotIndex, const FoliageGPUCuller::ViewInputs& inputs)
    {
        OLO_PROFILE_FUNCTION();

        // Cleared FIRST and unconditionally. A slot that is not re-culled this
        // frame must not keep claiming last frame's compacted set -- that is the
        // latched-flag failure TerrainGPUQuadtree::HasDispatched documents, and
        // here it would draw the plants that were visible from a camera position
        // the viewer has already left.
        for (auto& layer : m_Layers)
            layer.CullViews[slotIndex].Active = false;

        if (!IsGPUCullingEnabled())
            return false;

        m_Culler.EnsureInitialised();
        if (!m_Culler.IsAvailable())
        {
            if (!m_WarnedCullUnavailable)
            {
                OLO_CORE_ERROR("FoliageRenderer: GPU culling is unavailable — every generated instance is "
                               "submitted instead. The frame is correct and slower, not empty.");
                m_WarnedCullUnavailable = true;
            }
            return false;
        }

        bool any = false;
        TArray<LayerDraw> draws;
        TArray<FoliageGPUCuller::Part> parts;

        for (u32 layerIndex = 0; layerIndex < static_cast<u32>(m_Layers.Num()); ++layerIndex)
        {
            auto& layer = m_Layers[layerIndex];
            if (layer.InstanceCount == 0 || !layer.InstanceVBO)
                continue;

            if (!m_Culler.BuildLayer(layer.CullLayer, m_Registry, layerIndex, layer.InstanceCount,
                                     layer.BoundsProfile))
                continue;

            // THE list, the same one the beauty and shadow paths walk. Part i of
            // the indirect args block is draw i here, so the two cannot disagree
            // about which index range a command describes.
            EnumerateLayerDraws(layer, draws);
            if (draws.IsEmpty())
                continue;
            if (draws.Num() > FoliageGPUCuller::kMaxParts)
            {
                // Latched. This runs per layer per view slot per frame, so an
                // unlatched warning about a condition that does not change is
                // five identical lines every frame forever.
                if (!m_WarnedTooManyParts)
                {
                    OLO_CORE_WARN("FoliageRenderer: layer {} emits {} draws, more than the {} an indirect args "
                                  "block holds — this layer keeps the uncompacted path. Further layers not "
                                  "logged.",
                                  layerIndex, draws.Num(), FoliageGPUCuller::kMaxParts);
                    m_WarnedTooManyParts = true;
                }
                continue;
            }

            parts.Reset();
            parts.Reserve(draws.Num());
            bool hasMeshPart = false;
            for (const auto& draw : draws)
            {
                parts.Add(FoliageGPUCuller::Part{ draw.IndexCount, draw.BaseIndex, draw.IsAuthoredMesh });
                hasMeshPart = hasMeshPart || draw.IsAuthoredMesh;
            }

            // Room for the mesh region beside the all-survivor list (#1533):
            // the region starts at the instance count, so the buffer holds two.
            auto& view = layer.CullViews[slotIndex];
            const u32 capacityWanted = hasMeshPart ? layer.InstanceCount * 2u : layer.InstanceCount;
            const bool recreated =
                m_Culler.EnsureViewCapacity(view.Resources, capacityWanted, layer.CullLayer.GroupCount);
            if (recreated || (!view.CardVAO && !view.MeshVAO))
                RebuildCulledVertexArrays(layer, slotIndex);

            FoliageGPUCuller::ViewInputs layerInputs = inputs;
            layerInputs.MaxDistance = layer.ViewDistance;

            // Only the MAIN view publishes the ratio counters — see the
            // s_EmitStats comment in FoliageCullCommon.glsl.
            const bool emitStats = slotIndex == static_cast<u32>(FoliageGPUCuller::ViewSlot::Main);
            // The SAME packed lanes every one of this layer's draws carries
            // (issue #1237), so the main view and all four cascades thin the
            // same plants — a cascade that kept a thinned-out plant would cast
            // a shadow with nothing above it.
            FoliageGPUCuller::LodInputs lodInputs;
            lodInputs.Transition0 = FoliageLodTransition0(layer.Lod);
            lodInputs.Transition1 = FoliageLodTransition1(layer.Lod);
            // Every draw of a layer carries the same hand-over band.
            if (hasMeshPart)
                lodInputs.MeshReach = FoliageLod::MeshRegionReach(draws[0].HandoverStart, draws[0].HandoverEnd,
                                                                  layer.Lod.TransitionSpread, layer.Lod.Hysteresis);
            if (m_Culler.Cull(layer.CullLayer, view.Resources, layer.InstanceVBO->GetRHIHandle(), std::span(parts.GetData(), static_cast<sizet>(parts.Num())), layerInputs,
                              emitStats, lodInputs))
            {
                view.Active = true;
                any = true;
            }
        }

        return any;
    }
} // namespace OloEngine
