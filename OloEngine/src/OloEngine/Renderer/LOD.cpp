#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Asset/AssetManager.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Floor on the camera-to-bounds distance. A camera inside the bounds
        // legitimately reports "as large as the screen"; the floor only keeps
        // the division finite.
        constexpr f32 kMinLODDistance = 1e-3f;

        [[nodiscard]] f32 SanitizeBias(f32 bias)
        {
            return (std::isfinite(bias) && bias > 0.0f) ? bias : 1.0f;
        }
    } // namespace

    i32 LODGroup::SelectLOD(f32 distance) const
    {
        OLO_PROFILE_FUNCTION();
        if (Levels.empty())
        {
            return -1;
        }

        f32 const safeBias = SanitizeBias(Bias);
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

    bool LODGroup::HasErrorData() const
    {
        return std::ranges::any_of(Levels,
                                   [](const LODLevel& level)
                                   { return std::isfinite(level.Error) && level.Error > 0.0f; });
    }

    i32 LODGroup::SelectLODByPixelError(f32 projectedPixelSize, f32 pixelErrorThreshold) const
    {
        OLO_PROFILE_FUNCTION();
        if (Levels.empty())
        {
            return -1;
        }

        if (!std::isfinite(projectedPixelSize) || projectedPixelSize < 0.0f)
        {
            projectedPixelSize = 0.0f;
        }
        f32 const safeThreshold = (std::isfinite(pixelErrorThreshold) && pixelErrorThreshold > 0.0f)
                                      ? pixelErrorThreshold
                                      : 1.0f;

        // Bias keeps the same meaning it has in the distance path: > 1 holds
        // detail longer. There it shrinks the distance; here it inflates the
        // apparent size, which is the same statement about the same object.
        f32 const effectivePixelSize = projectedPixelSize * SanitizeBias(Bias);

        // Levels are ordered fine → coarse and Error is monotonically increasing, so
        // the first level that exceeds the budget ends the scan; everything past it
        // is coarser still.
        i32 selected = 0;
        for (i32 i = 1; i < static_cast<i32>(Levels.size()); ++i)
        {
            f32 const error = Levels[i].Error;

            // A level at or below zero error past index 0 is UNMEASURED, not free.
            // It is what the inspector's "Add LOD Level" button produces, and what a
            // partially-authored group carries. Treating it as free would make it
            // satisfy any budget and win at every distance — the coarsest blank
            // level would be selected point-blank. There is nothing to judge it on,
            // so the scan stops here and keeps the last measured level.
            if (!std::isfinite(error) || error <= 0.0f)
            {
                break;
            }
            if (effectivePixelSize * error >= safeThreshold)
            {
                break;
            }
            selected = i;
        }

        return selected;
    }

    f32 EstimateProjectedPixelSize(const BoundingBox& localBounds,
                                   const glm::mat4& modelMatrix,
                                   const LODViewParams& view)
    {
        // Per-axis world extent WITHOUT re-fitting an axis-aligned box around a
        // rotated one: the model matrix's basis-vector lengths are the per-axis
        // scale. A refit AABB grows and shrinks as the OBJECT rotates, which
        // would make the estimate depend on object orientation for no visual
        // reason; the rough extent below does not.
        glm::vec3 const localSize = localBounds.GetSize();
        f32 const extentX = glm::length(glm::vec3(modelMatrix[0])) * localSize.x;
        f32 const extentY = glm::length(glm::vec3(modelMatrix[1])) * localSize.y;
        f32 const extentZ = glm::length(glm::vec3(modelMatrix[2])) * localSize.z;
        f32 const extent = std::max({ extentX, extentY, extentZ });

        if (!std::isfinite(extent) || extent <= 0.0f)
        {
            return 0.0f;
        }

        glm::vec3 const center = glm::vec3(modelMatrix * glm::vec4(localBounds.GetCenter(), 1.0f));
        if (!Math::IsFinite(center) || !Math::IsFinite(view.ViewPosition))
        {
            return 0.0f;
        }

        // Distance to the near side of the bounds rather than to the centre, so a
        // large mesh the camera is standing inside does not report a small size.
        f32 const distance = std::max(glm::length(center - view.ViewPosition) - 0.5f * extent, kMinLODDistance);

        // Small-angle screen projection. An object subtending `extent / distance`
        // radians covers that fraction of the vertical view cone, and the cone
        // spans 2 * tan(fovY/2) at unit distance. Camera POSITION, FOV and
        // resolution are the only inputs — no view matrix, hence no orientation
        // dependence.
        f32 const tanHalfFovY = (std::isfinite(view.TanHalfFovY) && view.TanHalfFovY > 0.0f) ? view.TanHalfFovY : 1.0f;
        auto const screenHeight = static_cast<f32>(std::max(view.ScreenHeight, 1u));

        f32 const pixelSize = (extent / distance) / (2.0f * tanHalfFovY) * screenHeight;
        return std::isfinite(pixelSize) ? pixelSize : 0.0f;
    }

    LODSelectionResult SelectLODMesh(
        const Ref<Mesh>& mesh,
        const glm::mat4& modelMatrix,
        const LODViewParams& view,
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

        i32 lodIndex = -1;
        if (lodGroup->HasErrorData())
        {
            f32 const pixelSize = EstimateProjectedPixelSize(mesh->GetBoundingBox(), modelMatrix, view);
            lodIndex = lodGroup->SelectLODByPixelError(pixelSize, view.PixelErrorThreshold);
        }
        else
        {
            // Hand-authored group with no measured error — keep the legacy
            // camera-distance thresholds so existing scenes select as before.
            BoundingSphere const sphere = mesh->GetTransformedBoundingSphere(modelMatrix);
            lodIndex = lodGroup->SelectLOD(glm::length(view.ViewPosition - sphere.Center));
        }

        if (lodIndex < 0)
        {
            return result;
        }

        result.SelectedLODIndex = lodIndex;
        if (AssetHandle lodMeshHandle = lodGroup->Levels[lodIndex].MeshHandle; lodMeshHandle != 0)
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
