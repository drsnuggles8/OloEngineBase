#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshSettings.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class Scene;

    class NavMeshGenerator
    {
      public:
        // Generate navmesh from all MeshComponents + TerrainComponents + Collider3DComponents in scene
        static Ref<NavMesh> Generate(Scene* scene, const NavMeshSettings& settings, const glm::vec3& boundsMin, const glm::vec3& boundsMax);

      private:
        // Collect world-space triangles from scene geometry
        static void CollectSceneGeometry(Scene* scene, std::vector<f32>& outVerts, std::vector<i32>& outTris);
    };
} // namespace OloEngine
