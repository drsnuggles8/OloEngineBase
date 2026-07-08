#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/ClothWindSystem.h"

#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Wind/WindSystem.h"

#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Converts wind velocity (m/s) + cloth mass (kg) into a force (N): this is a
        // deliberately simple gameplay approximation (no cross-sectional area, air
        // density, or relative-velocity-squared drag — see WindSystem::GetWindAtPoint's
        // own "precision is secondary" contract). Scaling by mass keeps the response
        // roughly proportional to how gravity already affects the same body, so a
        // heavier cloth needs a stronger wind to billow as much as a light one — same
        // reasoning BuoyancySystem uses for its drag-by-mass scaling.
        constexpr f32 kWindDragCoefficient = 1.0f;
    } // namespace

    void ClothWindSystem::OnUpdate(Scene* scene, f32 time)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene || !std::isfinite(time))
            return;

        JoltScene* jolt = scene->GetPhysicsScene();
        if (!jolt || jolt->GetClothCount() == 0)
            return;

        const WindSettings& wind = scene->GetWindSettings();
        if (!wind.Enabled)
            return;

        auto view = scene->GetAllEntitiesWith<TransformComponent, ClothComponent>();
        for (auto e : view)
        {
            Entity entity{ e, scene };
            const auto& cloth = entity.GetComponent<ClothComponent>();
            if (!cloth.m_Enabled)
                continue;

            const f32 influence = std::isfinite(cloth.m_WindInfluence) ? std::clamp(cloth.m_WindInfluence, 0.0f, 1.0f) : 0.0f;
            if (influence <= 0.0f)
                continue;

            const UUID entityID = entity.GetUUID();

            // Live Jolt body position, not entity.GetWorldTransform() — the cached
            // WorldTransformComponent is refreshed by the PropagateTransforms scheduler node,
            // which runs AFTER PhysicsKick/PhysicsFence this same tick, so it would be one
            // physics tick stale for a moving/reparented cloth (issue #460 wind-coupling
            // slice). This single lookup also replaces the separate HasClothBody existence
            // check that used to precede it — a cloth with no live body simply fails here.
            glm::vec3 worldPos;
            if (!jolt->GetClothWorldPosition(entityID, worldPos))
                continue;
            if (!std::isfinite(worldPos.x) || !std::isfinite(worldPos.y) || !std::isfinite(worldPos.z))
                continue;

            const glm::vec3 windVelocity = WindSystem::GetWindAtPoint(wind, worldPos, time);
            const f32 mass = (std::isfinite(cloth.m_Mass) && cloth.m_Mass > 0.0f) ? cloth.m_Mass : 1.0f;

            const glm::vec3 force = windVelocity * (mass * kWindDragCoefficient * influence);
            jolt->ApplyClothWindForce(entityID, force);
        }
    }
} // namespace OloEngine
