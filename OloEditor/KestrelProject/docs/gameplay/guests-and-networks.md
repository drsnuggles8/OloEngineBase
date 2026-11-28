# Guests, transport and piste networks

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

A skier is an autonomous inspectable individual with skill, preferences, needs, itinerary and group relationships. Day visitors and holiday groups have different requirements. A good network serves different abilities and allows safe returns before closing, not just maximum nominal piste length.

Represent physical skiable corridors, directed navigation edges/junctions and named pistes independently. Distinguish advertised difficulty, assessed geometry and current snow/weather difficulty. Shared sections must not duplicate capacity or traffic. Closed routes are excluded from ordinary navigation; permitted off-piste areas have meaningful risk. Guests who ignore clear warnings may incur recovery fees; operator-caused and unexpected failures are the operator’s responsibility. Safe recovery is independent of payment.

Rendering LOD never changes authoritative behavior. Guest movement may use compact state and event-driven updates rather than per-frame decision logic. Exact pathfinding and risk algorithms remain engineering experiments.

Population scale is an architectural requirement from the start ([AUD-21](#aud-21)): the vertical slice simulates at least 20,000 concurrent, individually represented skiers, and the full game's engineering ambition is about 100,000 to 200,000 concurrent guests in a large region. The slice includes a basic functional ski school ([AUD-25](#aud-25)).

## Decision records

<a id="g-01"></a>

### G-01

**Disposition:** Accepted.

Distinguish day visitors from multi-day holiday visitors, including accommodation, visit duration, spending and weather sensitivity.

<a id="g-02"></a>

### G-02

**Disposition:** Accepted.

Guests have individual, continuously varying characteristics and needs (inspectable bars, preferences and activity styles); demographic segments summarize distributions rather than rigid classes.

<a id="g-03"></a>

### G-03

**Disposition:** Accepted.

Resort choice emerges from guest preferences, prices, available alternatives and experience rather than a single attractiveness score. Satisfied visitors may return. Include a no-visit alternative.

<a id="g-04"></a>

### G-04

**Disposition:** Accepted direction.

Families, friends and other organized groups are meaningful. Include school trips (owner example: two teachers and twenty pupils on a week-long trip) and ski-school groups. Shared plans and group coordination should not require manual player control. Exact group sizes and coordination rules are tunable.

<a id="g-05"></a>

### G-05

**Disposition:** Accepted.

Accessible terrain, connected networks and safe return routes are central measures of quality. Support multiple transport/activity modes and interconnected resorts; raw piste length alone is insufficient.

<a id="g-06"></a>

### G-06

**Disposition:** Accepted with refinements.

Recreational route selection balances enjoyment, skill, novelty, congestion and preferences; destination routing prioritizes reaching the destination. Guests follow friends/family/instructors and can exhibit group-following or swarming. Consider time until planned breaks and resort/lift closing when selecting routes. Individual movements must look natural.

<a id="g-07"></a>

### G-07

**Disposition:** Accepted; configurable disruption.

Visible lift queues and network congestion. Beginners may slow boarding/unloading or cause interruptions; surface lifts can have mid-route falls and stops where appropriate. Skill mix and lift type affect actual throughput. Make this disruption mechanic toggleable to avoid frustrating players; exact rate and coupling to safety remain experimental.

<a id="g-08"></a>

### G-08

**Disposition:** Accepted.

Use simplified imperfect information, signs/maps, current published conditions and familiarity; abstraction is preferred when it improves fun.

<a id="g-09"></a>

### G-09

**Disposition:** Accepted.

Ordinary falls, meaningful injury risk and patrol response, with player agency through construction, conditions, safety management and truthful ratings. Keep serious incidents proportionate and avoid graphic/punitive presentation.

<a id="g-10"></a>

### G-10

**Disposition:** Accepted.

Guests autonomously manage their needs. Players provide well-located facilities and services; no individual micromanagement. Aggregate causes and interventions in management UI.

<a id="g-11"></a>

### G-11

**Disposition:** Accepted.

Multidimensional satisfaction and category-specific reputation, with an accessible overall summary. Explain the actual causes.

<a id="g-12"></a>

### G-12

**Disposition:** Accepted.

Every active individual guest is selectable/inspectable with detailed information, thoughts, activities and preferences. Returning visitors and favorites are desired; long-term persistence scope is to be measured, not necessarily a permanent record of every visitor.

<a id="g-13"></a>

### G-13

**Disposition:** Accepted.

Ski schools and experience improve guest skill, allowing visitors to choose more advanced terrain as ability increases. Keep progression calibrated through playtests.

<a id="g-14"></a>

### G-14

**Disposition:** Accepted; AUD-04 controls responsibility.

Use closing policies, warnings and return-route checks. Staff recover stranded guests regardless of payment. Fees may apply if clear warnings were ignored; operator bears recovery costs for operator-caused or unexpected failures. Exact amounts and attribution remain open.

<a id="g-15"></a>

### G-15

**Disposition:** Accepted.

Extensive data-driven management overlays and forecasts for proposed changes, with estimates rather than guaranteed future outcomes.

<a id="g-16"></a>

### G-16

**Disposition:** Accepted.

Convincing individual skiing, including natural movement and animation; no need for physically exact skiing dynamics. Kinematic/abstract simulation is acceptable when visuals are believable.

<a id="g-17"></a>

### G-17

**Disposition:** Accepted, strong invariant.

LOD is for rendering/presentation, not gameplay effects. Camera, zoom and rendering tier must not change authoritative queues, movement decisions, economic outcomes, satisfaction or accidents. Time-speed invariance remains required. Any explicitly chosen inactive-region aggregation must be distinct from camera/rendering LOD and validated against the same rules.

<a id="g-18-g-20"></a>

### G-18–G-20

**Disposition:** Accepted direction.

Prototype mechanics early, test them through small network challenges, and use human playtesting/feedback to decide what is fun before scaling.

<a id="g-21"></a>

### G-21

**Disposition:** Historical process decision; fulfilled for design phase.

Design workshops and audit precede implementation issue edits. Consolidation is now requested; repository integration and backlog reconciliation follow separately. No issue edits authorized by the original decision.

<a id="aud-04"></a>

### AUD-04

**Disposition:** Accepted.

Separate physical corridor, directed navigation segments/junctions and named piste identity/classification. Distinguish responsibility for stranded guests: fees may apply when clear warnings were ignored; operator bears costs for operator-caused or unexpected failures. Recovery is available irrespective of payment.

<a id="aud-21"></a>

### AUD-21

**Disposition:** Accepted (owner decision, 2026-11-28; changes the slice's population target).

The vertical-slice minimum is at least **20,000 concurrent, individually represented simulated skiers** on a representative busy region, with identity, needs, route choices, queues, transport and financial effects authoritative; idle records do not count. It replaces the earlier 5,000 target. Hardware (GTX 1660 / RX 5600 class, 6 GB VRAM, 16 GB RAM, six-core CPU) and the p95 frame time of 16.6 ms or less at 1080p on the busiest slice view are unchanged, and the population is never silently reduced if a measurement fails. The full game's engineering ambition is about **100,000 to 200,000 concurrent guests per large region**, a scalability investigation target rather than a measured capability or a requirement at the slice's frame budget; multi-region capacity is a separate measure. No arbitrary player-facing population cap. Simulation scale, rendered crowd count, daily distinct visitors and empire-wide totals are reported separately. Scalability is designed in from the first guest implementation and exercised at representative load (a 1k to 20k ladder plus 50k to 200k stress runs that report achieved rates), not deferred to optimisation. Simulation fidelity never depends on the camera ([G-17](#g-17)); render LOD and culling are allowed. Internal representation is chosen after profiling. Provenance: [owner decisions of 2026-11-28 on #1727](https://github.com/drsnuggles8/OloEngineBase/issues/1727#issuecomment-7241796309), Q1 and Q9.

<a id="aud-25"></a>

### AUD-25

**Disposition:** Accepted (owner decision, 2026-11-28; slice scope for G-13).

A basic functional ski school is in the vertical slice, after the first external playtest checkpoint: bounded skill improvement, capacity, cost and income, and consequences for guests' later choices, with no instant mastery. Detailed lessons, individually managed instructors and multi-day or return-guest progression remain later work. A decorative building does not satisfy this. Refines [G-13](#g-13). Provenance: [owner decisions of 2026-11-28 on #1727](https://github.com/drsnuggles8/OloEngineBase/issues/1727#issuecomment-7241796309), Q14.

## Validation and acceptance (proposed test focus)

Bottleneck, inaccessible summit, branching routes, group reunions and last-lift tests; camera/speed invariance; no guest or queue duplication.

## Related design decisions

[C-02](construction.md#c-02), [C-09](construction.md#c-09), [AUD-03](time-and-history.md#aud-03), [AUD-05](snow-and-weather.md#aud-05), [W-10](snow-and-weather.md#w-10), [U-15](../experience/art-direction.md#u-15), [AUD-19](../validation/automated-balancing.md#aud-19)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
