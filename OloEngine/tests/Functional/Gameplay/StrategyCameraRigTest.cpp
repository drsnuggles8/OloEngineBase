#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
#include "Functional/FunctionalTest.h"
#include "OloEngine/Gameplay/Strategy/StrategyCameraRig.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <limits>

namespace OloEngine::Functional
{
    class StrategyCameraRigTest : public FunctionalTest
    {
      protected:
        void BuildScene() override
        {
            auto mountain = GetScene().CreateEntity("Mountain");
            mountain.GetComponent<TransformComponent>().Translation = { -5000.0f, 0.0f, -5000.0f };
            auto& terrain = mountain.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = true;
            terrain.m_ProceduralSeed = 1740;
            terrain.m_ProceduralResolution = 128;
            terrain.m_WorldSizeX = 10000.0f;
            terrain.m_WorldSizeZ = 10000.0f;
            terrain.m_HeightScale = 1500.0f;
            terrain.m_HeightShaping.RidgeBlend = 0.58f;
            EnablePhysics3D();
        }
    };

    TEST_F(StrategyCameraRigTest, SweepsFromTwentyMetresToMassifWithNearPlaneAboveTerrain)
    {
        auto* physics = GetScene().GetPhysicsScene();
        ASSERT_NE(physics, nullptr);
        constexpr f32 dt = 1.0f / 60.0f;
        constexpr f32 nearClip = 0.5f;
        constexpr f32 halfHeight = 0.2071068f; // near * tan(vertical FOV/2) for 45 degrees
        constexpr f32 halfWidth = halfHeight * 16.0f / 9.0f;
        const f32 radius = glm::length(glm::vec3(halfWidth, halfHeight, nearClip));
        const StrategyCameraSettings settings{ { -4990.0f, -4990.0f }, { 4990.0f, 4990.0f } };
        const auto ground = [&](glm::vec2 xz, f32 probeRadius)
        {
            return QueryStrategyGround(*physics, xz, probeRadius, settings.GroundBottom, settings.GroundTop);
        };
        const auto initialHeight = ground({ 0.0f, 0.0f }, 0.0f);
        ASSERT_TRUE(initialHeight.has_value());
        StrategyCameraState state;
        state.Height = *initialHeight - 80.0f; // Force the clearance branch while the pivot settles.
        state.Pitch = state.TargetPitch = glm::radians(55.0f);
        state.LogDistance = state.TargetLogDistance = std::log(20.0f);
        unsigned onMountain = 0;
        f32 minClearance = std::numeric_limits<f32>::max();
        const std::array<glm::vec3, 5> nearPoints{
            glm::vec3{ 0.0f, 0.0f, -nearClip },
            glm::vec3{ -halfWidth, -halfHeight, -nearClip },
            glm::vec3{ -halfWidth, halfHeight, -nearClip },
            glm::vec3{ halfWidth, -halfHeight, -nearClip },
            glm::vec3{ halfWidth, halfHeight, -nearClip }
        };

        for (int frame = 0; frame < 320; ++frame)
        {
            const f32 logZoom = frame >= 90 && frame < 240 ? 0.06f : 0.0f;
            const glm::vec2 pan = frame < 240 ? glm::vec2(0.2f, 0.2f) : glm::vec2(0.0f);
            const StrategyCameraIntent intent{ pan, glm::vec2(0.0f), { 0.008f, 0.0f }, logZoom };
            const auto pose = StepStrategyCamera(state, settings, dt, intent, radius, ground);
            if (frame == 0)
            {
                const f32 uncorrectedY = state.Height + std::exp(state.LogDistance) * std::sin(state.Pitch);
                EXPECT_GT(pose.Position.y, uncorrectedY + 1.0f);
            }
            if (frame == 89)
                EXPECT_NEAR(std::exp(state.LogDistance), 20.0f, 0.01f);

            const glm::quat rotation(pose.Rotation);
            for (const auto& local : nearPoints)
            {
                const glm::vec3 point = pose.Position + rotation * local;
                SceneQueryHit hit;
                if (!physics->CastRay(RayCastInfo({ point.x, 2000.0f, point.z }, { 0.0f, -1.0f, 0.0f }, 2100.0f), hit))
                    continue;
                ++onMountain;
                minClearance = std::min(minClearance, point.y - hit.m_Position.y);
            }
        }

        EXPECT_GT(onMountain, 600u);
        EXPECT_GT(minClearance, 0.5f);
        EXPECT_NEAR(std::exp(state.LogDistance), settings.MaxDistance, 2.0f);
        EXPECT_GT(glm::length(state.Focus), 100.0f);
        RecordProperty("near_plane_points_over_terrain", std::to_string(onMountain));
        RecordProperty("minimum_near_plane_clearance_m", std::to_string(minClearance));
    }

    TEST_F(StrategyCameraRigTest, FocusAndClearanceFollowARebase)
    {
        auto* physics = GetScene().GetPhysicsScene();
        ASSERT_NE(physics, nullptr);
        StrategyCameraSettings settings{ { -4990.0f, -4990.0f }, { 4990.0f, 4990.0f } };
        StrategyCameraState state;
        state.Focus = { 12.0f, 32.0f };
        state.TargetFocus = { 18.0f, 42.0f };
        state.Yaw = state.TargetYaw = 0.3f;
        state.Pitch = state.TargetPitch = glm::radians(55.0f);
        state.LogDistance = state.TargetLogDistance = std::log(20.0f);
        SyncStrategyCameraOrigin(state, settings, GetScene().GetWorldOrigin());

        const auto ground = [&](glm::vec2 xz, f32 radius)
        {
            return QueryStrategyGround(*physics, xz, radius, settings.GroundBottom, settings.GroundTop);
        };
        const auto initialHeight = ground(state.Focus, 0.0f);
        ASSERT_TRUE(initialHeight.has_value());
        state.Height = *initialHeight - 80.0f;
        constexpr f32 dt = 1.0f / 60.0f;
        constexpr f32 radius = 0.66f;
        auto unshiftedState = state;
        const auto withoutRebase = StepStrategyCamera(unshiftedState, settings, dt, {}, radius, ground);

        constexpr glm::vec3 shift{ -3072.0f, 128.0f, 1024.0f };
        GetScene().RebaseOrigin(shift);
        SyncStrategyCameraOrigin(state, settings, GetScene().GetWorldOrigin());
        // Sampling the same origin on another tick must not apply a second shift.
        SyncStrategyCameraOrigin(state, settings, GetScene().GetWorldOrigin());
        const auto withRebase = StepStrategyCamera(state, settings, dt, {}, radius, ground);

        EXPECT_LT(glm::length(withRebase.Position - (withoutRebase.Position + shift)), 0.01f);
        EXPECT_LT(glm::length(withRebase.Rotation - withoutRebase.Rotation), 0.001f);
        EXPECT_FLOAT_EQ(settings.MinFocus.x, -4990.0f + shift.x);
        EXPECT_FLOAT_EQ(settings.MaxFocus.y, 4990.0f + shift.z);
        EXPECT_FLOAT_EQ(settings.GroundBottom, -100.0f + shift.y);
        EXPECT_FLOAT_EQ(settings.GroundTop, 2000.0f + shift.y);
    }

    class StrategyCameraGroundProbeTest : public FunctionalTest
    {
      protected:
        void BuildScene() override
        {
            auto floor = GetScene().CreateEntity("Floor near the ground search ceiling");
            floor.GetComponent<TransformComponent>().Translation.y = 9.0f;
            auto& collider = floor.AddComponent<BoxCollider3DComponent>();
            collider.m_HalfExtents = { 30.0f, 0.5f, 30.0f };
            floor.AddComponent<Rigidbody3DComponent>().m_Type = BodyType3D::Static;
            EnablePhysics3D();
        }
    };

    TEST_F(StrategyCameraGroundProbeTest, WideNearPlaneDoesNotStartIntersectingGround)
    {
        auto* physics = GetScene().GetPhysicsScene();
        ASSERT_NE(physics, nullptr);
        constexpr f32 floorTop = 9.5f;
        constexpr f32 radius = 6.55f;
        const auto contact = QueryStrategyGround(*physics, glm::vec2(0.0f), radius, 0.0f, 10.0f);
        ASSERT_TRUE(contact.has_value());
        EXPECT_NEAR(*contact, floorTop + radius, 0.02f);
    }
} // namespace OloEngine::Functional
