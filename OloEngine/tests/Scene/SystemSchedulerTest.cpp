// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// SystemSchedulerTest
//
// Contract tests for OloEngine::SystemScheduler (issue #453) — the declarative
// dependency-graph scheduler that replaced the hard-coded OnUpdateRuntime call
// chain. Two things are pinned here:
//
//   1. The derivation is correct and deterministic: no constraints => execution
//      order equals registration order; explicit before/after edges and derived
//      read/write (RAW/WAW/WAR) edges reorder as declared; a cycle / dangling
//      reference / duplicate name is a LOUD SystemSchedulerError, never a silent
//      skip (plan step 6).
//
//   2. The engine's real gameplay schedule (Scene::GetGameplayScheduler) derives
//      the exact historical SimulateRuntimeStep sequence — proving the refactor
//      preserves today's single-threaded ordering bit-for-bit (acceptance
//      criterion for the first slice).
//
// The read/write model mirrors the RenderGraph's per-resource hazard derivation,
// so resource-derived edges always point forward in registration order and can
// never by themselves form a cycle. The tests exploit that: a resource edge is
// proven to exist by adding a contradictory explicit After() and asserting the
// combination is now a cycle (the resource edge is the other half of the loop).
// =============================================================================

#include "OloEngine/Scene/SystemScheduler.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Task/Scheduler.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

namespace
{
    // A no-op exec: the ordering tests only inspect GetOrderedNames(), never run.
    SystemScheduler::ExecFn NoOp()
    {
        return [](Scene&, Timestep) {};
    }
} // namespace

TEST(SystemSchedulerTest, EmptySchedulerBuildsAndReportsNoSystems)
{
    SystemScheduler sched;
    EXPECT_NO_THROW(sched.Build());
    EXPECT_EQ(sched.SystemCount(), 0u);
    EXPECT_TRUE(sched.GetOrderedNames().empty());
}

TEST(SystemSchedulerTest, NoConstraintsPreservesRegistrationOrder)
{
    SystemScheduler sched;
    sched.AddSystem("A", NoOp());
    sched.AddSystem("B", NoOp());
    sched.AddSystem("C", NoOp());

    const std::vector<std::string> expected{ "A", "B", "C" };
    EXPECT_EQ(sched.GetOrderedNames(), expected);
}

TEST(SystemSchedulerTest, AfterConstraintReordersAgainstRegistrationOrder)
{
    // A declares it runs after C. The lowest-index unconstrained node (B) still
    // comes first; then C, then A. This is a genuine reorder away from [A,B,C].
    SystemScheduler sched;
    sched.AddSystem("A", NoOp()).After("C");
    sched.AddSystem("B", NoOp());
    sched.AddSystem("C", NoOp());

    const std::vector<std::string> expected{ "B", "C", "A" };
    EXPECT_EQ(sched.GetOrderedNames(), expected);
}

TEST(SystemSchedulerTest, BeforeConstraintReordersAgainstRegistrationOrder)
{
    // C declares it runs before A: edge C -> A. B is unconstrained.
    SystemScheduler sched;
    sched.AddSystem("A", NoOp());
    sched.AddSystem("B", NoOp());
    sched.AddSystem("C", NoOp()).Before("A");

    const std::vector<std::string> expected{ "B", "C", "A" };
    EXPECT_EQ(sched.GetOrderedNames(), expected);
}

TEST(SystemSchedulerTest, ExplicitAfterAndBeforeAgreeOnTheSameEdge)
{
    SystemScheduler sched;
    sched.AddSystem("Writer", NoOp()).Before("Reader");
    sched.AddSystem("Reader", NoOp()).After("Writer");

    const std::vector<std::string> expected{ "Writer", "Reader" };
    EXPECT_EQ(sched.GetOrderedNames(), expected);
}

TEST(SystemSchedulerTest, ReadAfterWriteEdgeExists)
{
    // Producer writes R, Consumer reads R => derived edge Producer -> Consumer.
    // Prove the edge exists by adding the reverse explicit edge and asserting the
    // pair is now a cycle (Producer -> Consumer -> Producer).
    SystemScheduler sched;
    sched.AddSystem("Producer", NoOp()).Writes("R").After("Consumer");
    sched.AddSystem("Consumer", NoOp()).Reads("R");
    EXPECT_THROW(sched.Build(), SystemSchedulerError);
}

TEST(SystemSchedulerTest, WriteAfterReadEdgeExists)
{
    // Reader reads R, then Writer writes R => derived WAR edge Reader -> Writer.
    SystemScheduler sched;
    sched.AddSystem("Reader", NoOp()).Reads("R");
    sched.AddSystem("Writer", NoOp()).Writes("R").Before("Reader");
    EXPECT_THROW(sched.Build(), SystemSchedulerError);
}

TEST(SystemSchedulerTest, WriteAfterWriteEdgeExists)
{
    // Two writers of R => derived WAW edge First -> Second.
    SystemScheduler sched;
    sched.AddSystem("First", NoOp()).Writes("R").After("Second");
    sched.AddSystem("Second", NoOp()).Writes("R");
    EXPECT_THROW(sched.Build(), SystemSchedulerError);
}

TEST(SystemSchedulerTest, ReadOnlySystemsSharingAResourceAreIndependent)
{
    // Two readers (no writer) of the same resource impose no edge on each other,
    // so registration order stands. This is the parallelism seam the gameplay
    // Audio/Particles/Snow systems rely on.
    SystemScheduler sched;
    sched.AddSystem("R2", NoOp()).Reads("World");
    sched.AddSystem("R1", NoOp()).Reads("World");

    const std::vector<std::string> expected{ "R2", "R1" };
    EXPECT_EQ(sched.GetOrderedNames(), expected);
}

TEST(SystemSchedulerTest, ReadModifyWriteDoesNotSelfDeadlock)
{
    // A system that both reads and writes the same resource must not create a
    // self-edge (that would be an unbreakable cycle).
    SystemScheduler sched;
    sched.AddSystem("Physics", NoOp()).ReadsWrites("Transforms");
    EXPECT_NO_THROW(sched.Build());
    ASSERT_EQ(sched.GetOrderedNames().size(), 1u);
    EXPECT_EQ(sched.GetOrderedNames()[0], "Physics");
}

TEST(SystemSchedulerTest, DirectCycleThrows)
{
    SystemScheduler sched;
    sched.AddSystem("A", NoOp()).After("B");
    sched.AddSystem("B", NoOp()).After("A");
    EXPECT_THROW(sched.Build(), SystemSchedulerError);
}

TEST(SystemSchedulerTest, IndirectCycleThrows)
{
    SystemScheduler sched;
    sched.AddSystem("A", NoOp()).After("C");
    sched.AddSystem("B", NoOp()).After("A");
    sched.AddSystem("C", NoOp()).After("B");
    EXPECT_THROW(sched.Build(), SystemSchedulerError);
}

TEST(SystemSchedulerTest, DanglingAfterReferenceThrows)
{
    SystemScheduler sched;
    sched.AddSystem("A", NoOp()).After("DoesNotExist");
    EXPECT_THROW(sched.Build(), SystemSchedulerError);
}

TEST(SystemSchedulerTest, DanglingBeforeReferenceThrows)
{
    SystemScheduler sched;
    sched.AddSystem("A", NoOp()).Before("DoesNotExist");
    EXPECT_THROW(sched.Build(), SystemSchedulerError);
}

TEST(SystemSchedulerTest, DuplicateSystemNameThrows)
{
    SystemScheduler sched;
    sched.AddSystem("A", NoOp());
    sched.AddSystem("A", NoOp());
    EXPECT_THROW(sched.Build(), SystemSchedulerError);
}

TEST(SystemSchedulerTest, ExecuteRunsEverySystemOnceInDerivedOrder)
{
    std::vector<std::string> log;
    const auto record = [&log](std::string name) -> SystemScheduler::ExecFn
    {
        return [&log, name = std::move(name)](Scene&, Timestep)
        { log.push_back(name); };
    };

    SystemScheduler sched;
    sched.AddSystem("A", record("A")).After("C");
    sched.AddSystem("B", record("B"));
    sched.AddSystem("C", record("C"));

    // Execute takes a Scene& (the real gameplay execs use it); our recording
    // lambdas ignore it, but we still pass a live Scene so nothing binds a null
    // reference. Scene::Create() is the same construction the Functional harness
    // and the sibling unit benchmark use.
    Ref<Scene> scene = Scene::Create();
    sched.Execute(*scene, Timestep{ 0.016f });

    const std::vector<std::string> expected{ "B", "C", "A" };
    EXPECT_EQ(log, expected);
}

// ── The canonical-order acceptance test ──────────────────────────────────────
// The derived gameplay-system order must equal this canonical sequence exactly.
// If a future edit reorders a system or adds/removes one, this pins the change
// to a visible diff instead of a silent behavior shift.
//
// The canonical order is the pre-#453 hard-coded sequence with two DELIBERATE
// deltas (the physics kick/fence split — UE TG_DuringPhysics analog):
//   * "Physics" became PhysicsKick → PhysicsFence, with the ECS-free world step
//     running as a task in between.
//   * Dialogue (historically pre-animation) and Quest (historically post-AI /
//     pre-Abilities) moved into the physics shadow between kick and fence: both
//     are transform- and physics-independent, so the only observable change is
//     their slot relative to systems they share no data with.
TEST(SystemSchedulerTest, GameplayScheduleMatchesCanonicalOrder)
{
    const std::vector<std::string> expected{
        "Scripts",
        "Cinematics",
        "Locomotion",
        "Retargeting",
        "Animation",
        "AnimationGraph",
        "RootMotionApply",
        "MorphEval",
        "Fluid",
        "PhysicsKick",
        "Dialogue",
        "Quest",
        "PhysicsFence",
        "PropagateTransforms",
        "Navigation",
        "SpatialIndex",
        "Perception",
        "AI",
        "Inventory",
        "Abilities",
        "Audio",
        "ParticlesCPU",
        "ParticlesGPU",
        "SnowDeformers",
    };
    EXPECT_EQ(Scene::GetGameplaySystemOrderForTesting(), expected);
}

TEST(SystemSchedulerTest, DependsOnReportsTransitiveReachability)
{
    SystemScheduler sched;
    sched.AddSystem("A", NoOp()).Writes("X");
    sched.AddSystem("B", NoOp()).Reads("X").Writes("Y");
    sched.AddSystem("C", NoOp()).Reads("Y");
    sched.AddSystem("Lone", NoOp());

    EXPECT_TRUE(sched.DependsOn("B", "A"));  // direct RAW edge
    EXPECT_TRUE(sched.DependsOn("C", "A"));  // transitive, two hops
    EXPECT_FALSE(sched.DependsOn("A", "C")); // wrong direction
    EXPECT_FALSE(sched.DependsOn("Lone", "A"));
    EXPECT_FALSE(sched.DependsOn("A", "A")); // no self-dependency
    EXPECT_THROW((void)sched.DependsOn("Typo", "A"), SystemSchedulerError);
}

// The critical cross-subsystem seams the historical comments call out must hold
// as dependency PATHS in the derived graph, not merely as positions in the
// sequential order. A position check can be satisfied by the registration-order
// tie-break even when the edge is missing — and a missing edge is exactly the
// silent gap that becomes a race once independent systems run concurrently
// (DependsOn == "can never overlap under the parallel executor").
TEST(SystemSchedulerTest, GameplayScheduleHonoursDocumentedSeams)
{
    SystemScheduler& sched = Scene::GetGameplayScheduler();

    EXPECT_TRUE(sched.DependsOn("Cinematics", "Scripts"));   // authored transforms win
    EXPECT_TRUE(sched.DependsOn("Animation", "Cinematics")); // animation sees posed entities
    EXPECT_TRUE(sched.DependsOn("MorphEval", "Animation"));  // morph eval after weight writers
    EXPECT_TRUE(sched.DependsOn("MorphEval", "AnimationGraph"));

    // Locomotion seam (issue #631 part 4): the graph evaluation consumes the
    // parameters the controller writes (RAW on AnimationParams).
    EXPECT_TRUE(sched.DependsOn("AnimationGraph", "Locomotion"));

    // Live-retargeting seam (issue #631 part 2): both animation systems sample
    // the clip lists the bake writes (RAW on AnimationClips).
    EXPECT_TRUE(sched.DependsOn("Animation", "Retargeting"));
    EXPECT_TRUE(sched.DependsOn("AnimationGraph", "Retargeting"));

    // Root-motion seam (issue #631): the applier consumes the deltas both
    // animation systems publish (RAW on RootMotion), and PhysicsKick's
    // transform read is ordered after the applier's transform write — that is
    // the edge guaranteeing a character controller integrates THIS tick's
    // extracted motion, not last tick's.
    EXPECT_TRUE(sched.DependsOn("RootMotionApply", "Animation"));
    EXPECT_TRUE(sched.DependsOn("RootMotionApply", "AnimationGraph"));
    EXPECT_TRUE(sched.DependsOn("PhysicsKick", "RootMotionApply"));
    EXPECT_TRUE(sched.DependsOn("PhysicsFence", "RootMotionApply"));
    EXPECT_TRUE(sched.DependsOn("PropagateTransforms", "RootMotionApply"));

    // Physics kick/fence: the kick consumes posed transforms (buoyancy +
    // character/vehicle phases), the fence joins the world step and overwrites
    // the transforms — so the fence must come after the kick AND after every
    // pre-physics transform reader.
    EXPECT_TRUE(sched.DependsOn("PhysicsKick", "Cinematics"));
    EXPECT_TRUE(sched.DependsOn("PhysicsFence", "PhysicsKick"));
    EXPECT_TRUE(sched.DependsOn("PhysicsFence", "Animation"));

    // Fluid queues coupling impulses on Jolt bodies (the kBodyForces channel),
    // which the kick's world step integrates — the queue-before-step contract
    // (issue #630). Fluid reads posed transforms, so it also sits after the
    // authored-transform writers.
    EXPECT_TRUE(sched.DependsOn("PhysicsKick", "Fluid"));
    EXPECT_TRUE(sched.DependsOn("Fluid", "Cinematics"));

    // The physics shadow's legality is the ABSENCE of paths: Dialogue and Quest
    // must be unordered against both physics nodes (they may run on the game
    // thread while the world step is in flight), in both directions.
    EXPECT_FALSE(sched.DependsOn("Dialogue", "PhysicsKick"));
    EXPECT_FALSE(sched.DependsOn("PhysicsFence", "Dialogue"));
    EXPECT_FALSE(sched.DependsOn("Quest", "PhysicsKick"));
    EXPECT_FALSE(sched.DependsOn("PhysicsFence", "Quest"));

    // Post-physics consumers: every transform reader/writer downstream of the
    // fence, in the documented relative order.
    EXPECT_TRUE(sched.DependsOn("PropagateTransforms", "PhysicsFence")); // compose after movers (#499)
    EXPECT_TRUE(sched.DependsOn("Navigation", "PropagateTransforms"));   // nav writes after compose read
    EXPECT_TRUE(sched.DependsOn("SpatialIndex", "Navigation"));          // index sees nav-moved agents
    EXPECT_TRUE(sched.DependsOn("Perception", "SpatialIndex"));          // perception queries the index
    EXPECT_TRUE(sched.DependsOn("AI", "Perception"));                    // AI consumes fresh sensor data
    EXPECT_TRUE(sched.DependsOn("Inventory", "PhysicsFence"));           // pickup proximity reads post-physics transforms
    EXPECT_TRUE(sched.DependsOn("Audio", "PhysicsFence"));               // pose sync reads post-physics transforms
    EXPECT_TRUE(sched.DependsOn("Audio", "Navigation"));
    EXPECT_TRUE(sched.DependsOn("ParticlesCPU", "PhysicsFence"));
    EXPECT_TRUE(sched.DependsOn("ParticlesGPU", "PhysicsFence"));
    EXPECT_TRUE(sched.DependsOn("SnowDeformers", "PhysicsFence"));

    // And the deliberate independence the parallel executor exploits: the
    // marked systems (and the transform read-only tail) have no path between
    // one another. ParticlesCPU (marked) must be unordered against the other
    // marked systems (Abilities, Audio) — that independence is what lets it run
    // on a worker (issue #576). ParticlesGPU is an unmarked barrier, so it is
    // free to be unordered here too; the executor still joins tasks before it.
    EXPECT_FALSE(sched.DependsOn("ParticlesCPU", "Audio"));
    EXPECT_FALSE(sched.DependsOn("Audio", "ParticlesCPU"));
    EXPECT_FALSE(sched.DependsOn("ParticlesCPU", "Abilities"));
    EXPECT_FALSE(sched.DependsOn("Abilities", "ParticlesCPU"));
    EXPECT_FALSE(sched.DependsOn("ParticlesCPU", "ParticlesGPU"));
    EXPECT_FALSE(sched.DependsOn("ParticlesGPU", "ParticlesCPU"));
    EXPECT_FALSE(sched.DependsOn("SnowDeformers", "ParticlesCPU"));
    EXPECT_FALSE(sched.DependsOn("Audio", "Abilities"));
    EXPECT_FALSE(sched.DependsOn("Abilities", "Audio"));
}

// ── Parallel execution ───────────────────────────────────────────────────────
// These run with real FScheduler workers (same fixture shape as TaskTestBase in
// TaskSystemTest.cpp) so the task-dispatch path is genuinely concurrent; with
// zero workers Tasks::Launch degrades to inline execution and the tests would
// pass vacuously.
class SystemSchedulerParallelTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        LowLevelTasks::FScheduler::Get().StartWorkers();
    }

    static void TearDownTestSuite()
    {
        LowLevelTasks::FScheduler::Get().StopWorkers();
    }

    void SetUp() override
    {
        m_PreviousEnabled = SystemScheduler::IsParallelExecutionEnabled();
        SystemScheduler::SetParallelExecutionEnabled(true);
        m_Scene = Scene::Create();
        m_Scene->SetRenderingEnabled(false);
    }

    void TearDown() override
    {
        SystemScheduler::SetParallelExecutionEnabled(m_PreviousEnabled);
        m_Scene.Reset();
    }

    Ref<Scene> m_Scene;
    bool m_PreviousEnabled = true;
};

TEST_F(SystemSchedulerParallelTest, ParallelSystemsAllRunOnceAndExecuteIsReusable)
{
    std::atomic<u32> counter{ 0 };
    SystemScheduler sched;
    for (int i = 0; i < 8; ++i)
    {
        sched.AddSystem("P" + std::to_string(i),
                        [&counter](Scene&, Timestep)
                        { counter.fetch_add(1, std::memory_order_relaxed); })
            .Parallelizable();
    }

    sched.Execute(*m_Scene, Timestep{ 0.016f });
    EXPECT_EQ(counter.load(), 8u);

    // The in-flight bookkeeping must reset between runs.
    sched.Execute(*m_Scene, Timestep{ 0.016f });
    EXPECT_EQ(counter.load(), 16u);
}

TEST_F(SystemSchedulerParallelTest, UnmarkedSystemIsABarrierForInFlightParallelWork)
{
    // Parallel A finishes late; unmarked B must observe A's side effect (the
    // executor joins ALL in-flight tasks before an unmarked system runs) and B
    // itself must run on the calling thread.
    std::atomic<bool> parallelDone{ false };
    std::atomic<bool> barrierObservedDone{ false };
    const std::thread::id callerThread = std::this_thread::get_id();
    std::thread::id barrierThread;

    SystemScheduler sched;
    sched.AddSystem("SlowParallel",
                    [&parallelDone](Scene&, Timestep)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        parallelDone.store(true, std::memory_order_release);
                    })
        .Parallelizable();
    sched.AddSystem("MainBarrier",
                    [&](Scene&, Timestep)
                    {
                        barrierObservedDone.store(parallelDone.load(std::memory_order_acquire));
                        barrierThread = std::this_thread::get_id();
                    });

    sched.Execute(*m_Scene, Timestep{ 0.016f });

    EXPECT_TRUE(barrierObservedDone.load());
    EXPECT_EQ(barrierThread, callerThread);
}

TEST_F(SystemSchedulerParallelTest, EdgesBetweenParallelSystemsAreHonoured)
{
    // Producer and Consumer are BOTH parallel-marked, with a derived RAW edge
    // between them. The executor must not launch Consumer while Producer is in
    // flight, even though both are eligible for worker dispatch.
    std::atomic<bool> producerDone{ false };
    std::atomic<bool> consumerSawProducer{ false };

    SystemScheduler sched;
    sched.AddSystem("Producer",
                    [&producerDone](Scene&, Timestep)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        producerDone.store(true, std::memory_order_release);
                    })
        .Writes("R")
        .Parallelizable();
    sched.AddSystem("Consumer",
                    [&](Scene&, Timestep)
                    { consumerSawProducer.store(producerDone.load(std::memory_order_acquire)); })
        .Reads("R")
        .Parallelizable();

    sched.Execute(*m_Scene, Timestep{ 0.016f });

    EXPECT_TRUE(consumerSawProducer.load());
}

TEST_F(SystemSchedulerParallelTest, KillSwitchRunsParallelMarkedSystemsOnCallingThread)
{
    SystemScheduler::SetParallelExecutionEnabled(false);

    const std::thread::id callerThread = std::this_thread::get_id();
    std::vector<std::thread::id> observed(3);
    SystemScheduler sched;
    for (int i = 0; i < 3; ++i)
    {
        sched.AddSystem("P" + std::to_string(i),
                        [&observed, i](Scene&, Timestep)
                        { observed[static_cast<sizet>(i)] = std::this_thread::get_id(); })
            .Parallelizable();
    }

    sched.Execute(*m_Scene, Timestep{ 0.016f });

    for (const std::thread::id& id : observed)
    {
        EXPECT_EQ(id, callerThread);
    }
}

TEST_F(SystemSchedulerParallelTest, ExceptionInParallelSystemRethrowsOnCallerAfterJoin)
{
    std::atomic<u32> othersRan{ 0 };
    SystemScheduler sched;
    sched.AddSystem("Thrower",
                    [](Scene&, Timestep)
                    { throw std::runtime_error("boom"); })
        .Parallelizable();
    sched.AddSystem("Bystander",
                    [&othersRan](Scene&, Timestep)
                    { othersRan.fetch_add(1, std::memory_order_relaxed); })
        .Parallelizable();

    EXPECT_THROW(sched.Execute(*m_Scene, Timestep{ 0.016f }), std::runtime_error);
    // The join must complete before the rethrow — the bystander still ran.
    EXPECT_EQ(othersRan.load(), 1u);
}

// ── The parallel acceptance test ─────────────────────────────────────────────
// A real gameplay tick through Scene::OnUpdateRuntime must produce identical
// state whether the parallel-marked systems (Audio, Abilities) run as tasks or
// sequentially — the executor may only change WHERE work runs, never WHAT it
// computes. Ability cooldowns are the observable: they tick inside Abilities
// (parallel-marked) every frame.
namespace
{
    f32 TickAbilityCooldownScene(bool parallelEnabled, u32 tickCount)
    {
        const bool previous = SystemScheduler::IsParallelExecutionEnabled();
        SystemScheduler::SetParallelExecutionEnabled(parallelEnabled);

        Ref<Scene> scene = Scene::Create();
        scene->SetRenderingEnabled(false);
        Entity caster = scene->CreateEntity("Caster");
        auto& ability = caster.AddComponent<AbilityComponent>();
        ability.InitializeDefaultRPGAttributes(100.0f, 50.0f, 10.0f, 5.0f);
        ability.Cooldowns.StartCooldown(GameplayTag("Ability.Fireball"), 1.0f);

        for (u32 i = 0; i < tickCount; ++i)
        {
            scene->OnUpdateRuntime(Timestep{ 1.0f / 60.0f });
        }

        const f32 remaining =
            caster.GetComponent<AbilityComponent>().Cooldowns.GetRemainingCooldown(GameplayTag("Ability.Fireball"));
        SystemScheduler::SetParallelExecutionEnabled(previous);
        return remaining;
    }
} // namespace

TEST_F(SystemSchedulerParallelTest, GameplayTickParallelMatchesSequential)
{
    const f32 sequential = TickAbilityCooldownScene(false, 30);
    const f32 parallel = TickAbilityCooldownScene(true, 30);

    // Same float ops in the same order, only on a different thread — the results
    // must match exactly (EXPECT_FLOAT_EQ = 4 ULP, per the no-raw-== float rule).
    EXPECT_FLOAT_EQ(sequential, parallel);
    // And the cooldown genuinely ticked: 30 frames at 1/60 s ≈ 0.5 s consumed.
    EXPECT_GT(sequential, 0.0f);
    EXPECT_LT(sequential, 1.0f);
}

// ── ParticlesCPU parallel acceptance (issue #576) ────────────────────────────
// The CPU particle partition is now Parallelizable — it runs on a worker thread
// while Audio/Abilities run. Because each ParticleSystem draws from its own
// per-system RNG (seeded from run-seed × UUID, issue #452), the emitted stream
// must be bit-identical whether ParticlesCPU is dispatched to a worker or run
// inline. This drives real workers (the fixture StartWorkers()) so the parallel
// path is genuinely concurrent, then compares the exact particle pools.
namespace
{
    std::vector<u32> TickParticleScene(bool parallelEnabled, u32 tickCount)
    {
        const bool previous = SystemScheduler::IsParallelExecutionEnabled();
        SystemScheduler::SetParallelExecutionEnabled(parallelEnabled);

        Ref<Scene> scene = Scene::Create();
        scene->SetRenderingEnabled(false);

        Entity camera = scene->CreateEntity("Camera");
        camera.AddComponent<CameraComponent>().Primary = true;

        // Several CPU emitters with variance on every RNG-driven field + a Sphere
        // shape, each seeded from a distinct per-system stream.
        constexpr u32 kEmitters = 4;
        std::vector<Entity> emitters;
        emitters.reserve(kEmitters);
        for (u32 e = 0; e < kEmitters; ++e)
        {
            Entity emitter = scene->CreateEntity("Emitter" + std::to_string(e));
            emitter.GetComponent<TransformComponent>().Translation = { static_cast<f32>(e), 0.0f, 0.0f };
            auto& sys = emitter.AddComponent<ParticleSystemComponent>().System;
            sys.Playing = true;
            sys.Looping = true;
            sys.Duration = 1000.0f;
            sys.UseGPU = false;
            sys.Emitter.RateOverTime = 30.0f;
            sys.Emitter.InitialSpeed = 2.0f;
            sys.Emitter.SpeedVariance = 1.0f;
            sys.Emitter.SizeVariance = 0.3f;
            sys.Emitter.RotationVariance = 2.0f;
            sys.Emitter.LifetimeMin = 1.0f;
            sys.Emitter.LifetimeMax = 2.0f;
            sys.Emitter.Shape = EmitSphere{ 1.5f };
            sys.SeedRandom(ParticleSystem::DeriveSeed(0x00ABCDEFu, e + 1));
            emitters.push_back(emitter);
        }

        for (u32 i = 0; i < tickCount; ++i)
        {
            scene->OnUpdateRuntime(Timestep{ 1.0f / 60.0f });
        }

        std::vector<u32> sig;
        const auto push = [&sig](f32 v)
        { sig.push_back(std::bit_cast<u32>(v)); };
        for (Entity emitter : emitters)
        {
            const ParticleSystem& sys = emitter.GetComponent<ParticleSystemComponent>().System;
            const ParticlePool& pool = sys.GetPool();
            const u32 count = sys.GetAliveCount();
            sig.push_back(count);
            for (u32 i = 0; i < count; ++i)
            {
                push(pool.m_Positions[i].x);
                push(pool.m_Positions[i].y);
                push(pool.m_Positions[i].z);
                push(pool.m_Velocities[i].x);
                push(pool.m_Velocities[i].y);
                push(pool.m_Velocities[i].z);
                push(pool.m_Sizes[i]);
                push(pool.m_Rotations[i]);
                push(pool.m_Lifetimes[i]);
            }
        }

        SystemScheduler::SetParallelExecutionEnabled(previous);
        return sig;
    }
} // namespace

TEST_F(SystemSchedulerParallelTest, ParticleTickParallelMatchesSequential)
{
    const std::vector<u32> sequential = TickParticleScene(false, 30);
    const std::vector<u32> parallel = TickParticleScene(true, 30);

    ASSERT_GT(sequential[0], 5u) << "sanity: emitters should have produced a non-trivial pool";
    EXPECT_EQ(sequential, parallel)
        << "ParticlesCPU produced a different particle stream on a worker thread than inline — "
           "the per-system RNG is not fully isolated from execution placement (#576/#452)";
}
