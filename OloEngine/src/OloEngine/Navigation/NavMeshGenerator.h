#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshSettings.h"
#include "OloEngine/Navigation/OffMeshLink.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class Scene;

    class NavMeshGenerator
    {
      public:
        // Generate navmesh from all MeshComponents + TerrainComponents + Collider3DComponents in scene.
        // Optional off-mesh links are baked in as Detour off-mesh connections so agents can cross gaps the
        // walkable surface can't span (caller collects them from the scene's NavMeshBoundsComponent(s)).
        static Ref<NavMesh> Generate(Scene* scene, const NavMeshSettings& settings, const glm::vec3& boundsMin,
                                     const glm::vec3& boundsMax, const std::vector<OffMeshLink>& links = {});

      private:
        // Collect world-space triangles from scene geometry
        static void CollectSceneGeometry(Scene* scene, std::vector<f32>& outVerts, std::vector<i32>& outTris);
    };
} // namespace OloEngine
