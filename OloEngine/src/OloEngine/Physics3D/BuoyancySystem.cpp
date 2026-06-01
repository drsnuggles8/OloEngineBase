#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/BuoyancySystem.h"

#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Renderer/WaterSurface.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace OloEngine
{
    namespace
    {
        constexpr f32 kGravity = 9.81f; // matches WaterSurface / Water.glsl

        // A pre-resolved water tile: wave params + the flat-plane footprint test
        // (an XZ axis-aligned rectangle, mirroring Scene::GetWaterCameraFootprintGap).
        struct WaterVolume
        {
            WaterSurface::Params m_Params;
            glm::mat4 m_InvModel{ 1.0f };
            f32 m_HalfX = 0.0f;
            f32 m_HalfZ = 0.0f;
        };

        [[nodiscard]] bool IsOverFootprint(const WaterVolume& w, const glm::vec3& worldPos)
        {
            const glm::vec4 local = w.m_InvModel * glm::vec4(worldPos, 1.0f);
            if (!std::isfinite(local.x) || !std::isfinite(local.z))
                return false;
            return local.x >= -w.m_HalfX && local.x <= w.m_HalfX && local.z >= -w.m_HalfZ && local.z <= w.m_HalfZ;
        }

        [[nodiscard]] WaterSurface::Params MakeParams(const WaterComponent& wc, f32 planeHeight)
        {
            WaterSurface::Params p;
            p.m_WaveDir0 = wc.PackWaveDir0();
            p.m_WaveDir1 = wc.PackWaveDir1();
            p.m_WaveFrequency = wc.m_WaveFrequency;
            p.m_WaveAmplitude = wc.m_WaveAmplitude;
            p.m_WaveSpeed = wc.m_WaveSpeed;
            p.m_PlaneHeight = planeHeight;
            return p;
        }

        // The 8 corners of the buoyancy box (sign of each local half-extent axis).
        constexpr std::array<glm::vec3, 8> kCornerSigns = { {
            { -1.0f, -1.0f, -1.0f }, { 1.0f, -1.0f, -1.0f }, { -1.0f, -1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f },
            { -1.0f, 1.0f, -1.0f }, { 1.0f, 1.0f, -1.0f }, { -1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f },
        } };
    } // namespace

    void BuoyancySystem::OnUpdate(Scene* scene, f32 rawTime, [[maybe_unused]] f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene || !std::isfinite(rawTime))
            return;

        JoltScene* jolt = scene->GetPhysicsScene();
        if (!jolt)
            return;

        // --- Resolve enabled water tiles up front (typically 0 or 1) ---
        std::vector<WaterVolume> waters;
        {
            auto waterView = scene->GetAllEntitiesWith<TransformComponent, WaterComponent>();
            for (auto e : waterView)
            {
                Entity waterEntity{ e, scene };
                const auto& wc = waterEntity.GetComponent<WaterComponent>();
                if (!wc.m_Enabled)
                    continue;

                const glm::mat4 model = waterEntity.GetComponent<TransformComponent>().GetTransform();
                // The water grid is a horizontal XZ plane at local y = 0; its world
                // height is the y of the tile origin (same assumption as the
                // underwater-fog footprint test).
                const glm::vec4 originWorld = model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                if (!std::isfinite(originWorld.y))
                    continue;

                const f32 safeSizeX = std::isfinite(wc.m_WorldSizeX) ? wc.m_WorldSizeX : 0.1f;
                const f32 safeSizeZ = std::isfinite(wc.m_WorldSizeZ) ? wc.m_WorldSizeZ : 0.1f;

                WaterVolume w;
                w.m_Params = MakeParams(wc, originWorld.y);
                w.m_InvModel = glm::inverse(model);
                w.m_HalfX = std::clamp(safeSizeX, 0.1f, 10000.0f) * 0.5f;
                w.m_HalfZ = std::clamp(safeSizeZ, 0.1f, 10000.0f) * 0.5f;
                waters.push_back(w);
            }
        }
        if (waters.empty())
            return;

        // --- Apply buoyancy to each dynamic body with a BuoyancyComponent ---
        auto view = scene->GetAllEntitiesWith<TransformComponent, BuoyancyComponent, Rigidbody3DComponent>();
        for (auto e : view)
        {
            Entity entity{ e, scene };
            const auto& buoyancy = entity.GetComponent<BuoyancyComponent>();
            if (!buoyancy.m_Enabled)
                continue;

            if (entity.GetComponent<Rigidbody3DComponent>().m_Type != BodyType3D::Dynamic)
                continue;

            Ref<JoltBody> body = jolt->GetBody(entity);
            if (!body || !body->IsDynamic())
                continue;

            const glm::vec3 bodyPos = body->GetPosition();
            if (!std::isfinite(bodyPos.x) || !std::isfinite(bodyPos.y) || !std::isfinite(bodyPos.z))
                continue;

            // Pick the first enabled water tile whose footprint the body sits over.
            const WaterVolume* water = nullptr;
            for (const auto& w : waters)
            {
                if (IsOverFootprint(w, bodyPos))
                {
                    water = &w;
                    break;
                }
            }
            if (!water)
                continue;

            // --- Sanitize tunables (defence in depth against bad serialized data) ---
            glm::vec3 ext = buoyancy.m_ProbeExtents;
            ext.x = std::isfinite(ext.x) ? std::clamp(ext.x, 0.01f, 1000.0f) : 0.5f;
            ext.y = std::isfinite(ext.y) ? std::clamp(ext.y, 0.01f, 1000.0f) : 0.5f;
            ext.z = std::isfinite(ext.z) ? std::clamp(ext.z, 0.01f, 1000.0f) : 0.5f;

            const f32 fluidDensity = (std::isfinite(buoyancy.m_FluidDensity) && buoyancy.m_FluidDensity > 0.0f)
                                         ? buoyancy.m_FluidDensity : 1000.0f;
            const f32 buoyancyScale = std::isfinite(buoyancy.m_BuoyancyScale) ? std::max(buoyancy.m_BuoyancyScale, 0.0f) : 1.0f;
            const f32 ramp = (std::isfinite(buoyancy.m_SubmergenceRamp) && buoyancy.m_SubmergenceRamp > 1e-3f)
                                 ? buoyancy.m_SubmergenceRamp : 1e-3f;
            const f32 linearDrag = std::isfinite(buoyancy.m_LinearDrag) ? std::max(buoyancy.m_LinearDrag, 0.0f) : 0.0f;
            const f32 angularDrag = std::isfinite(buoyancy.m_AngularDrag) ? std::max(buoyancy.m_AngularDrag, 0.0f) : 0.0f;

            // Each corner probe represents 1/8 of the box volume:
            // (2ex)(2ey)(2ez) / 8 == ex*ey*ez.
            const f32 probeVolume = ext.x * ext.y * ext.z;

            const glm::quat bodyRot = body->GetRotation();

            // --- Per-probe Archimedes force (applied at the corner -> torque) ---
            f32 submergedAccum = 0.0f;
            for (const glm::vec3& sign : kCornerSigns)
            {
                const glm::vec3 probeWorld = bodyPos + bodyRot * (sign * ext);
                const f32 surfaceY = WaterSurface::SampleHeight(water->m_Params,
                                                                glm::vec2(probeWorld.x, probeWorld.z), rawTime);
                const f32 depth = surfaceY - probeWorld.y;
                if (!(depth > 0.0f)) // dry (the negation also rejects NaN)
                    continue;

                const f32 submergedFrac = std::clamp(depth / ramp, 0.0f, 1.0f);
                submergedAccum += submergedFrac;

                const f32 buoyantMag = fluidDensity * kGravity * probeVolume * submergedFrac * buoyancyScale;
                if (!std::isfinite(buoyantMag) || buoyantMag <= 0.0f)
                    continue;

                body->AddForce(glm::vec3(0.0f, buoyantMag, 0.0f), probeWorld, EForceMode::Force);
            }

            if (submergedAccum <= 0.0f)
                continue; // body is clear of the water — no drag to apply

            const f32 submergedFracTotal = submergedAccum / static_cast<f32>(kCornerSigns.size()); // 0..1

            // --- Submerged drag ---
            // Scaling by mass makes the coefficient a mass-independent decay rate
            // (per second), so a heavy crate and a light buoy damp at the same
            // tunable rate. Plain forces/torques keep Jolt's integration stable.
            const f32 mass = body->GetMass();
            const f32 dragMassScale = (std::isfinite(mass) && mass > 0.0f) ? mass : 1.0f;

            if (linearDrag > 0.0f)
            {
                const glm::vec3 linVel = body->GetLinearVelocity();
                if (std::isfinite(linVel.x) && std::isfinite(linVel.y) && std::isfinite(linVel.z))
                {
                    const glm::vec3 dragForce = -linVel * (linearDrag * submergedFracTotal * dragMassScale);
                    body->AddForce(dragForce, EForceMode::Force);
                }
            }
            if (angularDrag > 0.0f)
            {
                const glm::vec3 angVel = body->GetAngularVelocity();
                if (std::isfinite(angVel.x) && std::isfinite(angVel.y) && std::isfinite(angVel.z))
                {
                    const glm::vec3 dragTorque = -angVel * (angularDrag * submergedFracTotal * dragMassScale);
                    body->AddTorque(dragTorque);
                }
            }
        }
    }
}
