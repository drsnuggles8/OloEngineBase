# Model + effort rubric for a HANDOVER

Apply to every task, every batch, in the HANDOVER and in the `/start-work` report.

Source: Anthropic's own guidance, re-read 2026-09-29 — [Choosing the right model](https://platform.claude.com/docs/en/about-claude/models/choosing-a-model),
[Effort](https://platform.claude.com/docs/en/build-with-claude/effort), and the per-model *Prompting
Claude Opus 5.5 / Sonnet 5.5 / Fable 5.1* guides. When a model ships, re-read those pages and update
this file; do not extend it from memory. Only the models below are recommended: never Opus 5,
Sonnet 5, Fable 5 or Haiku (Haiku 5.5 is a subagent model, not a session model).

Anthropic's framing, which this rubric follows: *most workloads start with Opus 5.5*, and *tuning
effort is often a better lever than switching models*. So pick the model by the **ceiling** the
task needs, then the effort by how hard and how long it is. Match both to the task's *reasoning
difficulty and verification burden*, NOT its leverage tier: a high-leverage task can be mechanical,
and a small diff can be correctness-critical.

| Model | Anthropic's positioning | Default effort | Use it here for |
|---|---|---|---|
| **Opus 5.5** | "Complex agentic coding and enterprise work"; "most workloads start with" it | **medium** | **The default.** Renderer and engine work whose failure is silent, cross-subsystem invariants, concurrency, anything gated on a visual or runtime verification loop. |
| **Sonnet 5.5** | "Speed and capability for everyday coding, agent, and enterprise workloads" | high | The cheaper lane, for a task that is **well specified**, has an existing pattern to copy **and** test guards that catch mistakes (codegen slice mirroring a prior slice, settings plumbing with a reference implementation, a tool mirroring its siblings, a warning/smell batch, a rename sweep, a docs pass). If the why-line needs "but watch out for…", it is an Opus 5.5 task. |
| **Fable 5.1** | "The highest available capability"; "agent sessions that run for hours" | high | (a) A **long-running session driving a whole Tier 1/4 epic** kept whole (see below). (b) **Escalation**: a task where Opus 5.5 at `xhigh`/`max` has already fallen short on reasoning. Anthropic positions it as the top of the ladder, not a fast lane: do not pick it for mechanical work. |

**Effort** (Anthropic's level descriptions; the per-model advice overrides the generic table):

| Level | Anthropic's use case | Use it here for |
|---|---|---|
| `low` | simpler tasks needing the best speed and lowest cost, "such as subagents" | Subagent work; a one-line fix. Rarely a session level for this repo. |
| `medium` | "agentic tasks that require a balance of speed, cost, and performance" | **Default for Opus 5.5**, standard feature and most renderer work. **Start point for Sonnet 5.5** on a well-specified task. |
| `high` | "complex reasoning, difficult coding problems, agentic tasks" | Tricky correctness, subtle bugs, cross-subsystem invariants, a race-test-driven task. **Sonnet 5.5** on a harder or longer task. **Fable 5.1**'s starting level. |
| `xhigh` | "long-running agentic and coding tasks (over 30 minutes) with token budgets in the millions" | A long session where `high` was already seen to fall short. |
| `max` | "deepest possible reasoning"; on most workloads adds cost for small gains | Only when a `xhigh` attempt visibly ran out of headroom. |

Rules that come from the same pages:

- **Levels are not comparable across models.** Opus 5.5's default moved *down* from Opus 5's `high`
  to `medium`, and Sonnet 5.5's levels are recalibrated against Sonnet 5, so "Opus 5.5 medium" is
  the reference point, not a downgrade. Do not carry a setting over from an older model.
- **Raise effort before switching model**, and escalate to Fable 5.1 only after `xhigh`/`max` on
  Opus 5.5 has fallen short.
- **`xhigh`/`max` need evidence, not a hunch**: say in the why-line which earlier attempt at a lower
  level went wrong on *reasoning* rather than on missing information.
- **Effort is set per session** in Claude Code (`/effort`). Per-message effort is an API beta and is
  not exposed in the CLI, so a HANDOVER names one level for the whole task.
- Higher effort means more tool calls and more explanation; lower effort means terser, fewer calls.
  A worker that seems to under-investigate at `medium` needs `high`, not a longer prompt.
- Adaptive thinking is always on for Opus 5.5 and Fable 5.1, so there is no "thinking off" choice
  to make in the HANDOVER.

## The whole-epic Fable 5.1 session

The default for a large Tier 1/4 epic kept whole (SKILL.md §3, "Right-size the unit of work"). It
fans sub-pieces out to subagents via the Agent tool instead of a human pre-slicing the scope.
Structure the HANDOVER's Plan section around it:

1. **Research first**, read-only: Explore agents (or the project's `rules-lookup` agent for the
   applicable `docs/agent-rules/` guides) map the existing patterns and touch-points the epic needs.
2. **The orchestrating session fixes the cross-cutting core contracts itself** (component shapes,
   scheduler registration, the seams between the epic's sub-parts) *before* delegating anything,
   because a wrong subagent guess is expensive to unwind there.
3. **Only then delegate genuinely file-disjoint sub-pieces.** A shared worktree means parallel
   subagents editing the *same* files collide: sequence those, or use `isolation: 'worktree'`.
   Run implementation subagents **one at a time**: parallel fan-outs have exhausted the 5-hour
   usage limit mid-task and contend for the build lock and the GPU.
4. **Cross-binding touch-point edits and cross-part wiring stay in the main session**, sequentially,
   since that is where a missed edge fails silently.
5. **Verify every subagent's actual diff** before trusting its summary.

Subagents doing read-only or mechanical legwork can run a cheaper model (Anthropic: "an
orchestrator that delegates bulk work to lower-cost workers"): set the Agent tool's `model` per
delegation, e.g. Sonnet 5.5 for a mechanical sub-piece. The project agents in `.claude/agents/`
already pin theirs. Any mandatory verification loop (visual, runtime) still applies regardless of
the driving model: a Fable-orchestrated session does not get to skip screenshot evidence on a
rendering change.
