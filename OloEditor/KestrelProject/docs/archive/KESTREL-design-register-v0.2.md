# KESTREL — game design register

Status: working draft v0.2, 2026-11-25 (construction workshop owner responses recorded). This records the discussion with the project owner; it is not an implementation mandate. Proposed mechanics must be explicitly accepted before becoming requirements. Balancing examples are not final numbers.

## 1. Established direction

The existing [north star](https://github.com/drsnuggles8/OloEngineBase/blob/master/docs/process/north-star.md) is the authority for the current internal vertical slice (June 30, 2027; one Alpine mountain, 1950 start, full seasonal cycle, technology to roughly 1965, 1080p60 on GTX 1660/RX 5600 class hardware). The following owner-stated design direction concerns the intended full game; it does not silently change that milestone.

| ID | Owner-stated direction |
| --- | --- |
| D-001 | KESTREL is a resort construction and management game, primarily focused on building resorts. The game is not designed around the trailer. |
| D-002 | Multiple maps are essential to the full game. Each resort in the empire can be played as a sandbox; local resort play and long-term portfolio growth belong together. |
| D-003 | Include curated scenarios in the spirit of RollerCoaster Tycoon, including rescue and expansion of existing resorts. |
| D-004 | Buying and merging neighboring resorts into a larger connected ski domain is important and should be represented in a scenario. |
| D-005 | Geography and weather must have gameplay consequences, not just visual effects. |
| D-006 | Content and rules should be data-driven. |
| D-007 | Difficulty must not rely on arbitrary cost/demand bonuses or AI opponents cheating. Competition should be fair. |
| D-008 | Establish proper human playtesting and feedback loops early. |
| D-009 | Major winter sporting events are a desired scenario direction (owner proposed Olympics/X Games-style events). Exact event types, rules and licensed branding remain undecided. |

Existing north-star direction additionally includes full seasons, historical technologies, an eventual scenario campaign and an international resort group. Exact timing, acquisition rules, economy, competitor AI, art direction, and launch content count remain open.

## 2. Working design principles (proposed synthesis)

- Construction, operation, visible consequences and long-term expansion form the central loop.
- Favor context-dependent choices and interconnected mechanics over a long list of isolated features.
- Simulate enough detail to create understandable decisions and immersion; automate repetitive operations.
- Preserve fair competition and transparent causal feedback. Scenario-specific starting constraints can differ, but the rules of play should not secretly favor AI.
- Sandbox, scenarios and empire use the same simulation and content definitions. Ownership and physical network connectivity are separate concepts.
- Trailer and demo are honest representations of gameplay, not reasons to implement otherwise unnecessary systems.
- Technical correctness, design effectiveness and human enjoyment are separate acceptance dimensions.

## 3. Design workshop queue

1. Construction and creative freedom
2. Time and historical progression
3. Guests and mountain networks
4. Economy and business
5. Snow, weather and geography
6. Empire, acquisitions and competition
7. Scenarios and sporting events
8. Progression and replayability
9. UX and art direction
10. Playtesting and product validation

## 4. Workshop 1 — construction (owner decisions and remaining proposals)

Source context: [#1740](https://github.com/drsnuggles8/OloEngineBase/issues/1740) is closed; [strategy-build-tools.md](https://github.com/drsnuggles8/OloEngineBase/blob/master/docs/guides/strategy-build-tools.md) documents an existing build history, preview/validation and strategy camera. [#1741](https://github.com/drsnuggles8/OloEngineBase/issues/1741) and [#1742](https://github.com/drsnuggles8/OloEngineBase/issues/1742) specify lift and trail construction. [#1743](https://github.com/drsnuggles8/OloEngineBase/issues/1743) and [#1744](https://github.com/drsnuggles8/OloEngineBase/issues/1744) own simulated and visible lift operation. Do not duplicate these owners.

| ID | Status | Decision / remaining question |
| --- | --- | --- |
| C-01 | Accepted | Hybrid construction: free-form infrastructure and accessible functional buildings, with optional creative customization. |
| C-02 | Accepted direction; representation provisional | Editable splines are the preferred authoring approach unless a demonstrably better interaction or representation exists. Pistes may split, join and offer independently rated alternatives (e.g. a black shortcut branching off a longer red route). Width and localized shaping remain adjustable design proposals. |
| C-03 | Accepted | Lift construction must be realistic but intuitive, with automatic engineering assistance and optional fine-tuning. Equipment and alignment choices must have genuine capacity and operational consequences. No mandatory detailed ropeway engineering. |
| C-04 | Accepted | Terrain reshaping is very limited in ordinary play, primarily constrained by meaningful costs. Preserve geography as a challenge; do not make bulldozing a cheap universal solution. Allow useful localized grading, foundations and other engineering works, including reservoirs for snowmaking. Exact hard limits and creative-mode exceptions remain open. |
| C-05 | Accepted | Functional hotels, restaurants and other buildings with optional variants, decorations and modular customization; focus on resort building rather than compulsory architectural micromanagement. |
| C-06 | Accepted | Land/construction rights and available technology together determine expansion opportunities. A map can contain multiple resorts; ownership is distinct from physical connectivity. Exact land-rights transaction rules and research gates remain open. |
| C-07 | Open | Recommended: reversible planning/paused edits; operational assets use demolition, replacement and salvage, not full retroactive refunds. Owner has not explicitly decided. |
| C-08 | Open, owner explicitly undecided | Construction timing remains open. Compare instant, short visible phases and longer project schedules after the time/progression discussion. |
| C-09 | Accepted direction | Piste difficulty must account for more than steepness: geometry, visibility, snow and weather conditions, informed by real-world practices. Support regional rating conventions including black/double-black diamonds where appropriate. Proposed implementation separates published baseline rating from changing suitability, risk and closure; exact classification model remains open. |
| C-10 | Accepted direction | Provide a readable lift and piste map. Proposed additions: automatic generation from live network data, planning overlays, conditions and flow views, and eventual export. Exact presentation and scope remain open. |
| C-11 | Accepted direction | Include meaningful snowmaking infrastructure such as water reservoirs. The player should be able to invest in cheaper or more reliable snow production through infrastructure placement. Reservoirs have capital cost and finite water storage; gravity/pressure, refill availability, pumping energy and suitable wet-bulb conditions are proposed interacting factors. No free or unlimited snow. |

### Construction workshop refinements (design hypotheses)

- Keep an editable geometric corridor separate from the directed routing graph. A named piste may consist of multiple edges; shared segments must not duplicate snow, capacity or grooming work. Splits and merges are explicit nodes. Intersections may be at grade or grade-separated. A black shortcut can close without closing the red main route. Validate routes under guest skill, lift/piste closures and return-path constraints. Do not force one spline to equal one entire named piste.
- Represent the permanent rating separately from current operational state: a red piste remains red on the map when icy, but its current risk, speed, suitability and opening status may change. Include longitudinal and cross slope, sustained steep sections, width, obstacles, curvature and normal grooming; weather/snow/visibility modify current conditions. Ratings and labels are region-specific, not one universal gradient table. References: [Burke Mountain trail signs](https://www.skiburke.com/the-mountain/winter/safety-policy), [National Ski Patrol terrain selection](https://www.patrollerschool.org/events/divi-home/outdoor-emergency-transportation/oet-senior-program-terrain-selection/), [Swiss slope conditions explainer](https://www.intersportrent.ch/en/rentertainer-blog/difficulty-chart-of-ski-slopes~14364578).
- For earthworks, investigate pricing by cut/fill volume, access, terrain and environmental/land constraints rather than a flat brush cost. Local grading can be worthwhile while moving a mountain is prohibitive. Reservoir location, usable capacity and connection to snow guns should create real choices. Reference: [Snow production — Snøkompetansesenteret](https://xn--snkompetanse-wjb.no/snoproduksjon/?lang=en). Detailed hydrology is not a prerequisite for a useful management mechanic.
- Land rights grant legal/contractual access; technology grants engineering feasibility. Avoid arbitrary gates that prevent feasible construction without explanation.
- Make piste maps derive from canonical network state, not manually authored pictures; communicate named runs, difficulty, directions, closed connections and lifts. Stylized panoramic rendering/export is a later hypothesis.

### Affected backlog (scope review, not new issues)

- #1741: assisted valid lift placement, fine-tuning and meaningful previews; crossings should be evaluated by clearance and equipment constraints, not blanket forbidden if feasible.
- #1742: explicit branch/junction topology, editable corridors and local grading; rating/conditions split and regional conventions.
- #1737/#1738/#1739: snowpack, snowmaking resources and weather-dependent suitability/closures; reservoirs and water distribution need a scope decision before implementation.
- #1745: skill-aware route choice at splits, downstream reachability and alternatives.
- #1754: automatically derived piste/lift network map and management overlays.
- #1740/#1751: clarify undo after simulation/economic effects before integrating unrestricted construction refunds.
- #1752: technology as a genuine feasibility constraint; no arbitrary gates without a rationale.

### Proposed player interaction

Select tool → sketch candidate → get live preview → adjust → confirm an atomic project → observe visible construction/opening → inspect effects on guest flow and finances → modify or expand.

A complete network has usable entry points, uphill links, downhill routes and facilities. Preview must distinguish physical invalidity from operational warnings. Allow incomplete construction plans; prohibit opening genuinely inaccessible or unsafe routes according to game rules. Warn about missing return paths under typical closures. Represent intersections, station roles, ownership and geometry separately from guest routing. Keep all balance data configurable.

### Prototype questions

- Can a newcomer place a lift and draw a usable piste without help?
- Do players understand the preview and why a candidate is invalid?
- Can two different valid network layouts solve the same congestion problem?
- Can players improve a design without rebuilding the entire system?
- Does the mountain still feel like a real place after construction?

Proposed playtest task: provide a small base area, a limited budget, a ridge, two terrain routes and an existing crowded lift. Ask testers to improve the resort without prescribing the solution. Compare construction completion, invalid placement recovery, route comprehension, and voluntary next projects. Record observations and build version; do not claim fun from automated tests alone.

## 5. Playtesting framework (proposed, detail to refine)

- Write a design hypothesis before implementing substantial mechanics. Record expected player decisions and observable outcomes.
- Use deterministic simulation experiments to detect impossible scenarios, dominant strategies, instability and economic exploits; use human sessions to establish comprehension, satisfaction, pacing and fairness.
- Conduct initial observed sessions with a small varied group, then repeated blind/return sessions and larger closed tests. Use behavior, short questionnaires and interviews together.
- Collect telemetry only with informed opt-in; allow inspection and withdrawal, avoid unnecessary personal information, and keep raw feedback/save snapshots out of the public repository by default. Use private storage/access controls and retention limits.
- Track findings by build, scenario, hypothesis, evidence, severity, confidence and owner; retest changes instead of assuming a fix improved enjoyment. Publish anonymized summaries only.

## 6. Proposed repository integration

Recommended canonical location: `docs/design/kestrel/` (consistent with the existing root docs index), with `README.md` as the entry point and one document per approved topic. Link it from `docs/README.md` and the shared game/agent guidance. A local project docs directory is also possible if the owner prefers it; avoid two sources of truth. Keep approved decisions separate from hypotheses and tunables. Use stable IDs and cross-links rather than copying content into `AGENTS.md` and `CLAUDE.md`.

After each workshop, record accepted decisions, rejected alternatives and why, unresolved questions, playtest hypotheses and affected issue owners. Do not modify the north-star milestone or create implementation tasks from unapproved ideas. Where current issues conflict with an accepted design, update their scope before work starts. The owner decides scope and publishing; agents can implement, measure and suggest revisions.

Workshop format preference: put each decision question immediately after its relevant section, rather than collecting all questions at the end. Free-form responses are valid; do not require questionnaires.

No repository write has been performed for this draft.
