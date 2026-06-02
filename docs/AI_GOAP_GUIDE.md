# GOAP — Goal-Oriented Action Planning

OloEngine's AI module ships three decision-making styles that compose freely
on the same entity:

| Style | Header root | Nature | Use it for |
|-------|-------------|--------|------------|
| Behaviour Tree | `AI/BehaviorTree/` | reactive | scripted, hand-authored behaviour |
| Finite State Machine | `AI/FSM/` | reactive | a small fixed set of modes |
| **GOAP** | `AI/GOAP/` | **deliberative** | emergent behaviour from goals + a pool of actions |

GOAP (Jeff Orkin, *"Three States and a Plan: The A.I. of F.E.A.R."*, GDC 2006)
flips authoring around: instead of scripting *what* an agent does, you declare
*what it can do* (actions, each with preconditions and effects) and *what it
wants* (goals), and a planner searches for a sequence of actions that reaches a
goal from the current world state. Add a new action and every goal can
automatically exploit it — no graph to rewire.

All of `AI/GOAP/` is engine-agnostic and has no GPU/scene dependency, so it is
exercised entirely by CPU unit tests
([`OloEngine/tests/AI/GoapTest.cpp`](../OloEngine/tests/AI/GoapTest.cpp)) plus
one Functional test that drives it through a real `Scene::OnUpdateRuntime`
([`GoapAgentPlansViaSceneTickTest.cpp`](../OloEngine/tests/Functional/AI/GoapAgentPlansViaSceneTickTest.cpp)).

## World state

[`GoapWorldState`](../OloEngine/src/OloEngine/AI/GOAP/GoapWorldState.h) is a set
of symbolic facts — `key → bool | i32`. Facts are **discrete on purpose**: A*
needs a finite, hashable, equality-comparable state, so continuous quantities
must be discretised by your sensors first (expose ammo as the i32 `"AmmoCount"`
or the derived bool `"HasAmmo"`; floats are not a fact type). The store is a
key-sorted vector — one allocation per copy, O(n) equality/hash — because the
planner copies a state per expanded edge.

```cpp
GoapWorldState s;
s.Set("hasAxe", true);
s.Set("logCount", 3);
bool ok = s.Satisfies(conditions); // every fact in `conditions` matches here
```

## Actions and goals

A [`GoapAction`](../OloEngine/src/OloEngine/AI/GOAP/GoapAction.h) is split into a
*planning* half (pure data: `Name`, `Cost`, `Preconditions`, `Effects`) and an
optional *execution* half (closures the agent drives: `IsUsable`, `OnEnter`,
`Perform`). The planner only ever reads the data half. An action with no
closures is an instantaneous symbolic operator that succeeds the moment it is
reached.

A [`GoapGoal`](../OloEngine/src/OloEngine/AI/GOAP/GoapGoal.h) is a `DesiredState`
(partial world state), a `Priority`, and an optional `IsValid` relevance gate.

```cpp
GoapAction chop;
chop.Name = "ChopWood";
chop.Cost = 3.0f;
chop.Preconditions.Set("hasAxe", true);
chop.Effects.Set("hasFirewood", true);
chop.Perform = [entity](f32 dt) { /* play anim, return Running/Success/Failure */ };
```

## Planner

[`GoapPlanner::Plan`](../OloEngine/src/OloEngine/AI/GOAP/GoapPlanner.h) runs
**forward A\*** over world states: from the start state it applies the effects
of any action whose preconditions (and `IsUsable` gate) hold, until a state
satisfies the goal, then walks the parent chain into an ordered `GoapPlan`.

- **Heuristic**: count of unsatisfied goal conditions, scaled by
  `GoapPlannerSettings::HeuristicWeight`. The default `1.0` is the textbook GOAP
  heuristic (fast; optimal when no single action satisfies more than one
  outstanding goal condition). Set it to `0.0` for pure Dijkstra when you need a
  provably minimum-cost plan; `> 1.0` is greedier/faster but may cost more.
- **Termination** is guaranteed by `MaxIterations` and `MaxPlanLength` caps, a
  best-cost closed set (which also prunes zero-cost no-op cycles), and a
  negative-cost floor. Action costs should be ≥ 0.

## Agent

[`GoapAgent`](../OloEngine/src/OloEngine/AI/GOAP/GoapAgent.h) is the runtime
brain: a pool of actions, a prioritised goal list, and the agent's world state.
Each `Update(dt)` it:

1. runs an optional `Sensor` to fold fresh observations into the world state,
2. (re)plans toward the highest-priority relevant, unsatisfied goal if it has no
   live plan or was invalidated,
3. drives the current action (`OnEnter` once, then `Perform` per tick),
   advancing on `Success`, and
4. replans when a step reports `Failure` or the world drifts so a step's
   preconditions no longer hold.

It is deliberately decoupled from `Entity`/`Scene`/blackboard — wire those in
through the action closures and the sensor.

## ECS integration

Attach a
[`GoapAgentComponent`](../OloEngine/src/OloEngine/AI/AIComponents.h) and
`AISystem::OnUpdate` (called from `Scene::OnUpdateRuntime`) ticks its
`RuntimeAgent` every frame while `Enabled`. Like `BehaviorTreeComponent`, the
runtime brain (actions/goals/world state) is built programmatically by gameplay
or scripting code and is **not** serialized; only the authored `Enabled` flag
and a script-facing `Blackboard` persist across scene save/load and save-games.

From Lua, scripts push observations and read the agent's commitment:

```lua
local goap = entity:GetComponent("GoapAgentComponent")
goap:SetWorldFactBool("seesEnemy", true)
goap:Invalidate()                 -- force a replan next tick
local goal = goap:CurrentGoal()   -- e.g. "EliminateThreat"
```

## References

- Jeff Orkin, *Three States and a Plan: The A.I. of F.E.A.R.*, GDC 2006.
- Jeff Orkin, *Applying Goal-Oriented Action Planning to Games*, AI Game
  Programming Wisdom 2 (2003).
