#include "OloEnginePCH.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Core/Log.h"

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    static glm::mat4 GetWorldTransform(Entity entity)
    {
        auto& tc = entity.GetComponent<TransformComponent>();
        return glm::translate(glm::mat4(1.0f), tc.Translation) * glm::toMat4(glm::quat(tc.Rotation)) * glm::scale(glm::mat4(1.0f), tc.Scale);
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

            for (i32 i = 0; i < static_cast<i32>(indices.Num()); i += 3)
            {
                outTris.push_back(baseVertex + static_cast<i32>(indices[i]));
                outTris.push_back(baseVertex + static_cast<i32>(indices[i + 1]));
                outTris.push_back(baseVertex + static_cast<i32>(indices[i + 2]));
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

                for (i32 i = 0; i < static_cast<i32>(indices.Num()); i += 3)
                {
                    outTris.push_back(baseVertex + static_cast<i32>(indices[i]));
                    outTris.push_back(baseVertex + static_cast<i32>(indices[i + 1]));
                    outTris.push_back(baseVertex + static_cast<i32>(indices[i + 2]));
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
                box.m_Offset + glm::vec3( halfSize.x, -halfSize.y, -halfSize.z),
                box.m_Offset + glm::vec3( halfSize.x,  halfSize.y, -halfSize.z),
                box.m_Offset + glm::vec3(-halfSize.x,  halfSize.y, -halfSize.z),
                box.m_Offset + glm::vec3(-halfSize.x, -halfSize.y,  halfSize.z),
                box.m_Offset + glm::vec3( halfSize.x, -halfSize.y,  halfSize.z),
                box.m_Offset + glm::vec3( halfSize.x,  halfSize.y,  halfSize.z),
                box.m_Offset + glm::vec3(-halfSize.x,  halfSize.y,  halfSize.z),
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
    }

    Ref<NavMesh> NavMeshGenerator::Generate(Scene* scene, const NavMeshSettings& settings,
                                            const glm::vec3& boundsMin, const glm::vec3& boundsMax)
    {
        OLO_PROFILE_FUNCTION();

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
