#pragma once

#include "OloEngine/Core/Base.h"

// box2d/types.h: typedef struct b2WorldDef { ... } b2WorldDef;
struct b2WorldDef;

namespace OloEngine::Physics2D
{
    // The Box2D worker count for a scheduler with this many workers: the workers plus
    // the thread calling b2World_Step, capped. 1 means a single-threaded world.
    [[nodiscard]] i32 WorkerCountForScheduler(u32 schedulerWorkers);

    // Runs Box2D's parallel stages (solver, broad phase, island split, continuous
    // collision) on the engine task scheduler instead of serially inside b2World_Step.
    // Box2D 3.x results do not depend on the worker count, so this changes cost, not
    // simulation. With no scheduler workers the world stays single-threaded.
    void UseEngineTaskSystem(b2WorldDef& def);
} // namespace OloEngine::Physics2D
