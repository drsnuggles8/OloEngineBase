# PBF / SPH solver stability with physical-unit masses (issue #630)

The Position-Based Fluids paper (Macklin & Müller 2013) writes its formulas
for **unit-mass particles**, and most reference constants (s_corr `k = 0.1`,
"small" vorticity ε) implicitly assume that scale. OloEngine's solver
(`OloEngine/src/OloEngine/Fluid/`) uses physical masses `m = ρ₀·d³` (so a
0.1 m-spacing water particle weighs 1 kg, a 0.2 m one 8 kg). Porting the paper
verbatim at this scale produced a **detonating** dam (avg density 7× rest,
velocities pinned at the clamp) and later a **catapulting** coupling. Every
one of these was caught by a CPU contract test, not by eyeballing — keep that
order of operations. The four landmines, in the order they bit:

## 1. s_corr diverges at close range — clamp the per-iteration correction

`s_corr = −k·(W(r)/W(Δq·h))ⁿ` grows without bound as two particles approach
(`W(r) → W(0)` while `|∇W|` explodes). One close pair gets a metre-scale
correction in a single Jacobi iteration and the block detonates. Fixes, both
required:

- **Clamp |Δp| per particle per iteration** to `kFluidMaxDeltaPFraction·h`
  (0.2·h). This is the actual stability guard — it bounds *every* correction
  pathology, not just s_corr.
- Rescale `k` for the unit system (engine default 0.01, not the paper's 0.1),
  and prefer `Δq = 0.2·h` so the rest-spacing ratio `W(d)/W(Δq·h)` stays ≪ 1.

## 2. XSPH / vorticity need the SPH volume weight `m/ρ₀`

The paper's `v_i += c·Σ (v_j − v_i)·W` and `ω = Σ (v_j − v_i) × ∇W` fold the
volume factor into unit mass. With physical kernels the sums must be weighted
by `vol = m/ρ₀ (= d³)` — the proper discretizations of smoothing and curl.
Without the weight, XSPH's neighbour sum has total weight ≫ 1 (it *amplifies*
velocity differences instead of damping — ~100× here) and |ω| comes out ~10³
too large, so vorticity confinement injects tens of m/s per step. Symptom:
mean speed rides the MaxSpeed clamp forever.

## 3. Jacobi over-correction sustains collective "breathing" — under-relax

Each particle sits in ~30 neighbours' density constraints; satisfying them
simultaneously (Jacobi) over-corrects by roughly the overlap factor, and the
resulting in-phase column oscillation **cannot be damped by XSPH** (XSPH is
neighbour-relative; the breathing mode has no relative motion). Tuning the CFM
ε does not fix this — smaller ε makes it *worse* (stronger overshoot), larger
ε leaves hydrostatic compression unresolved. The fix is standard
successive-under-relaxation: scale the summed correction by
`kFluidJacobiRelaxation = 0.4` and budget one extra iteration (defaults: 4
iterations, ε = 50). A settled dam then reads mean speeds < 0.25 m/s; peak
bottom-corner compression converges to ~14% (that is the realistic bound for
one substep at these defaults — don't chase the paper's single-digit numbers
without paying substeps/iterations).

## 4. Contact-based rigid coupling: average over iterations, test moving intruders

The reaction impulse `J = −correction·(m/dt)` is accumulated in the displace
pass **every iteration**, and the λ solve pushes particles back into a body
between iterations — so the per-step sum over-counts by ≈ the iteration count.
A floating box got launched to y = 30 m. Fix: divide the harvested feedback by
`SolverIterations` (CPU: after the loop; GPU: in `HarvestFeedback`, so no
shader change).

Also know what contact coupling *can't* do: a **static** body submerged in a
settled pool feels mostly the particles resting on top of it — hydrostatic
pressure does not transmit without penetration — so its net contact impulse
can point *down*. Write coupling contracts around **moving intruders**
(opposing impulse, robust sign) and **closed-loop floating** (integrate a
dynamic proxy against the feedback; assert it neither reaches the floor nor
exceeds the pool — the two failure directions are launch and sink). Proper
buoyancy of static bodies needs boundary-density (Akinci-style) handling — a
known follow-up, not a bug.

## 5. Jolt body snapshots: never query mass/velocity on a non-dynamic body

`JoltBody::GetMass()` reaches `JPH::Body::GetMotionProperties()`, which
**asserts `!IsStatic()` in Debug** — and wherever `Physics3DSystem::Init`
never ran (the test harness, and evidently the editor play path), Jolt's
`AssertFailed` handler is the default silent trap: the process dies with
exception 0x80000003 / CRT exit 3 and **zero log output** (the engine logger,
the GL debug callback, and the wired Jolt handler all log-first — a truly
silent breakpoint means it came from an *unwired* vendor assert). Guard every
body snapshot with `body->IsDynamic()` before `GetMass()` /
`GetLinearVelocity()` / `GetAngularVelocity()` (BuildProxy in
`Fluid/FluidSystem.cpp` is the reference). The failure was nondeterministic at
the scene level because it depended on whether the static ground landed in the
domain's `OverlapBox` hit set (touching AABBs are a narrow-phase edge case) —
diagnosed by breadcrumb-bisecting a play-mode stress test
(`FluidVisualEvidenceTest.PlayModeStressWithBodiesAndEmitterSurvives`, which
now pins the combination: GPU solver + Jolt bodies + emitter + full renderer).

## Working method that made this tractable

CPU reference solver first, bit-deterministic (serial, index-ordered), with
behavioural contracts (settle / incompressibility / determinism / coupling
signs) — then the GPU chain is *parity-tested against the CPU* (aggregate
tolerances: COM within 1.5 spacings; kill/emit counts exact; impulse signs).
Every stability landmine above was diagnosed from a failing contract's
numbers, and the GPU inherited each fix through one shared constant.
