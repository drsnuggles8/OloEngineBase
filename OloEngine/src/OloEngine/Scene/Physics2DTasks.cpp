#include "OloEnginePCH.h"
#include "OloEngine/Scene/Physics2DTasks.h"

#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/Task.h"

#include <box2d/box2d.h>

#include <algorithm>

namespace OloEngine::Physics2D
{
    namespace
    {
        // Box2D's solver keeps every non-orchestrating worker spinning for the whole
        // constraint solve, so each extra worker holds a scheduler thread while the
        // world steps. The cap keeps a 2D world from claiming the whole pool.
        constexpr i32 kMaxBox2DWorkers = 8;

        struct FBox2DTask
        {
            Tasks::TTask<void> Task;
        };

        void* EnqueueBox2DTask(b2TaskCallbackFcn* task, void* taskContext, void* /*userContext*/)
        {
            // Safe to run inline (no scheduler workers) or late: the thread inside
            // b2World_Step races for the solver's orchestrator slot itself, and workers
            // that start after the solve see its end sentinel and return at once.
            return new FBox2DTask{ Tasks::Launch(
                "Box2D", [task, taskContext]
                { task(taskContext); },
                Tasks::ETaskPriority::High) };
        }

        void FinishBox2DTask(void* userTask, void* /*userContext*/)
        {
            auto* box2DTask = static_cast<FBox2DTask*>(userTask);
            box2DTask->Task.Wait();
            delete box2DTask;
        }
    } // namespace

    i32 WorkerCountForScheduler(u32 schedulerWorkers)
    {
        return static_cast<i32>(std::min<u32>(schedulerWorkers, kMaxBox2DWorkers - 1)) + 1;
    }

    void UseEngineTaskSystem(b2WorldDef& def)
    {
        def.workerCount = WorkerCountForScheduler(LowLevelTasks::FScheduler::Get().GetNumWorkers());
        const bool parallel = def.workerCount > 1;
        def.enqueueTask = parallel ? &EnqueueBox2DTask : nullptr;
        def.finishTask = parallel ? &FinishBox2DTask : nullptr;
        def.userTaskContext = nullptr;
    }
} // namespace OloEngine::Physics2D
