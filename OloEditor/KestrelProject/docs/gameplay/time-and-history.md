# Time, calendar and historical operation

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

All ordinary play uses one continuous calendar with pause, five speeds and configurable intelligent advancement. Player-selectable calendar compression and day/year pacing are rejected. Internal experiments may vary parameters; one hour per year is a hypothesis, not a guarantee. Process intervening consequences, including nights and off-season, rather than silently removing them. Decouple animation and authoritative simulation.

Preserve RNG state by default, support optional future reseeding and diagnostic save lineage. No Ironman mode or ordinary reload penalties. Replay from a versioned initial state and timestamped commands; same seed/actions must give equivalent authoritative results across camera, speed, presets, save/load and headless execution. Keyed independent streams are a candidate, not a mandated algorithm.

## Decision records

<a id="t-01"></a>

### T-01

**Disposition:** Accepted.

Hybrid continuous calendar and optional advancement over routine periods; discrete simulation ticks conceptually similar to grand-strategy games, at a cadence suitable for KESTREL rather than necessarily hourly. Provide pause and five selectable speed levels (1–5); actual multipliers and tick intervals are tunable.

<a id="t-02"></a>

### T-02

**Disposition:** Accepted as playtest target.

Approximately one hour of effective play per simulated year is the initial pacing hypothesis, not a fixed clock multiplier or mandatory year length. Era locking remains a separate unconfirmed option.

<a id="t-03"></a>

### T-03

**Disposition:** Partially accepted.

Day/night and seasonal effects should matter, including after-hours grooming. Owner is unsure whether non-skiing periods (late spring, summer, autumn) should be skipped or compressed and does not want repetitive nightly popups. Simplified summer initially; genuine summer activities eventually, including hiking, mountain biking, golf and alpine/mountain coasters or summer toboggan runs. Winter sledding (Schlittenfahren) is also a candidate activity. Specific activities and release scope are not committed.

<a id="t-04"></a>

### T-04

**Disposition:** Accepted.

Pause, five speeds and event-based advancement with configurable stop conditions. Speed changes alone must not change simulation outcomes. Routine operations should not require acknowledgment or repetitive intervention.

<a id="t-05"></a>

### T-05

**Disposition:** Preferred direction; validate.

Owner leans toward instant construction; fun takes precedence over realistic construction schedules. Immediate construction is the recommended default after comparison with competing games. Persistent plans and cosmetic construction effects are compatible. Optional scheduled projects in advanced settings or selected scenarios remain a hypothesis, not a requirement. Winter replacement feasibility is not itself a reason to mandate waiting.

<a id="t-06"></a>

### T-06

**Disposition:** Accepted direction.

Mix historical availability with player-controlled research/organizational expertise and improvements. Prioritize enjoyable progression rather than literal simulation of invention by resort operators. Exact research costs, gates and branches remain open.

<a id="t-07"></a>

### T-07

**Disposition:** Accepted.

Historical eras change more than available equipment. Older equipment can have lower throughput, lower comfort and higher failure rates, with condition and maintenance contributing rather than arbitrary age penalties alone. Broader era-specific tourism, expectations, architecture and regulatory changes are desired; exact data and scope remain open.

<a id="t-08"></a>

### T-08

**Disposition:** Accepted for prototyping; AUD-08 controls.

Shared empire calendar or independent regional calendars, explained before selection. Connected or directly competing resorts within a region must share its clock. Test detailed delegated, simplified global and frozen independent-region presets. Cross-era capital, technology, acquisitions, obligations and mode transitions remain open.

<a id="t-09"></a>

### T-09

**Disposition:** Accepted.

Scenario-specific deadlines, sustained targets and open-ended objectives; continue playing after success.

<a id="t-10"></a>

### T-10

**Disposition:** Accepted; constrained by AUD-02.

Compare pacing parameters and time presentation in internal prototypes. Ship one coherent continuous calendar, not player-selectable calendar compression. Track waiting, interruptions and speed changes. Intelligent fast-forward and stop conditions are configurable.

<a id="t-11"></a>

### T-11

**Disposition:** Accepted; clarified by G-17 and AUD-19.

Separate rendering, authoritative simulation ticks, operational time, calendar and regional scheduling. Never let FPS, viewing distance or speed silently change gameplay. Explicit simplified inactive-region mode is a separately chosen ruleset for testing, not rendering LOD. Headless and interactive rules agree.

<a id="aud-02"></a>

### AUD-02

**Disposition:** Accepted baseline.

One coherent continuous calendar with pause and five speed levels; configurable intelligent fast-forward targets and stop conditions. Owner explicitly rejects player-configurable calendar compression or day/year pacing: central gameplay requires a consistently balanced model. Internal experiments may compare pacing parameters; approximately one hour per year is a playtest hypothesis, not a shipping guarantee. All intervening economic, weather, snow, guest and research consequences must be processed; speed must not silently change outcomes. Off-season presentation and acceleration remain open.

<a id="aud-03"></a>

### AUD-03

**Disposition:** Accepted.

Preserve random state by default; optional reseeding of future uncertainty, deterministic action replay and save/load lineage for diagnostics. Owner explicitly rejects Ironman-style mode. Do not punish ordinary reloads. Independent keyed RNG domains and stable event identifiers are proposed implementation approaches. Restore authoritative state including randomness, validate camera/preset/speed/headless/save equivalence for identical actions, and record version/seed provenance. Exact reseed scope and lineage persistence remain implementation questions; no intrusive anti-cheat.

## Validation and acceptance (proposed test focus)

Compare normal-day durations, fast-forward throughput, season pacing and visual motion; verify seed/action equivalence and absence of skipped expenses or RNG changes.

## Related design decisions

[AUD-08](empire-and-acquisitions.md#aud-08), [AUD-19](../validation/automated-balancing.md#aud-19), [G-17](guests-and-networks.md#g-17), [P-11](progression-and-research.md#p-11), [P-17](progression-and-research.md#p-17), [V-13](../validation/automated-balancing.md#v-13)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
