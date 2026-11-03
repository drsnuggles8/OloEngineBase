// OLO_TEST_LAYER: unit
//
// Physics2D::UseEngineTaskSystem runs Box2D 3.2's parallel stages on the engine
// task scheduler. What this pins:
//   - the worker-count mapping (scheduler workers + the stepping thread, capped);
//   - a world stepped through the scheduler is bit-identical to a single-threaded
//     one, the Box2D 3.x property that makes the change cost-only;
//   - stepping still finishes when the scheduler has no workers, so every Box2D
//     task runs inline inside Tasks::Launch, worker 0 first. Box2D's orchestrator
//     race is what makes that safe; a regression there hangs instead of failing.

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Scene/Physics2DTasks.h"
#include "OloEngine/Task/Scheduler.h"

#include <box2d/box2d.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <thread>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    struct Pyramid
    {
        b2WorldId World{};
        std::vector<b2BodyId> Bodies;
    };

    // Enough stacked boxes that the constraint solver splits its colours into
    // several blocks per stage, which is when Box2D hands work to other workers.
    Pyramid MakePyramid(const b2WorldDef& def, i32 rows)
    {
        Pyramid pyramid;
        pyramid.World = b2CreateWorld(&def);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        b2BodyDef groundDef = b2DefaultBodyDef();
        b2BodyId const ground = b2CreateBody(pyramid.World, &groundDef);
        b2Polygon const groundBox = b2MakeOffsetBox(100.0f, 1.0f, { 0.0f, -1.0f }, b2Rot_identity);
        b2CreatePolygonShape(ground, &shapeDef, &groundBox);

        b2Polygon const box = b2MakeBox(0.5f, 0.5f);
        for (i32 row = 0; row < rows; ++row)
        {
            for (i32 column = 0; column < rows - row; ++column)
            {
                b2BodyDef bodyDef = b2DefaultBodyDef();
                bodyDef.type = b2_dynamicBody;
                bodyDef.position = { static_cast<f32>(column) * 1.05f + static_cast<f32>(row) * 0.525f,
                                     0.5f + static_cast<f32>(row) };
                b2BodyId const body = b2CreateBody(pyramid.World, &bodyDef);
                b2CreatePolygonShape(body, &shapeDef, &box);
                pyramid.Bodies.push_back(body);
            }
        }
        return pyramid;
    }

    std::vector<f32> StepAndCapture(Pyramid& pyramid, i32 steps)
    {
        for (i32 step = 0; step < steps; ++step)
        {
            b2World_Step(pyramid.World, 1.0f / 60.0f, 4);
        }
        std::vector<f32> state;
        state.reserve(pyramid.Bodies.size() * 4);
        for (b2BodyId const body : pyramid.Bodies)
        {
            b2Vec2 const position = b2Body_GetPosition(body);
            b2Rot const rotation = b2Body_GetRotation(body);
            state.insert(state.end(), { position.x, position.y, rotation.c, rotation.s });
        }
        return state;
    }

    // Wraps the engine's enqueue callback so a test can prove it was used.
    std::atomic<u32> s_EnqueuedOnScheduler{ 0 };
    b2EnqueueTaskCallbackFcn* s_EngineEnqueue = nullptr;

    void* CountingEnqueue(b2TaskCallbackFcn* task, void* taskContext, void* userContext)
    {
        s_EnqueuedOnScheduler.fetch_add(1, std::memory_order_relaxed);
        return s_EngineEnqueue(task, taskContext, userContext);
    }
} // namespace

// The test binary does not start the scheduler; an Application does.
class Physics2DTasks : public ::testing::Test
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
};

TEST_F(Physics2DTasks, WorkerCountIsSchedulerWorkersPlusTheSteppingThreadCapped)
{
    EXPECT_EQ(Physics2D::WorkerCountForScheduler(0), 1);
    EXPECT_EQ(Physics2D::WorkerCountForScheduler(3), 4);
    EXPECT_EQ(Physics2D::WorkerCountForScheduler(7), 8);
    EXPECT_EQ(Physics2D::WorkerCountForScheduler(31), 8);
}

TEST_F(Physics2DTasks, SchedulerSteppedWorldMatchesASingleThreadedOneBitForBit)
{
    b2WorldDef parallelDef = b2DefaultWorldDef();
    Physics2D::UseEngineTaskSystem(parallelDef);
    ASSERT_GT(parallelDef.workerCount, 1) << "the suite fixture starts scheduler workers";
    ASSERT_NE(parallelDef.enqueueTask, nullptr);
    s_EngineEnqueue = parallelDef.enqueueTask;
    parallelDef.enqueueTask = &CountingEnqueue;
    s_EnqueuedOnScheduler.store(0, std::memory_order_relaxed);

    b2WorldDef const serialDef = b2DefaultWorldDef();
    ASSERT_EQ(serialDef.enqueueTask, nullptr);

    constexpr i32 kRows = 40; // 820 boxes
    constexpr i32 kSteps = 120;
    Pyramid parallel = MakePyramid(parallelDef, kRows);
    Pyramid serial = MakePyramid(serialDef, kRows);
    std::vector<f32> const parallelState = StepAndCapture(parallel, kSteps);
    std::vector<f32> const serialState = StepAndCapture(serial, kSteps);
    b2DestroyWorld(parallel.World);
    b2DestroyWorld(serial.World);

    EXPECT_GT(s_EnqueuedOnScheduler.load(std::memory_order_relaxed), 0u) << "Box2D never used the engine scheduler";
    ASSERT_EQ(parallelState.size(), serialState.size());
    EXPECT_EQ(std::memcmp(parallelState.data(), serialState.data(), parallelState.size() * sizeof(f32)), 0)
        << "Box2D results must not depend on the worker count";
}

TEST_F(Physics2DTasks, SteppingCompletesWhenTheSchedulerHasNoWorkers)
{
    b2WorldDef def = b2DefaultWorldDef();
    Physics2D::UseEngineTaskSystem(def);
    ASSERT_GT(def.workerCount, 1);

    // The world keeps its parallel worker count, but every task it enqueues now
    // runs inline in Tasks::Launch: worker 0 orchestrates the whole solve, and each
    // later spinner sees the finished solve and returns.
    auto& scheduler = LowLevelTasks::FScheduler::Get();
    scheduler.StopWorkers();
    struct RestoreWorkers
    {
        ~RestoreWorkers()
        {
            LowLevelTasks::FScheduler::Get().StartWorkers();
        }
    } restoreWorkers;

    // A detached thread lets a hang fail the test after the liveness budget instead
    // of blocking the process forever in a joining destructor.
    auto pyramid = std::make_shared<Pyramid>(MakePyramid(def, 20));
    auto finished = std::make_shared<std::promise<void>>();
    auto done = finished->get_future();
    std::thread([pyramid, finished]
                {
                    StepAndCapture(*pyramid, 30);
                    finished->set_value(); })
        .detach();
    ASSERT_EQ(done.wait_for(std::chrono::seconds(30)), std::future_status::ready)
        << "a Box2D task run inline must not wait for a worker that never starts";
    b2DestroyWorld(pyramid->World);
}
