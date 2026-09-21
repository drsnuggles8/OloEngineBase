#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// AnimalPopulationViaSceneTickTest — Functional Test, issue #1258.
//
// Cross-subsystem seam under test:
//   Scene::OnUpdateRuntime × SystemScheduler ("AnimalPaths") ×
//   Scene::ScheduleAnimalPopulationForFrame × AnimalScheduler ×
//   Scene::UpdateAnimation's deformation gate × AnimalPathComponent.
//
// AnimalSchedulerContractTest pins the allocator in isolation, against work
// items a test built by hand. This file pins what only a real Scene tick can
// show: that the components are actually GATHERED, that the schedule is
// actually SPENT on the animation tick rate, and that the population's motion
// reproduces through the real fixed-timestep loop rather than only through the
// closed form in a unit test.
//
// The distinction matters more here than usual. The scheduler is a pure
// function, so a unit test can prove every property it has; what a unit test
// cannot prove is that anything CALLS it — and "an isolated data structure left
// as completion" is precisely the failure this issue's delivery contract names.
// Each case below would still pass if the allocator were perfect and the wiring
// were absent, EXCEPT that it reads the wiring's observable effects: the stats
// block Scene publishes, and the clip time the gate advanced.
//
// The criteria map onto the cases:
//
//   1. "Population ... with reproducible counts and trajectories." —
//      RepeatedRunsPlaceThePopulationIdentically and
//      ThePathIsFrameRateIndependent. The second is the load-bearing one: it
//      runs the same population at 60 Hz and at 240 Hz and demands the same
//      positions at the same SIMULATED time, which an accumulated path cannot
//      do and a closed-form one cannot fail.
//
//   3. "Per-animal/group budget policies preserve hero quality and avoid
//      starvation, abrupt motion changes or invisible distant coats." —
//      TheHeroIsNeverCoarsenedThroughARealTick and
//      NoAnimalStarvesThroughARealTick, driven through the scene rather than
//      through hand-built items.
//
//   4. "Expose CPU/GPU time, memory, simulation/update frequencies and
//      frame-time tails across tested quality tiers." — the update-frequency
//      half is TheDeformationBudgetActuallyReducesTheTickRate, which counts
//      real AnimationSystem::Update calls by watching the clip time. GPU time
//      and memory are live-only and are reported in the PR's matrix.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/AnimalScheduler.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace OloEngine::Functional
{
    namespace
    {
        constexpr sizet kDeform = static_cast<sizet>(AnimalWorkAxis::Deformation);
        constexpr sizet kVis = static_cast<sizet>(AnimalWorkAxis::Visibility);

        constexpr u32 kHerdCount = 40u;
    } // namespace

    /// A population with one hero and a herd, all budgeted, all on paths.
    ///
    /// NO GROOM ASSETS AND NO MESHES. This fixture is headless and deliberately
    /// asset-free: what it exercises is the gather → schedule → spend wiring,
    /// and loading real grooms would make the case depend on an asset manager,
    /// a cook and a GL context for a property none of the three affects. The
    /// strand counts the budget prices come from the entities' groom
    /// components, so the ones that matter are set directly.
    class AnimalPopulationFixture : public FunctionalTest
    {
      protected:
        // RendererSettings IS PROCESS-WIDE, and this fixture writes it. Without
        // a restore, every later case in the same binary inherits a 700-unit
        // animal budget and an enabled scheduler — which is invisible until
        // some unrelated test starts failing depending on gtest's ordering.
        // Captured before BuildScene runs and put back in TearDown.
        void SetUp() override
        {
            m_SavedSettings = Renderer3D::GetRendererSettings();
            FunctionalTest::SetUp();
        }

        void TearDown() override
        {
            Renderer3D::GetRendererSettings() = m_SavedSettings;
            FunctionalTest::TearDown();
        }

        void BuildScene() override
        {
            EnableAnimation();

            // THE BUDGET IS TURNED ON HERE, IN THE TEST, and not relied on from
            // a default. RendererSettings is a process-wide struct that another
            // case in the same binary may have moved, and a test whose subject
            // is "the budget did something" must not be able to pass or fail on
            // the order the suite happened to run in.
            RendererSettings& settings = Renderer3D::GetRendererSettings();
            settings.AnimalSchedulingEnabled = true;
            settings.AnimalProtectHero = true;
            // A BUDGET THE POPULATION CANNOT FIT INSIDE, and it has to be. This
            // fixture is headless, so the only axis that carries any cost is
            // Deformation: no groom asset resolves without an asset manager, so
            // every animal's strand and guide counts gather as zero. Forty-one
            // 64-bone skeletons price at roughly 1 100 units, so a 6 000-unit
            // allowance would fit comfortably, nothing would ever be coarsened,
            // and every case below asserting "the hero was not coarsened" or
            // "nobody starved" would pass without the budget having done
            // anything at all.
            settings.AnimalFrameBudgetUnits = 700.0f;
            settings.AnimalHoldFrames = 0u;
            settings.AnimalStarvationFrames = 8u;
            settings.AnimalMinVisibleStrands = 256u;

            m_Clip = Ref<AnimationClip>::Create();
            m_Clip->Name = "Walk";
            m_Clip->Duration = 1.0f;
            m_Clip->InitializeBoneCache();

            m_Hero = MakeAnimal("Hero", AnimalRole::Hero, glm::vec3{ 0.0f, 0.0f, 4.0f }, 24000u,
                                /*withPath=*/false);

            for (u32 i = 0; i < kHerdCount; ++i)
            {
                const f32 depth = -6.0f - (static_cast<f32>(i) * 1.5f);
                const f32 lateral = static_cast<f32>((i % 7u)) * 2.0f - 6.0f;
                m_Herd.push_back(MakeAnimal("Herd", AnimalRole::Background,
                                            glm::vec3{ lateral, 0.0f, depth }, 12000u, /*withPath=*/true, i));
            }
        }

        Entity MakeAnimal(const std::string& tag, AnimalRole role, const glm::vec3& position, u32 strands,
                          bool withPath, u32 index = 0u)
        {
            Entity entity = GetScene().CreateEntity(tag);
            entity.GetComponent<TransformComponent>().Translation = position;

            // A REAL BONE COUNT. SkeletonData's palette size IS what the
            // deformation axis is priced per, and a default-constructed
            // skeleton has an empty one — so a fixture built from
            // Ref<Skeleton>::Create() with no argument prices the whole
            // population at zero and the budget never binds.
            auto& skeleton = entity.AddComponent<SkeletonComponent>();
            skeleton.m_Skeleton = Ref<Skeleton>::Create(sizet{ 64 });

            // A REAL CLIP, for a second and independent reason: UpdateAnimation
            // guards on `m_CurrentClip` before it reaches the budget's gate at
            // all, so a clipless entity is never ticked and the tick-rate cases
            // below would be measuring a pose that was never going to move.
            auto& animState = entity.AddComponent<AnimationStateComponent>();
            animState.m_IsPlaying = true;
            animState.m_CurrentClip = m_Clip;

            auto& groom = entity.AddComponent<GroomComponent>();
            groom.m_RenderStrands = true;
            groom.m_MaxRenderStrands = strands;

            auto& budget = entity.AddComponent<AnimalBudgetComponent>();
            budget.m_Role = static_cast<u8>(role);
            budget.m_Enabled = true;
            // A motion bound generous enough that the pose-step rule is not what
            // caps these animals — the cases below are about the BUDGET, and a
            // cap that bound first would make them pass for the wrong reason.
            budget.m_FullRateMotionMetres = 0.001f;
            budget.m_MaxDeformationSteps = 3u;

            if (withPath)
            {
                auto& path = entity.AddComponent<AnimalPathComponent>();
                path.m_RadiusX = 3.0f + static_cast<f32>(index % 5u);
                path.m_RadiusZ = 2.0f + static_cast<f32>(index % 3u);
                path.m_RateX = 0.2f + 0.01f * static_cast<f32>(index);
                path.m_RateZ = 0.3f + 0.013f * static_cast<f32>(index);
                path.m_PhaseX = 0.37f * static_cast<f32>(index);
                path.m_PhaseZ = 0.71f * static_cast<f32>(index);
            }
            return entity;
        }

        Entity m_Hero;
        std::vector<Entity> m_Herd;
        Ref<AnimationClip> m_Clip;
        RendererSettings m_SavedSettings;
    };

    /// A second, independently built population — the comparison arm for the
    /// two reproducibility cases.
    ///
    /// CONCRETE, because a gtest fixture is ABSTRACT: `TestBody` is pure
    /// virtual, so the fixture class itself cannot be instantiated. The public
    /// wrappers exist for the same reason the class does — `SetUp`, `TearDown`
    /// and the members are protected, and protected access does not reach
    /// across two different classes that merely share a base.
    struct SecondPopulation : AnimalPopulationFixture
    {
        void TestBody() override
        {
        }
        void Begin()
        {
            SetUp();
        }
        void End()
        {
            TearDown();
        }
        void Advance(u32 frames, f32 dtSeconds = 1.0f / 60.0f)
        {
            RunFrames(frames, dtSeconds);
        }
        [[nodiscard]] const std::vector<Entity>& Herd() const
        {
            return m_Herd;
        }
    };

    // -------------------------------------------------------------------------
    // Criterion 1 — reproducible counts and trajectories
    // -------------------------------------------------------------------------

    TEST_F(AnimalPopulationFixture, ThePopulationIsGatheredByTheRealSceneTick)
    {
        // The wiring case, and the reason every other case below means
        // anything: if ScheduleAnimalPopulationForFrame were never called, the
        // allocator could be flawless and the frame unchanged.
        RunFrames(4u);

        const AnimalSchedulerStats& stats = GetScene().GetAnimalSchedulerStats();
        EXPECT_EQ(stats.AnimalsConsidered, kHerdCount + 1u)
            << "the scene tick did not gather every AnimalBudgetComponent";
        EXPECT_EQ(stats.ConsideredByRole[static_cast<sizet>(AnimalRole::Hero)], 1u);
        EXPECT_EQ(stats.ConsideredByRole[static_cast<sizet>(AnimalRole::Background)], kHerdCount);
        EXPECT_EQ(GetScene().GetAnimalSchedules().size(), kHerdCount + 1u);
    }

    TEST_F(AnimalPopulationFixture, ThePathMovesTheHerdAndLeavesTheHeroAlone)
    {
        const glm::vec3 heroStart = m_Hero.GetComponent<TransformComponent>().Translation;
        const glm::vec3 herdStart = m_Herd.front().GetComponent<TransformComponent>().Translation;

        RunFrames(120u);

        const glm::vec3 heroEnd = m_Hero.GetComponent<TransformComponent>().Translation;
        const glm::vec3 herdEnd = m_Herd.front().GetComponent<TransformComponent>().Translation;

        EXPECT_GT(glm::length(herdEnd - herdStart), 0.5f) << "the herd did not move: the AnimalPaths system is not "
                                                             "registered, or the scheduler node never ran";
        // A HERO IS HAND-PLACED BY DEFINITION. Giving it a path would make its
        // apparent size a function of the clock and every hero-quality
        // assertion unrepeatable.
        EXPECT_LT(glm::length(heroEnd - heroStart), 1e-5f) << "the hero moved, but it carries no path component";
    }

    TEST_F(AnimalPopulationFixture, RepeatedRunsPlaceThePopulationIdentically)
    {
        RunFrames(90u);
        std::vector<glm::vec3> first;
        first.reserve(m_Herd.size());
        for (const Entity& animal : m_Herd)
        {
            first.push_back(animal.GetComponent<TransformComponent>().Translation);
        }

        // A second fixture, built from the same code and ticked the same way.
        // Compared bit for bit rather than within a tolerance: the path is a
        // closed form of accumulated time, so two runs that agree at all must
        // agree exactly, and a tolerance would hide a drift that had only just
        // started.
        SecondPopulation other;
        other.Begin();
        other.Advance(90u);
        ASSERT_EQ(other.Herd().size(), m_Herd.size());
        for (sizet i = 0; i < m_Herd.size(); ++i)
        {
            const glm::vec3 a = first[i];
            const glm::vec3 b = other.Herd()[i].GetComponent<TransformComponent>().Translation;
            EXPECT_TRUE(Math::BitwiseEqual(a.x, b.x) && Math::BitwiseEqual(a.y, b.y) && Math::BitwiseEqual(a.z, b.z))
                << "animal " << i << " landed somewhere else on an identical run";
        }
        other.End();
    }

    TEST_F(AnimalPopulationFixture, ThePathIsFrameRateIndependent)
    {
        // THE LOAD-BEARING REPRODUCIBILITY CASE. The same simulated time at two
        // different frame rates must put the population in the same place. An
        // accumulated path cannot pass this; a closed-form one cannot fail it,
        // which is exactly why the component stores an elapsed TIME and derives
        // the position rather than storing the position.
        //
        // 240 frames at 1/240 s and 60 frames at 1/60 s are both one second.
        RunFrames(60u, 1.0f / 60.0f);
        std::vector<glm::vec3> coarse;
        for (const Entity& animal : m_Herd)
        {
            coarse.push_back(animal.GetComponent<TransformComponent>().Translation);
        }

        SecondPopulation fine;
        fine.Begin();
        fine.Advance(240u, 1.0f / 240.0f);
        ASSERT_EQ(fine.Herd().size(), m_Herd.size());

        for (sizet i = 0; i < m_Herd.size(); ++i)
        {
            const glm::vec3 a = coarse[i];
            const glm::vec3 b = fine.Herd()[i].GetComponent<TransformComponent>().Translation;
            // A tolerance here and not a bitwise compare: the two runs
            // accumulate a different NUMBER of floating-point additions into
            // m_ElapsedSeconds, so they reach one second with a few ulps
            // between them. What the closed form guarantees is that the
            // POSITION is a continuous function of that time — not that two
            // different summations of 1/60 and 1/240 produce the same bits.
            EXPECT_NEAR(a.x, b.x, 1e-3f) << "animal " << i << " x";
            EXPECT_NEAR(a.z, b.z, 1e-3f) << "animal " << i << " z";
        }
        fine.End();
    }

    // -------------------------------------------------------------------------
    // Criterion 3 — the budget's behaviour, through the real tick
    // -------------------------------------------------------------------------

    TEST_F(AnimalPopulationFixture, TheHeroIsNeverCoarsenedThroughARealTick)
    {
        for (u32 frame = 0; frame < 120u; ++frame)
        {
            RunFrames(1u);
            const auto& schedules = GetScene().GetAnimalSchedules();
            const auto it = schedules.find(m_Hero.GetUUID());
            ASSERT_NE(it, schedules.end()) << "frame " << frame << ": the hero fell out of the population";
            for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
            {
                EXPECT_EQ(it->second.Step[a], 0u)
                    << "frame " << frame << " axis " << ToString(static_cast<AnimalWorkAxis>(a))
                    << ": the hero gave way through a real tick";
            }
        }
        EXPECT_EQ(GetScene().GetAnimalSchedulerStats().CoarsenedByRole[static_cast<sizet>(AnimalRole::Hero)], 0u);
    }

    TEST_F(AnimalPopulationFixture, TheLossRotatesAcrossTheHerdRatherThanPinningTheSameAnimals)
    {
        // THE FAIRNESS PROPERTY AS IT SURVIVES A REAL TICK, and stated
        // relatively for AnimalSchedulerContractTest's reason: under a budget
        // that cannot serve anybody, every animal is below desired every frame
        // and an absolute streak bound is a promise no allocator can keep.
        //
        // What the service order guarantees is that the loss MOVES. So this
        // counts how many distinct animals were coarsened at some point over
        // the run: a scheduler that always picked the same victims would show a
        // small set, and one that rotates shows most of the herd.
        std::unordered_map<u64, u32> coarsenedFrames;
        for (u32 frame = 0; frame < 200u; ++frame)
        {
            RunFrames(1u);
            for (const auto& [id, schedule] : GetScene().GetAnimalSchedules())
            {
                if (schedule.Role != AnimalRole::Background)
                {
                    continue;
                }
                if (schedule.Step[kDeform] > 0u)
                {
                    coarsenedFrames[static_cast<u64>(id)] += 1u;
                }
            }
        }

        ASSERT_FALSE(coarsenedFrames.empty()) << "nothing was ever coarsened, so the rotation asserts nothing";

        // How unevenly the loss was spread. A scheduler that pinned the same
        // victims leaves most of the herd at zero.
        u32 everCoarsened = 0u;
        for (const auto& [id, frames] : coarsenedFrames)
        {
            if (frames > 0u)
            {
                ++everCoarsened;
            }
        }

        EXPECT_GT(everCoarsened, kHerdCount / 2u)
            << "only " << everCoarsened << " of " << kHerdCount
            << " background animals were ever coarsened: the loss is being pinned on a fixed subset rather than "
               "rotated, which is the starvation the counter is meant to prevent";
    }

    // THE STRAND-FLOOR CASE IS DELIBERATELY NOT HERE, and saying so is the
    // point of this comment. It would be VACUOUS in this fixture: no groom
    // asset resolves without an asset manager, so every animal's strand count
    // gathers as zero, the visibility axis costs nothing, the floor never binds
    // and the assertion would pass no matter what MinVisibleStrands did.
    //
    // A test that asserts nothing is worse than no test, because it reads as
    // coverage. The floor is pinned twice where it can actually be pinned:
    // AnimalSchedulerContractTest sweeps it over a 120-frame run of an
    // over-subscribed herd, and AnimalBudgetVisualEvidenceTest measures it on
    // real pixels with real cooked grooms.

    // -------------------------------------------------------------------------
    // Criterion 4 — the update frequency half, measured rather than asserted
    // -------------------------------------------------------------------------

    TEST_F(AnimalPopulationFixture, TheDeformationBudgetActuallyReducesTheTickRate)
    {
        // COUNTS REAL AnimationSystem::Update CALLS, by watching the clip time
        // the gate advances. This is the case that would fail if the schedule
        // were computed perfectly and never spent — the whole "isolated data
        // structure" failure, caught by reading a number only the production
        // path writes.
        //
        // Forced rather than waited for: a budget small enough that the herd
        // MUST lose deformation rate, so the case cannot pass by the population
        // happening to fit.
        // Tighter still than the fixture's already-binding allowance, so the
        // herd must lose deformation rate rather than merely might.
        Renderer3D::GetRendererSettings().AnimalFrameBudgetUnits = 400.0f;

        constexpr u32 kFrames = 64u;
        std::unordered_map<u64, u32> ticksSeen;
        std::unordered_map<u64, f32> lastTime;
        for (const Entity& animal : m_Herd)
        {
            lastTime[static_cast<u64>(animal.GetUUID())] = -1.0f;
        }

        for (u32 frame = 0; frame < kFrames; ++frame)
        {
            RunFrames(1u);
            for (const Entity& animal : m_Herd)
            {
                const u64 id = static_cast<u64>(animal.GetUUID());
                const f32 now = animal.GetComponent<AnimationStateComponent>().m_CurrentTime;
                if (!Math::BitwiseEqual(now, lastTime[id]))
                {
                    ++ticksSeen[id];
                    lastTime[id] = now;
                }
            }
        }

        u32 maxTicks = 0u;
        u32 minTicks = kFrames;
        for (const auto& [id, count] : ticksSeen)
        {
            maxTicks = std::max(maxTicks, count);
            minTicks = std::min(minTicks, count);
        }

        EXPECT_LT(minTicks, kFrames) << "no animal lost any deformation rate, so the gate is not wired in";
        EXPECT_GT(maxTicks, 0u) << "every animal stopped ticking entirely, which is not a budget but a bug";
    }

    TEST_F(AnimalPopulationFixture, TheFrameTimeWindowIsFilledByTheRealSceneTick)
    {
        // CRITERION 4'S TAIL, WIRED. FrameTimeTailTest pins the statistic; this
        // pins that anything FEEDS it. A percentile class that no frame loop
        // pushes into is the "isolated data structure left as completion" the
        // delivery contract names, and it would report a clean window forever.
        EXPECT_EQ(GetScene().GetFrameTimeTail().SampleCount, 0u) << "the window should start empty";

        RunFrames(90u, 1.0f / 60.0f);

        const FrameTimeTailStats tail = GetScene().GetFrameTimeTail(16.6f);
        EXPECT_EQ(tail.SampleCount, 90u) << "the scene tick is not recording frames into the window";
        // The harness feeds a fixed dt, so every sample is the same 16.67 ms and
        // the whole distribution collapses onto it. That is the point: it means
        // the window is carrying the CALLER's delta rather than a wall-clock
        // read, which is what makes a capture under a mock clock reproducible.
        EXPECT_NEAR(tail.P50Ms, 1000.0f / 60.0f, 0.01f);
        EXPECT_NEAR(tail.P99Ms, 1000.0f / 60.0f, 0.01f);
        EXPECT_NEAR(tail.MaxMs, 1000.0f / 60.0f, 0.01f);
        // THE COUNTER IS EXACT, AND 16.667 IS OVER 16.6. Asserted both ways
        // round because the first draft of this case asserted zero against a
        // 16.6 ms budget "because a 60 Hz tick is not over 60 Hz" — which is
        // wrong by two thirds of a millisecond, every frame. A boundary
        // comparison that is off by a rounding error is exactly the kind of
        // thing an over-budget COUNT is there to make visible.
        EXPECT_EQ(tail.OverBudgetFrames, 90u) << "every 16.667 ms frame is over a 16.6 ms budget";
        EXPECT_EQ(GetScene().GetFrameTimeTail(20.0f).OverBudgetFrames, 0u) << "and none of them is over 20 ms";
    }

    TEST_F(AnimalPopulationFixture, TheExpensiveFramesAreStaggeredRatherThanAligned)
    {
        // THE TAIL, NOT THE MEAN, expressed as the property that produces it.
        // Forty animals at a quarter rate all ticking on the SAME frame cost
        // exactly as much on that frame as forty animals at full rate — the
        // mean falls and the stutter sits precisely where it was. The phase in
        // ShouldPoseAnimalThisFrame is what prevents that, and this case is the
        // only thing that would notice if it were removed.
        // Tighter still than the fixture's already-binding allowance, so the
        // herd must lose deformation rate rather than merely might.
        Renderer3D::GetRendererSettings().AnimalFrameBudgetUnits = 400.0f;

        constexpr u32 kFrames = 64u;
        std::vector<u32> tickedPerFrame;
        tickedPerFrame.reserve(kFrames);
        std::unordered_map<u64, f32> lastTime;
        for (const Entity& animal : m_Herd)
        {
            lastTime[static_cast<u64>(animal.GetUUID())] = -1.0f;
        }

        for (u32 frame = 0; frame < kFrames; ++frame)
        {
            RunFrames(1u);
            u32 ticked = 0u;
            for (const Entity& animal : m_Herd)
            {
                const u64 id = static_cast<u64>(animal.GetUUID());
                const f32 now = animal.GetComponent<AnimationStateComponent>().m_CurrentTime;
                if (!Math::BitwiseEqual(now, lastTime[id]))
                {
                    ++ticked;
                    lastTime[id] = now;
                }
            }
            tickedPerFrame.push_back(ticked);
        }

        // Ignore the first few frames: the hold and the ladders are still
        // settling, and a transient is not what this case is about.
        const u32 peak = *std::max_element(tickedPerFrame.begin() + 8, tickedPerFrame.end());
        EXPECT_LT(peak, static_cast<u32>(m_Herd.size()))
            << "every animal posed on the same frame: the deformation stagger is not spreading the population, so "
               "the frame-time tail is unchanged however good the mean looks";
    }
} // namespace OloEngine::Functional
