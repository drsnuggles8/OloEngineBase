# KESTREL — game design register

Status: working draft v0.3, 2026-11-26 (construction follow-up decisions recorded; time workshop proposals added). This records the discussion with the project owner; it is not an implementation mandate. Proposed mechanics must be explicitly accepted before becoming requirements. Balancing examples are not final numbers.

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
| C-02 | Accepted direction; representation provisional | Editable splines are the preferred authoring approach unless a demonstrably better interaction or representation exists. Pistes may split, join and offer independently rated alternatives (e.g. a black shortcut branching off a longer red route). The owner accepts the corridor/branching approach, including locally adjustable boundaries. Splines remain the preferred authoring representation rather than a mandatory internal format. |
| C-03 | Accepted | Lift construction must be realistic but intuitive, with automatic engineering assistance and optional fine-tuning. Equipment and alignment choices must have genuine capacity and operational consequences. No mandatory detailed ropeway engineering. |
| C-04 | Accepted | Terrain reshaping is very limited in ordinary play, primarily constrained by meaningful costs. Preserve geography as a challenge; do not make bulldozing a cheap universal solution. Allow useful localized grading, foundations and other engineering works, including reservoirs for snowmaking. The owner approves volume-based cut/fill pricing and wants substrate (soil versus hard rock) to affect cost. Exact coefficients, physical limits and creative-mode exceptions remain open. |
| C-05 | Accepted | Functional hotels, restaurants and other buildings with optional variants, decorations and modular customization; focus on resort building rather than compulsory architectural micromanagement. |
| C-06 | Accepted | Land/construction rights and available technology together determine expansion opportunities. A map can contain multiple resorts; ownership is distinct from physical connectivity. The owner proposes an abstract government-influence mechanism for obtaining land rights. The exact mechanism, eligibility and research gates remain open; transparent permit/relationship rules are a design hypothesis, not an approved implementation. |
| C-07 | Accepted direction | Undo is important. Make the percentage of construction cost recoverable configurable in advanced settings, with difficulty presets offering different defaults. Exact refund percentages, time/operation limits, and distinction between undo and demolition are unresolved. Uncommitted plan editing should remain free (proposed). No retroactive reversal of earned revenue or other simulation effects (proposed anti-exploit invariant). |
| C-08 | Open, owner explicitly undecided | Construction timing remains open. Compare instant, short visible phases and longer project schedules after the time/progression discussion. |
| C-09 | Accepted direction | Piste difficulty must account for more than steepness: geometry, visibility, snow and weather conditions, informed by real-world practices. Support regional rating conventions including black/double-black diamonds where appropriate. Separate assessed baseline difficulty, advertised rating and current conditions. The owner supports consequences for misrating: advertising a run as easier than it really is can increase accidents; overstating challenge can disappoint experienced guests. Exact override range, safety rules and behavioral model remain open. |
| C-10 | Accepted direction | Provide a readable lift and piste map. The owner wants an actual resort map, multiple selectable visual styles, and many data-driven planning/management views. Derive them from the live network. Export and exact styles remain proposals. |
| C-11 | Accepted direction | Include meaningful snowmaking infrastructure such as water reservoirs. The player should be able to invest in cheaper or more reliable snow production through infrastructure placement. The owner prefers a somewhat abstract utility model and is unsure whether precise electricity prices/energy calculations add enough gameplay; test before committing. Reservoirs, water availability, snowmaking coverage and plausible weather constraints are the desired direction; physical pipe routing, detailed pressure/energy and electricity price exposure remain hypotheses. No free or unlimited snow. |

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

### Construction follow-up (2026-11-26): decisions and testable refinements

- **Ratings and trust:** Preserve an internal terrain assessment and a separate advertised regional rating. A deliberate or mistaken understatement can cause guests to choose terrain beyond their ability, increasing incidents, dissatisfaction and reputational consequences; exaggeration can disappoint expert guests and unnecessarily deter others. These are directional consequences, not a calibrated injury model. Temporary ice, fog, powder and traffic alter current suitability without silently changing the printed piste map. The allowed override range, warnings and hard closure rules still need design. Never give players a profitable way to ignore safety through misleading ratings.
- **Ground materials:** Extend the approved earthworks cost concept with spatial material-dependent excavation rates, e.g. integrate c_cut(material, access) over removed volume and c_fill over placed volume; add transport/restoration costs. Soil should generally be cheaper to excavate than competent hard rock, with map-specific geology data or an explicitly approximate authored layer. DEM elevation alone does not provide geology. Show the reason for cost differences. Material reuse is a candidate optimization, not a commitment to geotechnical simulation.
- **Utilities:** Start with gameplay-relevant storage, refill, snowmaking coverage and operating costs; compare an abstract service model against physical water connections. Electricity may be a bundled cost or broad capacity constraint rather than a minute-by-minute tariff. Add detailed pumping, power or pressure mechanics only if a playable comparison establishes meaningful decisions. Reservoirs are useful infrastructure, not a guarantee of cheap snow regardless of climate or water supply.
- **Land rights and government influence:** Prototype a visible local-authority relationship/permit system. Eligibility could use project impacts, local economic benefits, prior commitments and transparent scenario rules. Owner proposes influence, not a final reputation formula or bribery mechanic. Purchasing a sector, securing permission, acquiring a competitor and technological feasibility are separate concerns. Avoid opaque arbitrary cooldowns and forced grinding; expose requirements and reasons for refusals.
- **Map styles:** Selectable visual presentation (e.g. classic illustrated panoramic, clean schematic/topographic, modern 3D) is a desired feature; exact styles are provisional. All views use identical network data and status. Analytical overlays include gradients, snow, queues, guest flow, reachability, ownership, cost/feasibility and planned projects; prioritize by player usefulness. A beautiful export is a candidate later feature.
- **Undo settings:** Difficulty presets can set refund defaults, but expose the percentage independently in advanced settings, including a forgiving creative preset. Separate free plan edits, reversible placement and removal of operating assets. Refunds are a disclosed economic rule, not a hidden AI advantage. Test exact accounting, intervening transactions, terrain restoration, replacement, redo and save/load. No final percentage or eligibility window is set. Construction speed remains undecided pending Workshop 2.

### Additional issue-scope implications (not filed)

- #1742/#1745/#1746: separate rated difficulty, actual conditions, guest beliefs and outcomes; branch-specific rating and capacity; warnings and feedback on misrating.
- #1740/#1751: configurable refund/salvage policy in the game layer, consistent undo accounting and no retroactive duplication of money; do not change generic BuildHistory semantics unnecessarily.
- #1742/#1732: map-driven substrate and cut/fill pricing; do not claim geological data comes from elevation alone.
- #1738: evaluate abstract snowmaking utilities first; reservoir placement, resource limits and operating cost previews.
- #1754: live network map with selectable styles and analytical overlays; prioritize legibility over decoration.
- #1752 and later empire/land-rights tasks: distinguish permissions, technical feasibility, capital and ownership; government influence remains a proposal.

## 5. Workshop 2 — time and historical progression (proposed, not accepted)

- Separate physical render frames, deterministic simulation ticks, displayed operational time, calendar/era progression and presentation. Game-speed controls should not change underlying economic or safety rules. Pause allows planning and inspection. A global empire date is the preferred hypothesis; local resorts should not independently advance into the future.
- Compare continuous compressed daily calendars, representative operational days with aggregated intervals, and season-based turns. Preferred experiment: continuous playable days with optional safe fast-forward over routine days/nights/off-season, preserving a common calendar and accounting; avoid requiring every day of every year to be played at normal speed. No final compression ratio.
- All four seasons matter. Winter is the main operational loop; spring and autumn preparation/maintenance/construction should provide meaningful choices; summer can initially be a short planning/construction interval, with eventual summer activities kept separate from the existing slice commitment.
- Historical era dates determine eligibility for technologies; research/experience/capital may determine adoption. Technology is not a universally superior ladder. Sandbox/scenarios can start in multiple eras and optionally lock or disable historical gates. Exact unlock system is open.
- Construction timing: prototype instant placement vs short visible construction vs scheduled major projects. Preserve free planning, show opening dates, avoid idle waiting; construction durations and seasonal constraints must match time compression. Compare using one lift expansion task. No choice yet.
- Empire: prototype detailed active resort and aggregated inactive resort simulation advancing on the same date, with equivalent accounting, capacity, weather and rules. The owner can switch focus, delegate policies and receive actionable alerts. Avoid forcing the player to babysit every resort; decide whether multiplayer/competition or urgent events affect pauses separately if needed.
- Playtest metrics: actual wall time until first skier, first meaningful intervention, first expansion, first seasonal transition; waiting/fast-forward usage and reasons, task switching burden, whether players can explain time changes, and desire to advance to the next season. Compare candidate clocks on the same playable prototype; no invented final pacing targets.
- Existing milestone says one full year and technology roughly 1950–1965; decide how to demonstrate the era range (accelerated progression, separate starting saves, or unlock fixture) without silently expanding the committed slice. Do not change north-star.md without owner approval.

## 6. Playtesting framework (proposed, detail to refine)

- Write a design hypothesis before implementing substantial mechanics. Record expected player decisions and observable outcomes.
- Use deterministic simulation experiments to detect impossible scenarios, dominant strategies, instability and economic exploits; use human sessions to establish comprehension, satisfaction, pacing and fairness.
- Conduct initial observed sessions with a small varied group, then repeated blind/return sessions and larger closed tests. Use behavior, short questionnaires and interviews together.
- Collect telemetry only with informed opt-in; allow inspection and withdrawal, avoid unnecessary personal information, and keep raw feedback/save snapshots out of the public repository by default. Use private storage/access controls and retention limits.
- Track findings by build, scenario, hypothesis, evidence, severity, confidence and owner; retest changes instead of assuming a fix improved enjoyment. Publish anonymized summaries only.

## 7. Proposed repository integration

Recommended canonical location: `docs/design/kestrel/` (consistent with the existing root docs index), with `README.md` as the entry point and one document per approved topic. Link it from `docs/README.md` and the shared game/agent guidance. A local project docs directory is also possible if the owner prefers it; avoid two sources of truth. Keep approved decisions separate from hypotheses and tunables. Use stable IDs and cross-links rather than copying content into `AGENTS.md` and `CLAUDE.md`.

After each workshop, record accepted decisions, rejected alternatives and why, unresolved questions, playtest hypotheses and affected issue owners. Do not modify the north-star milestone or create implementation tasks from unapproved ideas. Where current issues conflict with an accepted design, update their scope before work starts. The owner decides scope and publishing; agents can implement, measure and suggest revisions.

Workshop format preference: put each decision question immediately after its relevant section, rather than collecting all questions at the end. Free-form responses are valid; do not require questionnaires.

No repository write has been performed for this draft. v0.3 supersedes v0.2; the previous version is retained for comparison.
