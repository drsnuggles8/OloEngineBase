// OLO_TEST_LAYER: Functional
// =============================================================================
// FluidCouplingTest.cpp
//
// Functional cross-subsystem tests for the PBF fluid × Jolt coupling
// (issue #630), driven through the real Scene::OnUpdateRuntime tick so the
// whole seam is exercised: the "Fluid" scheduler node (Before PhysicsKick),
// solver stepping, body-proxy extraction via JoltScene::OverlapBox, and the
// reaction impulses queued on dynamic bodies before the world step.
//
// Headless — no GL — so every domain runs the deterministic CPU reference
// solver (FluidSolverMode::Auto resolves to CPU without a renderer; tests
// also pin m_SolverMode = CPU explicitly for clarity).
//
// Assertions are comparative/discriminating rather than absolute where the
// coupling magnitude is tuning-dependent: a light box must end clearly above
// a dense box in the same pool, and clearly above its own no-fluid control.
// =============================================================================

#include "OloEnginePCH.h"

#include "../FunctionalTest.h"

#include "OloEngine/Fluid/CPUFluidSolver.h"
#include "OloEngine/Fluid/FluidWorld.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <cmath>

namespace OloEngine::Functional
{
    namespace
    {
        constexpr f32 kBoxHalf = 0.2f;

        /// Pool: domain x/z in [-1, 1], y in [0, 1.5], lower 60% prefilled with
        /// resting fluid. The engine-default FluidSettings radius is 0.1 m
        /// (spacing 0.2), so this yields ~320 particles — coarse but dense
        /// enough to carry a 0.4 m box, and small enough that the serial CPU
        /// reference stays fast in Debug.
        void ConfigurePool(FluidComponent& fluid)
        {
            fluid.m_Enabled = true;
            fluid.m_SolverMode = FluidSolverMode::CPU;
            fluid.m_DomainHalfExtents = { 1.0f, 0.75f, 1.0f };
            fluid.m_MaxParticles = 4096;
            fluid.m_PrefillFraction = 0.6f;
        }
    } // namespace

    class FluidCouplingTest : public FunctionalTest
    {
      protected:
        void BuildScene() override
        {
            // Scenes are assembled per-test in the body.
        }

        Entity SpawnPool()
        {
            Entity pool = GetScene().CreateEntity("FluidPool");
            pool.GetComponent<TransformComponent>().Translation = { 0.0f, 0.75f, 0.0f };
            auto& fluid = pool.AddComponent<FluidComponent>();
            ConfigurePool(fluid);
            return pool;
        }

        /// Static ground whose top face sits at y = 0 (the domain floor), so a
        /// sunk box has a resting place and the float-vs-sink discriminator
        /// compares two settled heights instead of two fall distances.
        Entity SpawnGround()
        {
            Entity ground = GetScene().CreateEntity("Ground");
            ground.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
            auto& rb = ground.AddComponent<Rigidbody3DComponent>();
            rb.m_Type = BodyType3D::Static;
            auto& collider = ground.AddComponent<BoxCollider3DComponent>();
            collider.m_HalfExtents = { 5.0f, 0.5f, 5.0f };
            return ground;
        }

        /// Dynamic unit-test box (side 2*kBoxHalf) at `y`, with the given mass.
        Entity SpawnBox(const char* name, f32 y, f32 mass)
        {
            Entity box = GetScene().CreateEntity(name);
            box.GetComponent<TransformComponent>().Translation = { 0.0f, y, 0.0f };
            auto& rb = box.AddComponent<Rigidbody3DComponent>();
            rb.m_Type = BodyType3D::Dynamic;
            rb.m_Mass = mass;
            auto& collider = box.AddComponent<BoxCollider3DComponent>();
            collider.m_HalfExtents = { kBoxHalf, kBoxHalf, kBoxHalf };
            return box;
        }

        [[nodiscard]] f32 Y(Entity entity)
        {
            return entity.GetComponent<TransformComponent>().Translation.y;
        }

        /// Live particle count of a fluid entity's CPU solver instance.
        [[nodiscard]] u32 ParticleCount(Entity fluidEntity)
        {
            FluidWorld* world = GetScene().TryGetFluidWorld();
            if (!world)
            {
                return 0;
            }
            FluidInstance* instance = world->Find(fluidEntity.GetComponent<IDComponent>().ID);
            if (!instance || !instance->Cpu)
            {
                return 0;
            }
            return instance->Cpu->GetCount();
        }
    };

    TEST_F(FluidCouplingTest, PrefilledPoolSpawnsParticles)
    {
        Entity pool = SpawnPool();
        EnablePhysics3D();
        RunFrames(2);

        const u32 count = ParticleCount(pool);
        EXPECT_GT(count, 200u) << "prefill produced implausibly few particles";
        EXPECT_LT(count, 4096u);
    }

    TEST_F(FluidCouplingTest, LightBoxFloatsDenseBoxSinks)
    {
        Entity pool = SpawnPool();
        SpawnGround();
        // Mostly submerged start (water surface ~y = 0.9 after settling).
        // Displaced water for the 0.4 m box is ~64 kg: 15 kg floats, 500 kg
        // sinks to the ground (top face y = 0, so its centre rests ~0.2).
        Entity light = SpawnBox("LightBox", 0.7f, 15.0f);
        Entity dense = SpawnBox("DenseBox", 0.7f, 500.0f);
        // Separate them laterally so they do not collide with each other.
        light.GetComponent<TransformComponent>().Translation.x = -0.5f;
        dense.GetComponent<TransformComponent>().Translation.x = 0.5f;
        EnablePhysics3D();

        TickFor(4.0f);

        ASSERT_TRUE(std::isfinite(Y(light)));
        ASSERT_TRUE(std::isfinite(Y(dense)));

        // The dense box must end clearly below the light one — the sign of the
        // coupling, independent of its exact magnitude. (Both would otherwise
        // rest identically on the ground.)
        EXPECT_GT(Y(light), Y(dense) + 0.15f)
            << "light=" << Y(light) << " dense=" << Y(dense);

        // Absolute sanity: the dense box reached the ground region (rest is
        // centre ~0.2); the light box is held up in the fluid column, clearly
        // above ground rest. It floats DEEP — a 15 kg box displacing 64 kg
        // floats mostly submerged, and the coarse 0.2 m particle resolution
        // costs a little more — so the bound is ground-rest + one box half,
        // not the waterline.
        EXPECT_LT(Y(dense), 0.4f);
        EXPECT_GT(Y(light), 0.4f);
        EXPECT_GT(ParticleCount(pool), 200u) << "pool lost its particles";
    }

    TEST_F(FluidCouplingTest, DisabledFluidIsFreeFall)
    {
        Entity pool = SpawnPool();
        pool.GetComponent<FluidComponent>().m_Enabled = false;
        Entity box = SpawnBox("ControlBox", 0.45f, 3.0f);
        EnablePhysics3D();

        TickFor(1.0f);

        // No fluid, no floor: after 1 s the box is in free fall far below its
        // spawn (about -4.9 m of drop, minus solver details).
        ASSERT_TRUE(std::isfinite(Y(box)));
        EXPECT_LT(Y(box), -1.0f);
        EXPECT_EQ(ParticleCount(pool), 0u) << "disabled fluid must not simulate";
    }

    TEST_F(FluidCouplingTest, KillVolumeDrainsPool)
    {
        Entity pool = SpawnPool();
        Entity drain = GetScene().CreateEntity("Drain");
        drain.GetComponent<TransformComponent>().Translation = { 0.0f, 0.75f, 0.0f };
        auto& kill = drain.AddComponent<FluidKillVolumeComponent>();
        kill.m_HalfExtents = { 1.0f, 1.0f, 1.0f }; // swallows the whole domain
        EnablePhysics3D();

        RunFrames(3);
        EXPECT_EQ(ParticleCount(pool), 0u);
    }

    TEST_F(FluidCouplingTest, EmitterFillsEmptyDomain)
    {
        Entity pool = SpawnPool();
        pool.GetComponent<FluidComponent>().m_PrefillFraction = 0.0f;

        Entity emitter = GetScene().CreateEntity("Emitter");
        // Inside the domain, pointing straight down (rotate -Z forward onto -Y).
        emitter.GetComponent<TransformComponent>().Translation = { 0.0f, 1.2f, 0.0f };
        emitter.GetComponent<TransformComponent>().SetRotationEuler({ -glm::half_pi<f32>(), 0.0f, 0.0f });
        auto& ec = emitter.AddComponent<FluidEmitterComponent>();
        ec.m_Rate = 600.0f;
        ec.m_Speed = 2.0f;
        EnablePhysics3D();

        RunFrames(2);
        const u32 early = ParticleCount(pool);
        RunFrames(28);
        const u32 late = ParticleCount(pool);

        EXPECT_GT(late, 100u) << "600/s for 0.5 s should exceed 100 particles";
        EXPECT_GT(late, early) << "particle count must grow while the emitter runs";
    }
} // namespace OloEngine::Functional
