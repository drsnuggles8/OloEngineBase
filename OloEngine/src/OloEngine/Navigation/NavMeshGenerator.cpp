#include "OloEnginePCH.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/MeshColliderAsset.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Core/Log.h"

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    static glm::mat4 GetWorldTransform(Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        auto& tc = entity.GetComponent<TransformComponent>();
        return glm::translate(glm::mat4(1.0f), tc.Translation) * glm::toMat4(tc.GetRotation()) * glm::scale(glm::mat4(1.0f), tc.Scale);
    }

    // Append local-space vertices transformed to world-space and return the baseVertex index
    static i32 AppendTransformedVerts(const glm::mat4& worldTransform,
                                      const glm::vec3* localVerts, i32 count,
                                      std::vector<f32>& outVerts)
    {
        OLO_PROFILE_FUNCTION();

        const auto baseVertex = static_cast<i32>(outVerts.size() / 3);
        for (i32 i = 0; i < count; ++i)
        {
            glm::vec4 wp = worldTransform * glm::vec4(localVerts[i], 1.0f);
            outVerts.push_back(wp.x);
            outVerts.push_back(wp.y);
            outVerts.push_back(wp.z);
        }
        return baseVertex;
    }

    // Generate a UV-sphere approximation with given rings/segments
    static void GenerateSphereGeometry(f32 radius, const glm::vec3& offset,
                                       i32 rings, i32 segments,
                                       std::vector<glm::vec3>& outLocalVerts,
                                       std::vector<i32>& outLocalTris)
    {
        OLO_PROFILE_FUNCTION();

        outLocalVerts.clear();
        outLocalTris.clear();

        // Top pole
        outLocalVerts.push_back(offset + glm::vec3(0.0f, radius, 0.0f));

        for (i32 r = 1; r < rings; ++r)
        {
            f32 phi = glm::pi<f32>() * static_cast<f32>(r) / static_cast<f32>(rings);
            f32 y = radius * std::cos(phi);
            f32 ringRadius = radius * std::sin(phi);
            for (i32 s = 0; s < segments; ++s)
            {
                f32 theta = 2.0f * glm::pi<f32>() * static_cast<f32>(s) / static_cast<f32>(segments);
                outLocalVerts.push_back(offset + glm::vec3(ringRadius * std::cos(theta), y, ringRadius * std::sin(theta)));
            }
        }

        // Bottom pole
        outLocalVerts.push_back(offset + glm::vec3(0.0f, -radius, 0.0f));

        i32 bottomPole = static_cast<i32>(outLocalVerts.size()) - 1;

        // Top cap triangles
        for (i32 s = 0; s < segments; ++s)
        {
            outLocalTris.push_back(0);
            outLocalTris.push_back(1 + (s + 1) % segments);
            outLocalTris.push_back(1 + s);
        }

        // Middle quads (2 tris each)
        for (i32 r = 0; r < rings - 2; ++r)
        {
            i32 rowStart = 1 + r * segments;
            i32 nextRowStart = 1 + (r + 1) * segments;
            for (i32 s = 0; s < segments; ++s)
            {
                i32 s1 = (s + 1) % segments;
                outLocalTris.push_back(rowStart + s);
                outLocalTris.push_back(rowStart + s1);
                outLocalTris.push_back(nextRowStart + s);

                outLocalTris.push_back(nextRowStart + s);
                outLocalTris.push_back(rowStart + s1);
                outLocalTris.push_back(nextRowStart + s1);
            }
        }

        // Bottom cap triangles
        i32 lastRowStart = 1 + (rings - 2) * segments;
        for (i32 s = 0; s < segments; ++s)
        {
            outLocalTris.push_back(bottomPole);
            outLocalTris.push_back(lastRowStart + s);
            outLocalTris.push_back(lastRowStart + (s + 1) % segments);
        }
    }

    void NavMeshGenerator::CollectSceneGeometry(Scene* scene, std::vector<f32>& outVerts, std::vector<i32>& outTris)
    {
        OLO_PROFILE_FUNCTION();
        // Collect from MeshComponent entities
        auto meshView = scene->GetAllEntitiesWith<MeshComponent, TransformComponent>();
        for (auto e : meshView)
        {
            Entity entity = { e, scene };
            auto& meshComp = entity.GetComponent<MeshComponent>();
            if (!meshComp.m_MeshSource)
                continue;

            glm::mat4 worldTransform = GetWorldTransform(entity);
            const auto& vertices = meshComp.m_MeshSource->GetVertices();
            const auto& indices = meshComp.m_MeshSource->GetIndices();

            const auto baseVertex = static_cast<i32>(outVerts.size() / 3);

            for (i32 i = 0; i < static_cast<i32>(vertices.Num()); ++i)
            {
                glm::vec4 worldPos = worldTransform * glm::vec4(vertices[i].Position, 1.0f);
                outVerts.push_back(worldPos.x);
                outVerts.push_back(worldPos.y);
                outVerts.push_back(worldPos.z);
            }

            const auto vertexCount = static_cast<i32>(vertices.Num());
            for (i32 i = 0; i + 2 < static_cast<i32>(indices.Num()); i += 3)
            {
                i32 i0 = static_cast<i32>(indices[i]);
                i32 i1 = static_cast<i32>(indices[i + 1]);
                i32 i2 = static_cast<i32>(indices[i + 2]);
                if (i0 < 0 || i0 >= vertexCount || i1 < 0 || i1 >= vertexCount || i2 < 0 || i2 >= vertexCount)
                    continue;
                outTris.push_back(baseVertex + i0);
                outTris.push_back(baseVertex + i1);
                outTris.push_back(baseVertex + i2);
            }
        }

        // Collect from ModelComponent entities
        auto modelView = scene->GetAllEntitiesWith<ModelComponent, TransformComponent>();
        for (auto e : modelView)
        {
            Entity entity = { e, scene };
            auto& modelComp = entity.GetComponent<ModelComponent>();
            if (!modelComp.m_Model)
                continue;

            glm::mat4 worldTransform = GetWorldTransform(entity);

            for (const auto& mesh : modelComp.m_Model->GetMeshes())
            {
                if (!mesh)
                    continue;

                auto meshSource = mesh->GetMeshSource();
                if (!meshSource)
                    continue;

                const auto& vertices = meshSource->GetVertices();
                const auto& indices = meshSource->GetIndices();

                const auto baseVertex = static_cast<i32>(outVerts.size() / 3);

                for (i32 i = 0; i < static_cast<i32>(vertices.Num()); ++i)
                {
                    glm::vec4 worldPos = worldTransform * glm::vec4(vertices[i].Position, 1.0f);
                    outVerts.push_back(worldPos.x);
                    outVerts.push_back(worldPos.y);
                    outVerts.push_back(worldPos.z);
                }

                const auto vertexCount = static_cast<i32>(vertices.Num());
                for (i32 i = 0; i + 2 < static_cast<i32>(indices.Num()); i += 3)
                {
                    i32 i0 = static_cast<i32>(indices[i]);
                    i32 i1 = static_cast<i32>(indices[i + 1]);
                    i32 i2 = static_cast<i32>(indices[i + 2]);
                    if (i0 < 0 || i0 >= vertexCount || i1 < 0 || i1 >= vertexCount || i2 < 0 || i2 >= vertexCount)
                        continue;
                    outTris.push_back(baseVertex + i0);
                    outTris.push_back(baseVertex + i1);
                    outTris.push_back(baseVertex + i2);
                }
            }
        }

        // Collect from BoxCollider3DComponent (approximate as 12 triangles)
        auto boxView = scene->GetAllEntitiesWith<BoxCollider3DComponent, TransformComponent>();
        for (auto e : boxView)
        {
            Entity entity = { e, scene };
            auto& box = entity.GetComponent<BoxCollider3DComponent>();

            glm::mat4 worldTransform = GetWorldTransform(entity);
            glm::vec3 halfSize = box.m_HalfExtents;

            // 8 local corners of the box (around offset)
            glm::vec3 localCorners[8] = {
                box.m_Offset + glm::vec3(-halfSize.x, -halfSize.y, -halfSize.z),
                box.m_Offset + glm::vec3(halfSize.x, -halfSize.y, -halfSize.z),
                box.m_Offset + glm::vec3(halfSize.x, halfSize.y, -halfSize.z),
                box.m_Offset + glm::vec3(-halfSize.x, halfSize.y, -halfSize.z),
                box.m_Offset + glm::vec3(-halfSize.x, -halfSize.y, halfSize.z),
                box.m_Offset + glm::vec3(halfSize.x, -halfSize.y, halfSize.z),
                box.m_Offset + glm::vec3(halfSize.x, halfSize.y, halfSize.z),
                box.m_Offset + glm::vec3(-halfSize.x, halfSize.y, halfSize.z),
            };

            const auto baseVertex = static_cast<i32>(outVerts.size() / 3);
            for (auto& corner : localCorners)
            {
                glm::vec4 worldPos = worldTransform * glm::vec4(corner, 1.0f);
                outVerts.push_back(worldPos.x);
                outVerts.push_back(worldPos.y);
                outVerts.push_back(worldPos.z);
            }

            // 12 triangles (2 per face)
            static constexpr i32 boxIndices[] = {
                0, 1, 2, 0, 2, 3, // front
                4, 6, 5, 4, 7, 6, // back
                0, 4, 5, 0, 5, 1, // bottom
                2, 6, 7, 2, 7, 3, // top
                0, 7, 4, 0, 3, 7, // left
                1, 5, 6, 1, 6, 2  // right
            };
            for (i32 idx : boxIndices)
                outTris.push_back(baseVertex + idx);
        }

        // Collect from SphereCollider3DComponent (UV-sphere approximation)
        {
            constexpr i32 kRings = 8;
            constexpr i32 kSegments = 12;
            std::vector<glm::vec3> localVerts;
            std::vector<i32> localTris;

            auto sphereView = scene->GetAllEntitiesWith<SphereCollider3DComponent, TransformComponent>();
            for (auto e : sphereView)
            {
                Entity entity = { e, scene };
                auto& sphere = entity.GetComponent<SphereCollider3DComponent>();

                GenerateSphereGeometry(sphere.m_Radius, sphere.m_Offset, kRings, kSegments, localVerts, localTris);

                glm::mat4 worldTransform = GetWorldTransform(entity);
                const auto base = AppendTransformedVerts(worldTransform, localVerts.data(), static_cast<i32>(localVerts.size()), outVerts);

                for (i32 idx : localTris)
                    outTris.push_back(base + idx);
            }
        }

        // Collect from CapsuleCollider3DComponent (cylinder + hemisphere caps)
        {
            constexpr i32 kRings = 5;
            constexpr i32 kSegments = 12;
            std::vector<glm::vec3> localVerts;
            std::vector<i32> localTris;

            auto capsuleView = scene->GetAllEntitiesWith<CapsuleCollider3DComponent, TransformComponent>();
            for (auto e : capsuleView)
            {
                Entity entity = { e, scene };
                auto& capsule = entity.GetComponent<CapsuleCollider3DComponent>();

                localVerts.clear();
                localTris.clear();

                f32 r = capsule.m_Radius;
                f32 hh = capsule.m_HalfHeight;
                glm::vec3 off = capsule.m_Offset;

                // Top hemisphere (rings from pole down to equator at +hh)
                localVerts.push_back(off + glm::vec3(0.0f, hh + r, 0.0f)); // top pole
                for (i32 ring = 1; ring <= kRings; ++ring)
                {
                    f32 phi = (glm::pi<f32>() * 0.5f) * static_cast<f32>(ring) / static_cast<f32>(kRings);
                    f32 y = hh + r * std::cos(phi);
                    f32 ringR = r * std::sin(phi);
                    for (i32 s = 0; s < kSegments; ++s)
                    {
                        f32 theta = 2.0f * glm::pi<f32>() * static_cast<f32>(s) / static_cast<f32>(kSegments);
                        localVerts.push_back(off + glm::vec3(ringR * std::cos(theta), y, ringR * std::sin(theta)));
                    }
                }

                // Bottom hemisphere (equator at -hh, then rings down to pole)
                for (i32 ring = 0; ring < kRings; ++ring)
                {
                    f32 phi = (glm::pi<f32>() * 0.5f) * static_cast<f32>(ring) / static_cast<f32>(kRings);
                    f32 y = -hh - r * std::sin(phi);
                    f32 ringR = r * std::cos(phi);
                    for (i32 s = 0; s < kSegments; ++s)
                    {
                        f32 theta = 2.0f * glm::pi<f32>() * static_cast<f32>(s) / static_cast<f32>(kSegments);
                        localVerts.push_back(off + glm::vec3(ringR * std::cos(theta), y, ringR * std::sin(theta)));
                    }
                }
                localVerts.push_back(off + glm::vec3(0.0f, -hh - r, 0.0f)); // bottom pole

                i32 totalRings = 2 * kRings; // top hemisphere + bottom hemisphere (incl. equator at -hh)
                i32 bottomPole = static_cast<i32>(localVerts.size()) - 1;

                // Top cap
                for (i32 s = 0; s < kSegments; ++s)
                {
                    localTris.push_back(0);
                    localTris.push_back(1 + (s + 1) % kSegments);
                    localTris.push_back(1 + s);
                }

                // Middle rings
                for (i32 ring = 0; ring < totalRings - 1; ++ring)
                {
                    i32 rowStart = 1 + ring * kSegments;
                    i32 nextRow = 1 + (ring + 1) * kSegments;
                    for (i32 s = 0; s < kSegments; ++s)
                    {
                        i32 s1 = (s + 1) % kSegments;
                        localTris.push_back(rowStart + s);
                        localTris.push_back(rowStart + s1);
                        localTris.push_back(nextRow + s);

                        localTris.push_back(nextRow + s);
                        localTris.push_back(rowStart + s1);
                        localTris.push_back(nextRow + s1);
                    }
                }

                // Bottom cap
                i32 lastRow = 1 + (totalRings - 1) * kSegments;
                for (i32 s = 0; s < kSegments; ++s)
                {
                    localTris.push_back(bottomPole);
                    localTris.push_back(lastRow + s);
                    localTris.push_back(lastRow + (s + 1) % kSegments);
                }

                glm::mat4 worldTransform = GetWorldTransform(entity);
                const auto base = AppendTransformedVerts(worldTransform, localVerts.data(), static_cast<i32>(localVerts.size()), outVerts);

                for (i32 idx : localTris)
                    outTris.push_back(base + idx);
            }
        }

        // Collect from MeshCollider3DComponent (triangle mesh from MeshColliderAsset)
        auto meshColView = scene->GetAllEntitiesWith<MeshCollider3DComponent, TransformComponent>();
        for (auto e : meshColView)
        {
            Entity entity = { e, scene };
            auto& mc = entity.GetComponent<MeshCollider3DComponent>();

            if (mc.m_ColliderAsset == 0)
                continue;

            auto meshColliderAsset = AssetManager::GetAsset<MeshColliderAsset>(mc.m_ColliderAsset);
            if (!meshColliderAsset || meshColliderAsset->m_ColliderMesh == 0)
                continue;

            auto meshSource = AssetManager::GetAsset<MeshSource>(meshColliderAsset->m_ColliderMesh);
            if (!meshSource)
                continue;

            const auto& vertices = meshSource->GetVertices();
            const auto& indices = meshSource->GetIndices();

            glm::mat4 worldTransform = GetWorldTransform(entity);
            glm::mat4 colliderTransform = glm::translate(glm::mat4(1.0f), mc.m_Offset) * glm::scale(glm::mat4(1.0f), mc.m_Scale);
            glm::mat4 combined = worldTransform * colliderTransform;

            const auto baseVertex = static_cast<i32>(outVerts.size() / 3);
            for (i32 i = 0; i < static_cast<i32>(vertices.Num()); ++i)
            {
                glm::vec4 worldPos = combined * glm::vec4(vertices[i].Position, 1.0f);
                outVerts.push_back(worldPos.x);
                outVerts.push_back(worldPos.y);
                outVerts.push_back(worldPos.z);
            }

            const auto vertexCount = static_cast<i32>(vertices.Num());
            for (i32 i = 0; i + 2 < static_cast<i32>(indices.Num()); i += 3)
            {
                i32 i0 = static_cast<i32>(indices[i]);
                i32 i1 = static_cast<i32>(indices[i + 1]);
                i32 i2 = static_cast<i32>(indices[i + 2]);
                if (i0 < 0 || i0 >= vertexCount || i1 < 0 || i1 >= vertexCount || i2 < 0 || i2 >= vertexCount)
                    continue;
                outTris.push_back(baseVertex + i0);
                outTris.push_back(baseVertex + i1);
                outTris.push_back(baseVertex + i2);
            }
        }

        // Collect from TerrainComponent (grid tessellation from heightmap)
        auto terrainView = scene->GetAllEntitiesWith<TerrainComponent, TransformComponent>();
        for (auto e : terrainView)
        {
            Entity entity = { e, scene };
            auto& terrain = entity.GetComponent<TerrainComponent>();

            if (!terrain.m_TerrainData)
                continue;

            u32 resolution = terrain.m_TerrainData->GetResolution();
            if (resolution < 2)
                continue;

            // Downsample large heightmaps to keep vertex count manageable
            constexpr u32 kMaxNavRes = 128;
            u32 step = 1;
            while (resolution / step > kMaxNavRes)
                step *= 2;

            u32 gridRes = (resolution - 1) / step + 1;
            glm::mat4 worldTransform = GetWorldTransform(entity);

            const auto baseVertex = static_cast<i32>(outVerts.size() / 3);

            for (u32 z = 0; z < gridRes; ++z)
            {
                u32 srcZ = std::min(z * step, resolution - 1);
                f32 nz = static_cast<f32>(srcZ) / static_cast<f32>(resolution - 1);
                for (u32 x = 0; x < gridRes; ++x)
                {
                    u32 srcX = std::min(x * step, resolution - 1);
                    f32 nx = static_cast<f32>(srcX) / static_cast<f32>(resolution - 1);
                    f32 height = terrain.m_TerrainData->GetHeightAt(nx, nz);

                    // Local-space: X in [0, WorldSizeX], Y = height * HeightScale, Z in [0, WorldSizeZ]
                    glm::vec3 localPos(nx * terrain.m_WorldSizeX, height * terrain.m_HeightScale, nz * terrain.m_WorldSizeZ);
                    glm::vec4 wp = worldTransform * glm::vec4(localPos, 1.0f);
                    outVerts.push_back(wp.x);
                    outVerts.push_back(wp.y);
                    outVerts.push_back(wp.z);
                }
            }

            // Generate triangle indices for the grid
            for (u32 z = 0; z < gridRes - 1; ++z)
            {
                for (u32 x = 0; x < gridRes - 1; ++x)
                {
                    i32 topLeft = baseVertex + static_cast<i32>(z * gridRes + x);
                    i32 topRight = topLeft + 1;
                    i32 bottomLeft = topLeft + static_cast<i32>(gridRes);
                    i32 bottomRight = bottomLeft + 1;

                    outTris.push_back(topLeft);
                    outTris.push_back(bottomLeft);
                    outTris.push_back(topRight);

                    outTris.push_back(topRight);
                    outTris.push_back(bottomLeft);
                    outTris.push_back(bottomRight);
                }
            }
        }
    }

    Ref<NavMesh> NavMeshGenerator::Generate(Scene* scene, const NavMeshSettings& settings,
                                            const glm::vec3& boundsMin, const glm::vec3& boundsMax)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
        {
            OLO_CORE_ERROR("NavMeshGenerator::Generate: scene is null");
            return nullptr;
        }

        std::vector<f32> verts;
        std::vector<i32> tris;
        CollectSceneGeometry(scene, verts, tris);

        const i32 nverts = static_cast<i32>(verts.size() / 3);
        const i32 ntris = static_cast<i32>(tris.size() / 3);

        if (nverts == 0 || ntris == 0)
        {
            OLO_CORE_WARN("NavMeshGenerator: No geometry found in scene");
            return nullptr;
        }

        OLO_CORE_INFO("NavMeshGenerator: Collected {} vertices, {} triangles", nverts, ntris);

        // Validate settings
        if (settings.CellSize <= 0.0f || settings.CellHeight <= 0.0f)
        {
            OLO_CORE_ERROR("NavMeshGenerator: CellSize ({}) and CellHeight ({}) must be > 0", settings.CellSize, settings.CellHeight);
            return nullptr;
        }
        if (settings.AgentRadius < 0.0f || settings.AgentHeight <= 0.0f || settings.AgentMaxClimb < 0.0f)
        {
            OLO_CORE_ERROR("NavMeshGenerator: Invalid agent parameters (Radius={}, Height={}, MaxClimb={})",
                           settings.AgentRadius, settings.AgentHeight, settings.AgentMaxClimb);
            return nullptr;
        }

        // Step 1: Initialize Recast config
        rcConfig cfg{};
        cfg.cs = settings.CellSize;
        cfg.ch = settings.CellHeight;
        cfg.walkableSlopeAngle = settings.AgentMaxSlope;
        cfg.walkableHeight = static_cast<i32>(std::ceilf(settings.AgentHeight / cfg.ch));
        cfg.walkableClimb = static_cast<i32>(std::floorf(settings.AgentMaxClimb / cfg.ch));
        cfg.walkableRadius = static_cast<i32>(std::ceilf(settings.AgentRadius / cfg.cs));
        cfg.maxEdgeLen = static_cast<i32>(settings.EdgeMaxLen / cfg.cs);
        cfg.maxSimplificationError = settings.EdgeMaxError;
        cfg.minRegionArea = settings.RegionMinSize * settings.RegionMinSize;
        cfg.mergeRegionArea = settings.RegionMergeSize * settings.RegionMergeSize;
        cfg.maxVertsPerPoly = settings.VertsPerPoly;
        cfg.detailSampleDist = settings.DetailSampleDist < 0.9f ? 0.0f : settings.CellSize * settings.DetailSampleDist;
        cfg.detailSampleMaxError = settings.CellHeight * settings.DetailSampleMaxError;

        // Set bounds
        cfg.bmin[0] = boundsMin.x;
        cfg.bmin[1] = boundsMin.y;
        cfg.bmin[2] = boundsMin.z;
        cfg.bmax[0] = boundsMax.x;
        cfg.bmax[1] = boundsMax.y;
        cfg.bmax[2] = boundsMax.z;

        rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

        rcContext ctx;

        // Step 2: Rasterize input polygon soup
        auto* solid = rcAllocHeightfield();
        if (!solid || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not create heightfield");
            rcFreeHeightField(solid);
            return nullptr;
        }

        std::vector<u8> triAreas(static_cast<size_t>(ntris), 0);
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris, triAreas.data());
        if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), triAreas.data(), ntris, *solid, cfg.walkableClimb))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not rasterize triangles");
            rcFreeHeightField(solid);
            return nullptr;
        }

        // Step 3: Filter walkable surfaces
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

        // Step 4: Partition walkable surface
        auto* chf = rcAllocCompactHeightfield();
        if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not build compact heightfield");
            rcFreeHeightField(solid);
            rcFreeCompactHeightfield(chf);
            return nullptr;
        }
        rcFreeHeightField(solid);

        if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not erode walkable area");
            rcFreeCompactHeightfield(chf);
            return nullptr;
        }

        if (!rcBuildDistanceField(&ctx, *chf))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not build distance field");
            rcFreeCompactHeightfield(chf);
            return nullptr;
        }

        if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not build regions");
            rcFreeCompactHeightfield(chf);
            return nullptr;
        }

        // Step 5: Trace and simplify region contours
        auto* cset = rcAllocContourSet();
        if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not create contours");
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            return nullptr;
        }

        // Step 6: Build polygon mesh
        auto* pmesh = rcAllocPolyMesh();
        if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not triangulate contours");
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            rcFreePolyMesh(pmesh);
            return nullptr;
        }

        // Step 7: Build detail mesh
        auto* dmesh = rcAllocPolyMeshDetail();
        if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not build detail mesh");
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            rcFreePolyMesh(pmesh);
            rcFreePolyMeshDetail(dmesh);
            return nullptr;
        }

        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);

        // Step 8: Create Detour navmesh data
        if (cfg.maxVertsPerPoly <= DT_VERTS_PER_POLYGON)
        {
            // Set flags for all polygons
            for (i32 i = 0; i < pmesh->npolys; ++i)
            {
                if (pmesh->areas[i] != RC_NULL_AREA)
                {
                    pmesh->areas[i] = RC_WALKABLE_AREA;
                    pmesh->flags[i] = 1;
                }
            }
        }

        dtNavMeshCreateParams params{};
        params.verts = pmesh->verts;
        params.vertCount = pmesh->nverts;
        params.polys = pmesh->polys;
        params.polyAreas = pmesh->areas;
        params.polyFlags = pmesh->flags;
        params.polyCount = pmesh->npolys;
        params.nvp = pmesh->nvp;
        params.detailMeshes = dmesh->meshes;
        params.detailVerts = dmesh->verts;
        params.detailVertsCount = dmesh->nverts;
        params.detailTris = dmesh->tris;
        params.detailTriCount = dmesh->ntris;
        params.walkableHeight = settings.AgentHeight;
        params.walkableRadius = settings.AgentRadius;
        params.walkableClimb = settings.AgentMaxClimb;
        rcVcopy(params.bmin, pmesh->bmin);
        rcVcopy(params.bmax, pmesh->bmax);
        params.cs = cfg.cs;
        params.ch = cfg.ch;
        params.buildBvTree = true;

        u8* navData = nullptr;
        i32 navDataSize = 0;
        if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
        {
            OLO_CORE_ERROR("NavMeshGenerator: Could not build Detour navmesh data");
            rcFreePolyMesh(pmesh);
            rcFreePolyMeshDetail(dmesh);
            return nullptr;
        }

        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);

        // Step 9: Create the Detour navmesh
        auto* navMesh = dtAllocNavMesh();
        if (!navMesh)
        {
            dtFree(navData);
            OLO_CORE_ERROR("NavMeshGenerator: Could not allocate Detour navmesh");
            return nullptr;
        }

        dtStatus status = navMesh->init(navData, navDataSize, DT_TILE_FREE_DATA);
        if (dtStatusFailed(status))
        {
            dtFree(navData);
            dtFreeNavMesh(navMesh);
            OLO_CORE_ERROR("NavMeshGenerator: Could not init Detour navmesh");
            return nullptr;
        }

        auto result = Ref<NavMesh>::Create();
        result->SetDetourNavMesh(navMesh);
        result->SetSettings(settings);

        OLO_CORE_INFO("NavMeshGenerator: Generated navmesh with {} polygons", result->GetPolyCount());
        return result;
    }
} // namespace OloEngine
