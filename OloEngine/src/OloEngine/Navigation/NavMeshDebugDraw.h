#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <DetourNavMesh.h>

#include <glm/glm.hpp>

namespace OloEngine
{
    class NavMeshDebugDraw
    {
    public:
        static void DrawNavMesh(const Ref<NavMesh>& navMesh, const glm::vec3& color = { 0.0f, 0.6f, 1.0f })
        {
            if (!navMesh || !navMesh->GetDetourNavMesh())
                return;

            const dtNavMesh* dtMesh = navMesh->GetDetourNavMesh();

            for (i32 i = 0; i < dtMesh->getMaxTiles(); ++i)
            {
                const dtMeshTile* tile = dtMesh->getTile(i);
                if (!tile || !tile->header)
                    continue;

                for (i32 j = 0; j < tile->header->polyCount; ++j)
                {
                    const dtPoly* poly = &tile->polys[j];
                    if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                        continue;

                    const dtPolyDetail* pd = &tile->detailMeshes[j];

                    // Draw the polygon edges
                    for (i32 k = 0; k < static_cast<i32>(poly->vertCount); ++k)
                    {
                        const f32* v0 = &tile->verts[poly->verts[k] * 3];
                        const f32* v1 = &tile->verts[poly->verts[(k + 1) % poly->vertCount] * 3];

                        glm::vec3 p0(v0[0], v0[1] + 0.05f, v0[2]);
                        glm::vec3 p1(v1[0], v1[1] + 0.05f, v1[2]);

                        Renderer3D::DrawLine(p0, p1, color, 1.0f);
                    }

                    // Draw detail triangles as semi-transparent overlay
                    for (i32 k = 0; k < pd->triCount; ++k)
                    {
                        const u8* t = &tile->detailTris[(pd->triBase + k) * 4];
                        glm::vec3 triangleVerts[3];
                        for (i32 m = 0; m < 3; ++m)
                        {
                            if (t[m] < poly->vertCount)
                            {
                                const f32* v = &tile->verts[poly->verts[t[m]] * 3];
                                triangleVerts[m] = { v[0], v[1] + 0.05f, v[2] };
                            }
                            else
                            {
                                const f32* v = &tile->detailVerts[(pd->vertBase + t[m] - poly->vertCount) * 3];
                                triangleVerts[m] = { v[0], v[1] + 0.05f, v[2] };
                            }
                        }

                        // Draw triangle edges
                        for (i32 m = 0; m < 3; ++m)
                        {
                            Renderer3D::DrawLine(triangleVerts[m], triangleVerts[(m + 1) % 3],
                                               glm::vec3(0.0f, 0.4f, 0.8f), 0.5f);
                        }
                    }
                }
            }
        }
    };
} // namespace OloEngine
