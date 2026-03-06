#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Asset/AssetManager.h"

#include <glm/geometric.hpp>

namespace OloEngine
{
    i32 LODGroup::SelectLOD(f32 distance) const
    {
        OLO_PROFILE_FUNCTION();
        if (Levels.empty())
        {
            return -1;
        }

        f32 const safeBias = (Bias <= 0.0f) ? 1.0f : Bias;
        f32 const effectiveDistance = distance / safeBias;

        for (i32 i = 0; i < static_cast<i32>(Levels.size()); ++i)
        {
            if (effectiveDistance <= Levels[i].MaxDistance)
            {
                return i;
            }
        }

        // Beyond all thresholds — return lowest detail level (last)
        return static_cast<i32>(Levels.size()) - 1;
    }

    LODSelectionResult SelectLODMesh(
        const Ref<Mesh>& mesh,
        const glm::mat4& modelMatrix,
        const glm::vec3& viewPosition,
        const LODGroup* lodGroup,
        Ref<Mesh>& outMesh)
    {
        OLO_PROFILE_FUNCTION();
        outMesh = mesh;
        LODSelectionResult result;

        if (!mesh || !lodGroup || lodGroup->Levels.empty())
        {
            return result;
        }

        BoundingSphere sphere = mesh->GetTransformedBoundingSphere(modelMatrix);
        f32 distance = glm::length(viewPosition - sphere.Center);
        i32 lodIndex = lodGroup->SelectLOD(distance);
        if (lodIndex < 0)
        {
            return result;
        }

        result.SelectedLODIndex = lodIndex;
        AssetHandle lodMeshHandle = lodGroup->Levels[lodIndex].MeshHandle;
        if (lodMeshHandle != 0)
        {
            auto lodMesh = AssetManager::GetAsset<Mesh>(lodMeshHandle);
            if (lodMesh)
            {
                if (lodMesh != mesh)
                {
                    result.Switched = true;
                }
                outMesh = lodMesh;
            }
        }

        return result;
    }
} // namespace OloEngine
