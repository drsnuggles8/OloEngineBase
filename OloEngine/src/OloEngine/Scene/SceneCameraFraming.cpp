#include "OloEnginePCH.h"
#include "SceneCameraFraming.h"

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    bool FrameCameraOnEntity(const Ref<Scene>& scene, u64 entityUuid, EditorCamera& camera)
    {
        if (!scene)
            return false;
        const auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityUuid));
        if (!entityOpt)
            return false;
        Entity entity = *entityOpt;
        if (!entity.HasComponent<TransformComponent>())
            return false;

        // World transform: compose local transforms up the parent chain (the
        // scene stores parent-relative transforms; gizmos and rendering do the
        // same composition).
        glm::mat4 world = entity.GetComponent<TransformComponent>().GetTransform();
        for (UUID parentId = entity.GetParentUUID(); static_cast<u64>(parentId) != 0;)
        {
            const auto parentOpt = scene->TryGetEntityWithUUID(parentId);
            if (!parentOpt)
                break;
            Entity parent = *parentOpt;
            if (parent.HasComponent<TransformComponent>())
                world = parent.GetComponent<TransformComponent>().GetTransform() * world;
            parentId = parent.GetParentUUID();
        }

        // Bounds: prefer real mesh bounds, fall back to a scale-derived radius.
        glm::vec3 center = glm::vec3(world[3]);
        f32 radius = 0.0f;
        if (entity.HasComponent<ModelComponent>())
        {
            if (const auto& model = entity.GetComponent<ModelComponent>().m_Model; model)
            {
                const BoundingBox worldBox = model->GetTransformedBoundingBox(world);
                center = worldBox.GetCenter();
                radius = glm::length(worldBox.GetExtents());
            }
        }
        else if (entity.HasComponent<MeshComponent>())
        {
            if (const auto& meshSource = entity.GetComponent<MeshComponent>().m_MeshSource; meshSource)
            {
                const BoundingBox worldBox = meshSource->GetBoundingBox().Transform(world);
                center = worldBox.GetCenter();
                radius = glm::length(worldBox.GetExtents());
            }
        }
        else if (entity.HasComponent<TerrainComponent>())
        {
            const auto& tc = entity.GetComponent<TerrainComponent>();
            center += glm::vec3(tc.m_WorldSizeX * 0.5f, tc.m_HeightScale * 0.4f, tc.m_WorldSizeZ * 0.5f);
            radius = std::max(tc.m_WorldSizeX, tc.m_WorldSizeZ) * 0.5f;
        }
        if (radius < 0.5f)
        {
            // Scale-derived fallback: length of the world matrix's basis columns.
            const f32 sx = glm::length(glm::vec3(world[0]));
            const f32 sy = glm::length(glm::vec3(world[1]));
            const f32 sz = glm::length(glm::vec3(world[2]));
            radius = std::max({ sx, sy, sz, 0.5f });
        }

        // Keep the current view direction; just re-pivot and fit. Derive the
        // distance from the camera's vertical FOV so the bounding sphere fills
        // the frame at any FOV (a fixed multiplier underfits at the editor's
        // default 30 degrees), with a small margin so silhouettes don't touch
        // the frame edge.
        const f32 fovRadians = glm::radians(camera.GetFOV());
        const f32 fitDistance = (radius / std::tan(fovRadians * 0.5f)) * 1.05f;
        camera.Focus(center, fitDistance, camera.GetYaw(), camera.GetPitch());
        return true;
    }
} // namespace OloEngine
