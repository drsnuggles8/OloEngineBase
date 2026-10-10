# The animation clock's history survives an unrendered tick

**Never reset a previous-frame time on a tick that does not render.** A throttled editor ticks the
scene between rendered frames. A reset there runs on every rendered frame, so every frame reports
zero motion. Reset a temporal history only where it really breaks: a scene load, entering or leaving
Play, or a 2D/3D mode switch.

## What happened (#1354)

`Scene::m_LastAnimationTime` is the previous frame's animation time. The wind, water and foliage
shaders read it as `PrevAnimationTime` for their per-fragment velocity. It starts at `-1`, which
means "seed from this frame". Each "rendering disabled" branch of `OnUpdateEditor`,
`OnUpdateSimulation` and `RenderRuntime` wrote `-1` back, so that a renderer resumed after a pause
would re-seed.

The editor's render throttle (`EditorLayer`'s `skipRender`) ticks the scene with rendering disabled
between rendered frames. A background editor on the IntegratedRenderer benchmark showed the result:
the previous time was `-1` on every rendered frame, so `prevAnimationTime == animationTime`
throughout. Wind, water and foliage velocity were zero, and TAA reprojected swaying grass as
static. The foliage ray-tracing plan read the same frozen step, so it charged no refresh work while
the cache refreshed every group every frame.

Four rounds of trace logging missed the writer. The first trace's 20-line cap was used up by the
startup scene, and the other writers it named never fired. An lldb hardware watchpoint on the
member's address (`watchpoint set expression -w write -s 4 -- <addr>`, then `bt`) named it on the
first hit.

## The rule, applied

- The animation clock advances only on rendered frames, by at most 0.1 s a frame
  (`ProcessScene3DSharedLogic`). A resumed frame's step is therefore bounded, and the
  rendering-disabled resets were never needed. They are gone. The 2D-mode resets stay: a mode switch
  is rare and does break continuity.
- `AnimationClockThrottleTest.AnUnrenderedTickKeepsThePreviousAnimationTime` renders, runs
  unrendered ticks, and checks the previous time survived them.
- A consumer that needs a step should measure it itself where it can. The vegetation planner times
  the step since its own last plan (`FoliageRenderer::m_PlanAnimationTime`), so an upstream re-seed
  cannot zero it.
