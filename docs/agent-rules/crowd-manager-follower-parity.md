# A valid navmesh silently switches every NavAgent onto the crowd follower

Short rule for anyone touching `NavigationSystem`, `CrowdManager`, or a test
that calls `Scene::SetNavMesh` (issue #616).

## The trap

`CrowdManager` (DetourCrowd) used to be constructed and ticked every frame but
never actually registered with — `AddAgent`/`SetAgentTarget` had zero
production callers, so every `NavAgentComponent` silently fell back to the
naive manual path-follower in `NavigationSystem.cpp`, regardless of whether a
`CrowdManager` existed. Tests written against that reality (e.g.
`NavAgentUnreachableTargetTerminatesTest`) called `Scene::SetNavMesh(navMesh)`
and assumed they were exercising "the manual follower" specifically, because
that was the *only* follower that ever ran.

Once agents actually register with the crowd (`NavigationSystem::OnUpdate`
lazily calls `CrowdManager::AddAgent` on first tick whenever
`crowdMgr->IsValid()`), that assumption breaks silently: **any scene with a
valid navmesh — even one with a single `NavAgentComponent` and no avoidance
concerns at all — now drives that agent through the crowd, not the manual
path.** `Scene::SetNavMesh` brings up a `CrowdManager` whenever the mesh is
valid; there is no "manual mode" toggle. A test whose comments/name say
"manual follower" but which calls `SetNavMesh` with a valid mesh is actually
testing the crowd path, and its assertions need to hold for *both* followers,
not just the one it was written against.

## The rule

**Whenever you add or edit a Functional test that calls `Scene::SetNavMesh`
with a valid mesh and a `NavAgentComponent`, treat the crowd as the active
follower by default** — don't assume "manual" from the absence of a second
agent. If you need to test the manual path specifically, either drive
`NavMeshQuery::FindPath` directly (no `NavAgentComponent`/`Scene::SetNavMesh`
at all — see `NavMeshQueryFindPathBetweenPointsTest`), or make the test's
comments describe "the active follower" generically. The upside: this parity
is a deliberate design goal, not an accident to route around — `CrowdManager::
GetAgentTargetState` maps `dtCrowdAgent::targetState`/`partial` onto the same
`None`/`Pending`/`Valid`/`Unreachable` states the manual path's
`FindPathResult` already produces, and `NavigationSystem::OnUpdate` writes the
same `NavAgentComponent::m_HasTarget`/`m_TargetUnreachable` fields regardless
of which follower is driving, so consumers (`BTMoveTo`, scripts) don't need to
know or care which one is active.

## Lifecycle gotcha this also surfaces

`m_Registry.destroy()` does not fire `OnComponentRemoved<T>` (the existing
pattern documented in CLAUDE.md for Rigidbody3D/Terrain/Vehicle bodies) — so a
crowd agent slot needs the same explicit two-place teardown: a hand-written
`Scene::OnComponentRemoved<NavAgentComponent>` specialization (for
`RemoveComponent<T>()` at runtime, which does dispatch through
`OnComponentRemoved`) **and** an explicit `CrowdManager::RemoveAgent` call in
`Scene::DestroyEntity` (for entity destruction, which doesn't). Registration
itself stays lazy/hookless (`NavigationSystem::OnUpdate`, on first tick with
`m_CrowdAgentId < 0`) — only *removal* needed the hand-written
`OnComponentAdded`/`OnComponentRemoved` machinery and its
`kComponentsCustomOnRemove` exclusion entry in `tools/OloHeaderTool/main.cpp`
(registration has no such entry, deliberately — `kComponentsCustomOnAdd` does
not include `NavAgentComponent`).

## Guard

`OloEngine/tests/Functional/Navigation/CrowdAvoidancePreventsOverlapTest.cpp`
covers registration (distinct non-negative crowd ids), the actual avoidance
behaviour (two agents converging head-on stay separated), and both teardown
paths (`RemoveComponent<NavAgentComponent>()` and `Scene::DestroyEntity`, each
asserting `CrowdManager::GetActiveAgentCount()` drops by one).
`NavAgentUnreachableTargetTerminatesTest` is the regression guard for the
parity claim itself — it exercises the disconnected-navmesh-island scenario
through whichever follower is active (the crowd, per the trap above) and
still expects the same terminal `m_TargetUnreachable`/`m_HasTarget` contract.
