# KESTREL — game design register

Status: working draft v0.12, 2026-11-26 (all ten initial workshops recorded; consolidation and consistency audit pending). This records the discussion with the project owner; it is not an implementation mandate. Proposed mechanics must be explicitly accepted before becoming requirements. Balancing examples are not final numbers.

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
| C-08 | Preferred direction; validate | Owner leans toward immediate construction and prioritizes fun over realism. Competitor comparison supports instant default construction with optional planning/animation. Delayed projects are an optional hypothesis, not a requirement. See T-05. |
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
- **Undo settings:** Difficulty presets can set refund defaults, but expose the percentage independently in advanced settings, including a forgiving creative preset. Separate free plan edits, reversible placement and removal of operating assets. Refunds are a disclosed economic rule, not a hidden AI advantage. Test exact accounting, intervening transactions, terrain restoration, replacement, redo and save/load. No final percentage or eligibility window is set. Construction timing preference is recorded in Workshop 2 (T-05).

### Additional issue-scope implications (not filed)

- #1742/#1745/#1746: separate rated difficulty, actual conditions, guest beliefs and outcomes; branch-specific rating and capacity; warnings and feedback on misrating.
- #1740/#1751: configurable refund/salvage policy in the game layer, consistent undo accounting and no retroactive duplication of money; do not change generic BuildHistory semantics unnecessarily.
- #1742/#1732: map-driven substrate and cut/fill pricing; do not claim geological data comes from elevation alone.
- #1738: evaluate abstract snowmaking utilities first; reservoir placement, resource limits and operating cost previews.
- #1754: live network map with selectable styles and analytical overlays; prioritize legibility over decoration.
- #1752 and later empire/land-rights tasks: distinguish permissions, technical feasibility, capital and ownership; government influence remains a proposal.

## 5. Workshop 2 — time and historical progression (owner decisions, 2026-11-26)

| ID | Status | Decision / unresolved details |
| --- | --- | --- |
| T-01 | Accepted | Hybrid continuous calendar and optional advancement over routine periods; discrete simulation ticks conceptually similar to grand-strategy games, at a cadence suitable for KESTREL rather than necessarily hourly. Provide pause and five selectable speed levels (1–5); actual multipliers and tick intervals are tunable. |
| T-02 | Accepted as playtest target | Approximately one hour of effective play per simulated year is the initial pacing hypothesis, not a fixed clock multiplier or mandatory year length. Era locking remains a separate unconfirmed option. |
| T-03 | Partially accepted | Day/night and seasonal effects should matter, including after-hours grooming. Owner is unsure whether non-skiing periods (late spring, summer, autumn) should be skipped or compressed and does not want repetitive nightly popups. Simplified summer initially; genuine summer activities eventually, including hiking, mountain biking, golf and alpine/mountain coasters or summer toboggan runs. Winter sledding (Schlittenfahren) is also a candidate activity. Specific activities and release scope are not committed. |
| T-04 | Accepted | Pause, five speeds and event-based advancement with configurable stop conditions. Speed changes alone must not change simulation outcomes. Routine operations should not require acknowledgment or repetitive intervention. |
| T-05 | Preferred direction; validate | Owner leans toward instant construction; fun takes precedence over realistic construction schedules. Immediate construction is the recommended default after comparison with competing games. Persistent plans and cosmetic construction effects are compatible. Optional scheduled projects in advanced settings or selected scenarios remain a hypothesis, not a requirement. Winter replacement feasibility is not itself a reason to mandate waiting. |
| T-06 | Accepted direction | Mix historical availability with player-controlled research/organizational expertise and improvements. Prioritize enjoyable progression rather than literal simulation of invention by resort operators. Exact research costs, gates and branches remain open. |
| T-07 | Accepted | Historical eras change more than available equipment. Older equipment can have lower throughput, lower comfort and higher failure rates, with condition and maintenance contributing rather than arbitrary age penalties alone. Broader era-specific tourism, expectations, architecture and regulatory changes are desired; exact data and scope remain open. |
| T-08 | Accepted, configurable | Player chooses a shared empire calendar or independent local resort clocks. Explain consequences clearly before selection. Global mode can use delegated/aggregated inactive operations; local clocks allow playing individual resorts at their own pace. Cross-era capital, technology, acquisitions, competition and connecting ski domains need explicit rules in local-clock mode; no silent time travel or duplicate revenue. Prefer one clock per connected region as a hypothesis to evaluate, not a final restriction. |
| T-09 | Accepted | Scenario-specific deadlines, sustained targets and open-ended objectives; continue playing after success. |
| T-10 | Accepted | Compare alternative time models in human playtesting, using the same scenarios and tracking reasons for speed changes, waiting and interruptions. |
| T-11 | Accepted | Separate rendering, deterministic simulation ticks, operational time, calendar and empire scheduling. Support detailed and aggregated simulation where appropriate; validate equivalence and do not tie state updates to FPS. |

### Construction comparison (public documentation checked 2026-11-26)

- [Transport Fever 2 manual](https://wiki.transportfever2.com/gamemanual/streetstracks.html): preview and confirm infrastructure construction; no separate construction logistics described for ordinary tracks.
- [Anno 1800 blueprint mode](https://www.ubisoft.com/en-gb/help/anno-1800/all-topics?platform=PlayStation+5) allows future planning; [monuments](https://anno1800.fandom.com/wiki/Monuments) are exceptional staged projects rather than the instant ordinary building pattern.
- [Ski-E-O! developer updates](https://steamcommunity.com/app/4493130/allnews/) describe placing and modifying lifts/buildings directly; evidence suggests immediate placement but does not independently prove every construction timing rule.
- [Timberborn official wiki](https://timberborn.wiki.gg/wiki/Construction) makes builders, materials and construction priority part of the core gameplay.
- [Workers & Resources construction guide](https://gridworksguides.com/guides/workers-resources-build-guide.html) describes construction supply chains and labor logistics as gameplay.

Recommendation: default to immediately usable infrastructure after validation/payment; do not add construction logistics by default. Compare optional brief or scheduled projects only if they create meaningful decisions. These references are documentation research, not hands-on comparative playtests.

### Time-model implementation and playtest hypotheses

- Five speed levels are UI levels, not necessarily multipliers 1x–5x. Fixed logical tick/event order; acceleration executes the same transitions or a validated equivalent aggregate. Compare identical seeds, action timestamps and simulated horizons across speed settings; random draws must not depend on wall time, frame rate or batch boundaries. If a deliberately approximate regional model is used, disclose and test its conservation and statistical bounds separately.
- No automatic nightly modal by default. Show non-blocking summaries and optionally stop only for configured meaningful events. Night grooming/snowmaking/maintenance runs under policies; allow skip/advance with explicit stop conditions. For off-season skipping, preserve maintenance, finances, projects, weather and calendar consequences; compare continuous compressed summer with an optional advance to opening.
- One hour/year implies about 75 hours for 1950–2025 at the target effective pace; actual progression varies by pauses and speed. Test daily attachment versus seasonal progress. The existing slice still specifies one seasonal year and technology roughly 1950–1965; separate test saves may demonstrate era variety without requiring fifteen simulated years. Do not change the milestone without owner approval.
- Local-clock mode needs a clear contract before implementation: define how transfers and global research work, what happens to neighboring linked regions, whether inactive regions freeze, how events/deadlines are evaluated and whether mode switching is allowed. Global mode is the coherent baseline; local mode is an accepted player option with unresolved semantics, not a separate simulation implementation.
- Test immediate versus staged construction on the same lift expansion, measuring time until operation, idle time, player satisfaction and desire to plan further. Test routine-day/night advancement and season transitions, plus one meaningful operational interruption. Avoid mandatory daily popups.
- Existing owners: #1735 (calendar/climate), #1736 (seasonal look), #1743 (lifts), #1747 (simulation LOD), #1751 (economy), #1752 (technology), #1755 (save/load); #1740–#1742 (construction). No new issues filed; verify actual status before amending.

## 6. Workshop 3 — guests and mountain networks (owner decisions)

Source context: existing [#1745](https://github.com/drsnuggles8/OloEngineBase/issues/1745) (guest routing), [#1746](https://github.com/drsnuggles8/OloEngineBase/issues/1746) (needs), [#1747](https://github.com/drsnuggles8/OloEngineBase/issues/1747) (population scaling), [#1748](https://github.com/drsnuggles8/OloEngineBase/issues/1748) (skier animation), [#1743](https://github.com/drsnuggles8/OloEngineBase/issues/1743) (lift operation), [#1751](https://github.com/drsnuggles8/OloEngineBase/issues/1751) (economy/reputation), [#1754](https://github.com/drsnuggles8/OloEngineBase/issues/1754) (overlays). These are existing owners, not instructions to change the current backlog now. The owner wants to continue the design workshops first and decide implementation issues later.

| ID | Status | Owner decision and scope |
| --- | --- | --- |
| G-01 | Accepted | Distinguish day visitors from multi-day holiday visitors, including accommodation, visit duration, spending and weather sensitivity. |
| G-02 | Accepted | Guests have individual, continuously varying characteristics and needs (inspectable bars, preferences and activity styles); demographic segments summarize distributions rather than rigid classes. |
| G-03 | Accepted | Resort choice emerges from guest preferences, prices, available alternatives and experience rather than a single attractiveness score. Satisfied visitors may return. Include a no-visit alternative. |
| G-04 | Accepted direction | Families, friends and other organized groups are meaningful. Include school trips (owner example: two teachers and twenty pupils on a week-long trip) and ski-school groups. Shared plans and group coordination should not require manual player control. Exact group sizes and coordination rules are tunable. |
| G-05 | Accepted | Accessible terrain, connected networks and safe return routes are central measures of quality. Support multiple transport/activity modes and interconnected resorts; raw piste length alone is insufficient. |
| G-06 | Accepted with refinements | Recreational route selection balances enjoyment, skill, novelty, congestion and preferences; destination routing prioritizes reaching the destination. Guests follow friends/family/instructors and can exhibit group-following or swarming. Consider time until planned breaks and resort/lift closing when selecting routes. Individual movements must look natural. |
| G-07 | Accepted; configurable disruption | Visible lift queues and network congestion. Beginners may slow boarding/unloading or cause interruptions; surface lifts can have mid-route falls and stops where appropriate. Skill mix and lift type affect actual throughput. Make this disruption mechanic toggleable to avoid frustrating players; exact rate and coupling to safety remain experimental. |
| G-08 | Accepted | Use simplified imperfect information, signs/maps, current published conditions and familiarity; abstraction is preferred when it improves fun. |
| G-09 | Accepted | Ordinary falls, meaningful injury risk and patrol response, with player agency through construction, conditions, safety management and truthful ratings. Keep serious incidents proportionate and avoid graphic/punitive presentation. |
| G-10 | Accepted | Guests autonomously manage their needs. Players provide well-located facilities and services; no individual micromanagement. Aggregate causes and interventions in management UI. |
| G-11 | Accepted | Multidimensional satisfaction and category-specific reputation, with an accessible overall summary. Explain the actual causes. |
| G-12 | Accepted | Every active individual guest is selectable/inspectable with detailed information, thoughts, activities and preferences. Returning visitors and favorites are desired; long-term persistence scope is to be measured, not necessarily a permanent record of every visitor. |
| G-13 | Accepted | Ski schools and experience improve guest skill, allowing visitors to choose more advanced terrain as ability increases. Keep progression calibrated through playtests. |
| G-14 | Accepted direction; fee rules open | Use closing policies, warnings and return-route checks to avoid stranded visitors; staff arrange transport when someone remains stranded. Owner proposes fines/fees for guests. Treat exact eligibility, who pays, amount and responsibility as unresolved; do not punish guests for operator-caused or unavoidable closures by default, and never make access to safe transport contingent on payment. |
| G-15 | Accepted | Extensive data-driven management overlays and forecasts for proposed changes, with estimates rather than guaranteed future outcomes. |
| G-16 | Accepted | Convincing individual skiing, including natural movement and animation; no need for physically exact skiing dynamics. Kinematic/abstract simulation is acceptable when visuals are believable. |
| G-17 | Accepted, strong invariant | LOD is for rendering/presentation, not gameplay effects. Camera, zoom and rendering tier must not change authoritative queues, movement decisions, economic outcomes, satisfaction or accidents. Time-speed invariance remains required. Any explicitly chosen inactive-region aggregation must be distinct from camera/rendering LOD and validated against the same rules. |
| G-18–G-20 | Accepted direction | Prototype mechanics early, test them through small network challenges, and use human playtesting/feedback to decide what is fun before scaling. |
| G-21 | Accepted process decision | Continue game design discussions and record decisions now. Defer creating or amending GitHub implementation issues until later. |

### Guest-network design hypotheses and technical implications (not committed algorithms)

- Keep the authoritative regional network multimodal and directed. A route includes edges, junctions, stations, access permissions, operating schedules and allowed activities. Accessibility requires a viable outward and return route for the guest's ability and time budget, not just reachability at the current instant. Shared terrain segments must not duplicate capacity, occupancy or snow wear.
- Model destination choice and on-mountain recreational choice separately. An expected utility/random-utility model is one candidate; include a no-visit alternative, transparent guest preferences and the same market information/rules for AI operators. Past satisfaction, repeat-visit propensity and familiarity should affect future demand. Avoid guaranteeing loyalty or compounding reputation indefinitely.
- Group state can hold a destination, rendezvous time, leader/activity plan and member IDs. Teachers/instructors and children have explicit group roles. Individual decisions can follow a leader or a shared itinerary, with safe separation/reunion, but do not blindly follow crowds onto closed/inappropriate terrain. Distinguish a social follow decision from local visual flocking/avoidance. Test whether group following creates interesting demand rather than artificial herding loops.
- Use time-dependent routing or bounded look-ahead for meals, group rendezvous, last lift departures and the return to accommodation. Closing warnings should be legible and operational policies configurable, not mandatory evening modals. Unexpected closures still require safe fallback handling. Do not treat fee revenue as a benefit of poor safety planning.
- Distinguish nominal capacity from realized boarding throughput; consider loading time, empty seats, skill mix and interruption durations by lift type. Beginner-related disruptions are an optional rules setting, applied equally to player and AI operators and consistently across speeds. Test whether the option changes equipment/location decisions or merely adds annoyance. No hidden penalty based solely on guest demographic.
- Keep advertised, assessed and dynamic piste difficulty distinct (C-09). Accidents may use a time-integrated hazard model with seed-stable events, not a probability applied per rendered frame. Exact risk parameters, patrol effects and visual reactions need calibration and clear causal UI. Skill gains should be plausible for gameplay pacing, and advanced terrain remains optional.
- Separate authoritative game state from visual reconstruction: stable identity, edge/progress, movement time, decisions, transactions, risk events and queue positions are independent of the camera. Presentation can add individual curves, lateral movement, carving, ski tracks, local avoidance and animation. If collision avoidance affects actual travel time or risk, its relevant effects belong in the authoritative simulation rather than a visible-only tier. Promote/demote rendering without gameplay state changes. Existing #1747 wording should eventually be reconciled with this invariant; do not implement a camera-dependent aggregate economy.
- For a forecast, evaluate a proposed network change against a bounded demand/condition ensemble and report ranges, assumptions and likely shifted bottlenecks. A forecast is not privileged knowledge of actual future RNG. AI should have access to equivalent information.
- Derive guest thoughts, satisfaction breakdowns and management summaries from actual events rather than fabricated complaints. Give each active guest an inspectable identity; benchmark memory/CPU cost. Use a limited repeat-visitor history if necessary without making current guests unselectable.

### Guest-network playtest plan (proposed initial scope)

Build a small, playable laboratory with roughly 100–300 visitors, two lift types, branching pistes, one food facility, basic costs/ticket revenue, weather/closure conditions, and traffic/accessibility overlays. These are experiment sizes, not release requirements. Reuse the production simulation where practical.

1. **The bottleneck:** congested main lift and an underused sector; compare capacity upgrades with new routes and demand specialization.
2. **The inaccessible summit:** great terrain with no easy return route; observe whether players discover the access constraint and enjoy designing alternatives.
3. **The divided mountain:** two complementary ski domains with poor connectivity; test the payoff of integration and sensitivity to closure of a critical link.
4. **School trip / closing time:** a week-long group, mixed ability, a ski-school lesson, a meal rendezvous, and a changed lift closing or weather hold. Observe coordination, natural movement and whether warnings are useful without becoming chores.
5. **Beginner lift disruption A/B:** same network, seed and actions, with the optional boarding/fall disruption mechanic enabled and disabled. Compare queue behavior, player choices, perceived fairness and frustration.

Measure construction and intervention choices, comprehension of bottlenecks and return routes, voluntary exploration/expansion, satisfaction with feedback, perceived fairness and frustration. Use paired seeded automated tests for capacity conservation, deterministic speed/camera invariance, group identity and closing-time fallback; use human tests for fun and readability. Do not equate an increase in guest population or a passing simulation test with improved player enjoyment.

## 7. Workshop 4 — economy and business (owner decisions)

Owner confirms the economic mantra: easy to understand, difficult to master, and deeply connected to the resort physically constructed. Construction remains the main activity; the economy gives it meaning. Fun takes precedence over accounting realism.

| ID | Status | Owner decision and scope |
| --- | --- | --- |
| E-01 | Accepted | Combine accessible attraction management in the style of RollerCoaster Tycoon with Transport Fever-style infrastructure, demand and capacity economics. Abstract utilities/materials; no industrial supply chains as a core requirement. |
| E-02 | Accepted direction; model open | Player-built facilities should generate income. Direct operating income and leasing/concession income are candidates; owner suggests a rent-like or tax-like payment from third-party businesses. Exact ownership, revenue split and operating responsibilities remain open. Do not count third-party turnover as operator revenue. |
| E-03 | Accepted | Price passes for the ski resort/domain, not individual lifts. Day, half-day, multi-day, season and other appropriate pass products; group discounts. Support configurable dynamic pricing policies, potentially unlocked by research/expertise; gating is a proposal, not final. |
| E-04 | Accepted | Multiple viable business strategies: affordable high-volume, premium, family, small local, and other context-dependent profiles; visitor count alone does not define success. |
| E-05 | Accepted | Meaningful operating costs and opening/resource allocation decisions, with routine operation delegated to policies. |
| E-06 | Accepted | Maintenance and aging create economic decisions; automated by default, with optional fine-tuning that rewards understanding the system. Keep safety floors and avoid mandatory repetitive clicks. |
| E-07 | Accepted | Investment planning should include forecasts of likely demand, costs, financial outcomes and uncertainty. |
| E-08 | Accepted | Different loan products and public investment applications for specific projects; local/regional and potentially other government funding. Exact products, eligibility, conditions and terms remain open. |
| E-09 | Accepted | Weather and snow conditions affect cash flow; failing to save sufficient reserves before a bad season should create meaningful challenges. Preserve seasonal costs and income even when off-season is accelerated or skipped. |
| E-10 | Accepted direction | Mismanagement must have consequences; banks and government may offer recovery options. Configurable financial failure remains supported; no unconditional bailout guarantee. Specific distress, refinancing, rescue and bankruptcy rules require design/playtesting. |
| E-11 | Accepted | Targeted marketing campaigns: choose audience/location, spending and message (owner references Hollywood Animals as inspiration). Awareness and actual resort experience should interact; no guaranteed guests for spending money. |
| E-12 | Accepted | Abstract and understandable stakeholder/government relationships, project approvals and occasional meaningful decisions rather than political micromanagement. Land rights and protected areas remain meaningful. |
| E-13 | Accepted | One worldwide game currency, no exchange rates. Include some form of gradual inflation or rising prices over time. Exact currency name, nominal/real presentation, rate, treatment of wages/revenue and historical comparison remain open. |
| E-14 | Accepted direction; configurable | Local resort accounts plus empire capital allocation/support, with configurable rules where appropriate. Cross-era transfers and local/global clock semantics remain unresolved. |
| E-15 | Accepted | Conventional management-game financial UI, with accessible summaries and optional details, history and asset/sector breakdowns. |
| E-16–E-18 | Accepted | Automated balancing/exploit tests and human playtesting are essential. Start with a small playable economy prototype; retain the construction-led economic philosophy. |

### Economy design hypotheses and invariants (not final balancing)

- Model facility ownership separately from facility operation. A player-owned building may be directly operated or leased to a concessionaire. Compare a simple rent/lease contract versus direct operation; do not require hotel or restaurant micromanagement. A government tax is distinct from rent collected by a resort. The player should know which operating costs and revenue they bear. No mandatory third-party system in the initial prototype.
- Keep an auditable transaction ledger with clear categories; cash, operating contribution, debt principal, interest and capital expenditure are distinct. Internal transfers cancel in consolidated accounts; never count a pass sale or third-party turnover twice. Avoid double charging multi-day/season pass holders and distinguish admissions from repeated lift rides.
- Pricing policies can set weekday/peak/off-peak, season, conditions and group discounts, with clear floors/ceilings and explanations. Test whether advanced pricing unlocks are fun rather than making basic usability a research reward. Do not introduce per-lift pricing for ordinary skiing.
- Maintenance automation must be competent by default. Advanced optimization should trade off preventive cost, reliability and disruption, not reward repeated slider adjustments or unsafe operation. Equipment wear, age/design and utilization can be separate state variables.
- Public funding is project-specific, e.g. access, regional tourism, training or event infrastructure. Eligibility, conditions and co-financing should be visible; a rescue is not free renewable cash. Banks and governments may help with viable turnaround plans but repeated poor decisions should carry consequences. AI operators use the same rules.
- A simple inflation index I(y) can increase nominal wages, prices and construction/operating costs gradually. Use a consistent unit/accounting basis and distinguish real versus nominal changes for historical comparisons. Do not inflate only costs without considering revenue and purchasing power. Existing fixed nominal debt contracts should retain their terms unless explicitly indexed. The index path and UI are hypotheses, not a calibrated historical series.
- Marketing should change awareness of a destination among selected groups/markets rather than directly creating guaranteed demand. Message/actual experience mismatch may harm reputation or loyalty. Compare campaign outcomes with a no-campaign baseline and account for repeat visitors.
- Forecasts must show assumptions and uncertainty; no privileged knowledge of future weather/demand RNG. The same information and economic constraints apply to AI. Test multiple viable strategies using common seeds, including poor winters, debt distress, competition and different geography. Human enjoyment remains a separate acceptance dimension.
- Inactive-region aggregation, camera LOD and speed settings must not create economic advantages; preserve accounting and deterministic invariants as agreed in Workshops 2–3. Define explicit rules for cross-era capital before implementing local-clock empire transfers.

### Initial economy playtests (proposed)

1. First profitable winter: day tickets, one food facility, basic construction and operating costs, modest starting cash and a simple loan.
2. Resort on the brink: overbuilt infrastructure and debt; test understandable recovery options and consequences.
3. Difficult winter: compare reserves, snowmaking, selective closures and higher-altitude development.
4. Two resorts, two visions: fair competition and multiple profitable market specializations.
5. Pricing/maintenance automation A/B: test whether optional advanced policies add meaningful optimization without making default play tedious.
6. Facility ownership experiment: compare direct operation with concession income, holding construction and guest service benefits comparable; observe whether the choice is understandable and interesting.

Measure time to first meaningful investment, idle/wait time, ability to explain cash versus profit and forecasts, voluntarily chosen strategies, response to bad seasons, fairness, and motivation to expand. Record seeded runs and human feedback separately. All prices, interest rates, inflation rates and other numerical values in the workshop are illustrative.

## 8. Workshop 5 — snow, weather and geography (owner decisions)

Snow is a core gameplay system, not merely a cosmetic effect. Favor realistic, locally varying environmental conditions, meaningful preparation and understandable player interventions. Official maps should start from real mountain data where possible; authored adjustments and fictional maps remain valid. No destructive natural disasters in the game.

| ID | Status | Owner decision and scope |
| --- | --- | --- |
| W-01 | Accepted | Geography is a strategic resource: realistic local snowfall, slope, elevation, aspect, wind exposure, geology, water and avalanche susceptibility affect development and operations. |
| W-02 | Accepted | Official maps should have distinct gameplay identities and be modeled on actual mountain data where possible; this builds on the existing real-terrain roadmap. |
| W-03 | Accepted | Correlated regional weather and local microclimate variation. Use climate data and, where obtainable and suitable, historical resort snow reports to establish and validate regional historical inputs. Sources, rights, quality, coverage and resolution must be checked; no source is yet selected. |
| W-04 | Accepted | In-depth persistent spatial snow simulation is a priority: different snow types, accumulation, depth, density, surface state, local conditions, usage and history. Scientific fidelity is subordinate to gameplay and computational budgets, but snow should not be reduced to a cosmetic or global quality scalar. |
| W-05 | Accepted | Skier traffic changes snow condition and wear; width, traffic distribution and grooming matter. Authoritative wear is independent of camera/rendering LOD. |
| W-06 | Accepted | Intermediate snowmaking model: physical equipment and reservoirs, water availability and weather-dependent production; pipes, electricity and detailed engineering abstract initially. |
| W-07 | Accepted | Automated grooming using fleets, capacity and priorities. Players may explicitly leave areas ungroomed. Some terrain exceeds the capabilities of available grooming technology; later equipment/expertise may make more terrain feasible. Exact thresholds and equipment types are open. |
| W-08 | Accepted | Useful forecasts with uncertainty and operational implications; historical technology may improve information. Same information constraints for AI and human operators. |
| W-09 | Accepted direction; configurable | Local wind and equipment-specific operating limits matter; allow players to disable or simplify realism-related weather interruptions. Exact settings and scope remain open. |
| W-10 | Accepted with modification | Abstract avalanche control/protection and risk assessment rather than destructive avalanche simulation. Avalanche hazard is particularly relevant to off-piste guests. Owner prefers not to have avalanches damage or directly affect lifts. Closures/guest exposure/mitigation are gameplay candidates; the exact scope of piste closures and safe fallback is to be prototyped. |
| W-11 | Accepted | Configurable long-term regional climate evolution, distinct from year-to-year variability; affects strategies across decades. |
| W-12 | Accepted | Abstract environmental impact and government/development relationships, with region-specific rules and expectations (e.g. Austria versus USA); avoid excessive micromanagement. No assertion that a particular country's actual laws have been modeled. |
| W-13 | Accepted direction | Optional night skiing with suitable lighting and operating hours; potential tech-tree unlock. Exact gating and economics remain hypotheses. |
| W-14 | Accepted | Real geographical data as the preferred starting point, with authored gameplay adjustments and fictional maps also supported. Data-driven maps and eventual community content are compatible; release/editor scope open. |
| W-15 | Accepted | Environmental and historical planning overlays, strategic suitability, current conditions and forecasts, consistent with earlier resort-map decisions. |
| W-16 | Accepted with explicit exclusion | Configurable environmental rules and fair presets; no destructive natural disasters, including as an advanced setting. Weather challenges should remain understandable and manageable. |
| W-17–W-18 | Accepted | Prototype interactions early; weather should present meaningful, fair choices and clear responses. Teach interventions through tutorials, documentation, contextual warnings and SimCity-style advisors (e.g. economy, traffic, mountain operations), not just raw statistics. |

### Environmental hypotheses, constraints and invariants

- Regional climate inputs can combine historical observations, reanalysis and suitable resort reports; distinguish modeled weather from measured snow depth, snowfall, on-piste/artificial snow and natural snow. Resort reports may be inconsistent, incomplete, proprietary, or biased toward marketing; document provenance, rights, units, measurement elevation, time period and uncertainty. Do not claim precise historical piste conditions from coarse regional data. Use climate distributions and local terrain downscaling, and validate plausible seasonal and elevation-dependent behavior. Exact datasets and historical coverage are open.
- A persistent snowpack grid should distinguish mass (snow water equivalent), density, depth and surface state. Snowmaking adds water-equivalent mass, grooming compacts/redistributes it, traffic changes wear, and melt/drainage removes it. Maintain nonnegative states and conservation where applicable. A coarse undeveloped grid and finer operational areas are candidates; gameplay resolution must not depend on camera position. Render snow from authoritative state; cosmetic detail can use LOD.
- Snow types and conditions should matter to different guests, piste difficulty, closures, grooming workload and snowmaking. Test whether snow texture/appearance and maps convey the underlying causes. No final numerical thresholds or physical parameterization yet.
- Grooming policies include no grooming/preserve powder, priority routes and quality targets. Equipment capability (including slope limits, potentially assisted/winch grooming) and historical availability can change feasible operations. Do not assume technology makes all steep slopes groomable. Routine dispatch should be automated; fine-tuning is optional.
- Snowmaking water and temperature/humidity constraints remain meaningful. Abstract energy and plumbing initially; compare more detailed network variants only if they improve gameplay. Forecasts use available information, not future RNG.
- Avalanche protection is a preventative service/operational decision; focus on guest exposure, especially optional off-piste activity, and understandable warnings. Do not implement destructive disasters or lift damage from avalanches. Design off-piste access, guest awareness, patrol response and closures explicitly before implementation. Avoid presenting game hazard models as real safety advice.
- Environmental restrictions and public approvals are data-driven by region/scenario, historically informed where practical. Protected areas remain meaningful; avoid arbitrary universal regional stereotypes. Night skiing is optional and can interact with daylight, maintenance, energy and demand; exact tech unlock is open.
- Advisors should provide symptom, likely cause, impact, recommended options and links to the affected assets/overlay. Teach player agency through contextual first-time guidance and optional tutorials, avoid repetitive modal pop-ups, allow snooze/mute and configurable escalation. Advice is not mandatory automation or a guaranteed solution.
- Disabling realism mechanisms must have explicit semantics and apply symmetrically to human and AI operators. No camera/time-speed advantage; no destructive disaster mode. Scenario-authored challenges can have known constraints without hidden AI bonuses.

### Initial environmental playtests (proposed)

1. Unreliable valley: compare snowmaking, higher-altitude expansion and selective operation; test whether players understand the underlying snow reliability.
2. Exposed summit: compare redundancy, alignment and equipment; A/B weather-interruption setting.
3. Powder weekend: snowfall followed by clear conditions; compare preparation, intentionally ungroomed zones and demand opportunities.
4. Every drop counts: scarce reservoir water and multiple viable snowmaking priorities.
5. Two mountains, two strategies: comparable starting resources on distinct real-data maps; test whether geography changes construction strategy.
6. Off-piste risk and advice: test warnings, abstract avalanche control, guest behavior and safe operational responses without destruction.
7. Advisor A/B: compare contextual causal advice with a plain notification/overlay, measuring comprehension, interruption burden and independent problem-solving.

Use seeded experiments for weather/snow state consistency, snow mass accounting, forecast calibration, speed/camera invariance and fairness. Human testing must establish whether choices are enjoyable, consequences clear, and weather friction feels fair. All example rates and thresholds remain illustrative.

## 9. Workshop 6 — empire, acquisitions and competition (owner decisions)

| ID | Status | Owner decision and scope |
| --- | --- | --- |
| A-01 | Accepted | Distinguish geographical regions, individual resorts, physically connected ski domains and owning resort groups. Ownership, access rights, commercial integration and physical connectivity are separate. |
| A-02 | Accepted | Empire saves may begin in different eras and starting conditions: greenfield development, an existing resort, distressed acquisition or an established portfolio. |
| A-03 | Accepted | Geographic expansion through land sectors, concessions, acquisition and new base areas. Region sizes follow geography; the initial 10 × 10 km map is not a universal limit. |
| A-04 | Accepted with refinement | Buy operating businesses, not empty plots: infrastructure, condition, accounts, obligations, reputation and development potential matter. Negotiation, rival bids and limited availability make acquisition opportunities meaningful. Provide due diligence before commitment. |
| A-05 | Accepted | Dynamic valuations and simplified negotiation, grounded in resort assets, liabilities, operations and development potential; no repetitive negotiation minigame. |
| A-06 | Accepted | Integration is a substantial, potentially multi-year and capital-intensive project: physical links, commercial access, operations, branding and capacity. Purchasing alone does not deliver a functional mega-resort. No mandatory lengthy construction timers implied; integration can require successive investments and operating results. |
| A-07 | Accepted | Cooperation without ownership changes: shared passes, joint infrastructure and other commercial arrangements are valid options. Scope and revenue allocation to prototype. |
| A-08 | Accepted | Fully autonomous AI builders and empire operators are a full-game goal: they develop actual resorts, acquire land and businesses, and manage their portfolios. Distinct ambition, aggressiveness and skill, inspired by Anno competitors. Deliver in stages; not a requirement for the initial slice. |
| A-09 | Accepted | AI difficulty improves decision quality rather than granting hidden advantages. Personality and competence are separate dimensions. AI uses the same authoritative gameplay commands, information, finances, permissions and rules as human players. |
| A-10 | Accepted | Genuine competition for guests, land, opportunities and acquisitions; no sabotage or human-targeted artificial hostility. Competition intensity is configurable. |
| A-11 | Accepted with refinement | Globe/world interface inspired by Planet Zoo. Investment opportunities appear over time as land becomes available or sellers offer resorts. Access depends on capital, opportunity and business credibility/reputation, not arbitrary map unlock levels. An operator's demonstrated management record should help acquire resorts. Exact selection and bidding rules remain open. |
| A-12 | Accepted direction; configurable | Delegated managers and configurable authority; players may instead choose inactive/frozen resorts or simplified automatic revenue for unattended resorts. Full simulation, frozen and simplified modes have different semantics to define and test. No unapproved permanent construction/demolition by default. |
| A-13 | Accepted starting point; revisitable | Global or independent regional calendars. Directly connected or competing resorts within a region share a clock. Cross-era finance and technology sharing configurable; exact rules and defaults are open and may be revised. |
| A-14 | Accepted | Research and organizational expertise generally shared across an empire to avoid repeating the same progression. Respect historical availability and local adoption where appropriate. |
| A-15 | Accepted | Geographic, seasonal and customer-market diversification should provide genuine financial resilience through underlying economics, not an artificial stability multiplier. |
| A-16 | Accepted | Voluntary resort sales; no surprise hostile takeovers. Loss of a resort may follow severe mismanagement under explicitly enabled insolvency rules, with clear consequences and recovery opportunities. |
| A-17 | Accepted with refinement | Players establish empire identity/branding at save creation (name, logo, colors, positioning) and can revise it over time. Individual resorts retain or adapt their identity and marketing; acquisition includes deciding how the new destination fits the brand. Historical records and milestones desirable. No arbitrary cosmetic bonuses. |
| A-18a | Accepted | Empire growth introduces genuinely different resort experiences and new opportunities to build, while specializing in one outstanding resort remains an equally valid playstyle. |
| A-18 | Accepted | Empire dashboard and exception-driven notifications. Avoid interrupting active resort construction or forcing switches; escalate genuinely important situations and allow direct navigation. |
| A-20 | Accepted | Prototype competition, integration, multi-region management, autonomous builders and portfolio growth progressively; use human playtests to verify additional enjoyment rather than repetitive administration. |

Note: A-18 retains the original dashboard decision ID (section 19); A-18a records the owner’s separate feedback on the section 18 gameplay example, and A-20 records the section 20 prototype agreement.

### Empire hypotheses, constraints and invariants

- A periodic acquisition market can expose limited-time or conditional opportunities, multiple bidders, seller reservation values and alternative buyers. Make offer expiry, eligibility and competing interest visible enough for meaningful choices; avoid constant attention demands, arbitrary scarcity or guaranteed player success. Reputation for managing businesses may improve seller/government confidence, but should not override all financial or strategic criteria. Whether sealed bids, counteroffers or a simple offer/acceptance loop are best is a playtest question.
- Distinguish equity purchase price from inherited debt and cash, and from integration capital expenditure. Dynamic valuation is a candidate, not a mandate for a DCF solver. Use explainable assumptions, ranges and due diligence, including adverse seasons and maintenance liabilities; avoid counting synergy twice. All illustrative prices, rates and timings are uncalibrated.
- Integration may span multiple years through staged investments, rights, network links, commercial agreements and operational coordination, even when individual construction is instant by default. A shared pass, common ownership and a physically connected ski domain are independent states. Synergy must emerge through accessible terrain, guest demand, spending and costs; account for return routes and new congestion. Shared-pass revenue must be conserved and not reward artificial transfers.
- AI operators use the same command validation and forecasts as human operators. Difficulty and personality may alter planning horizon, risk appetite, candidate search, market positioning and skill, not resources or knowledge of future randomness. Authored scenario asymmetries are explicit. Autonomous physical construction should be staged from operation to improvements to new networks to portfolio decisions; validate affordability, feasibility and visual plausibility. No sabotage.
- Inactive-region modes require explicit rules. Delegated full-equivalent operation is the coherent candidate for global-clock competitive play; frozen regional play fits independent calendars; simplified revenue is an optional relaxed abstraction, not a claim of exact simulation equivalence. Define whether costs, time, wear, debt, bookings, demand, research and rival activity advance in each mode. Do not silently generate risk-free money or allow switching modes to erase liabilities. Do not freeze only one competitor in an otherwise live connected region. Decide mode restrictions, transition rules and transfer limits before implementation. Player camera/rendering LOD never changes gameplay within a given mode. Time acceleration alone remains outcome-invariant.
- Shared expertise does not automatically unlock equipment from the future in a historically earlier region. Cross-era funding, inflation adjustment, technology propagation and region synchronization remain unresolved. Do not force repetition of identical research. Global mode uses coherent consolidated accounts; local mode may use separate finances or explicitly relaxed sharing. Exact rules are not approved yet.
- Group and resort identity can coexist. Retain local names, maps and heritage where desired; rebrand voluntarily. Historical records, achievements and marketing can support attachment without turning cosmetics into a dominant optimization strategy. One-resort play should not be punished for declining empire expansion.
- Portfolio resilience is an emergent consequence of imperfectly correlated weather and demand, with real overhead and capital commitments. No automatic percentage bonus for owning multiple resorts. Sales and severe distress must follow transparent rules; no arbitrary hostile takeover or forced loss of a beloved resort.
- Default managers handle routine operations within policy and budgets; permanent layout changes require explicit authorization. Empire notifications should be nonmodal by default, grouped and prioritized, with optional escalation/auto-pause and direct links to the relevant resort. Avoid making opportunity deadlines or ordinary alerts interrupt local creative flow.

### Initial empire playtests (proposed)

1. Two resorts, one region: authored competitors with symmetric rules; test specialization, pricing and intelligible competitive outcomes.
2. Across the Ridge: acquisition versus shared-pass cooperation; compare multiple physical connections and observe whether integration is rewarding beyond the purchase itself.
3. Contested opportunity: limited availability, rival bids and buyer credibility; test whether negotiation is engaging and fair without creating repetitive busywork.
4. Second region: switch between distinct mountains; compare global/delegated, independent/frozen and explicitly simplified operation. Test notifications, financial consistency, player pressure and attachment.
5. Living competitors: autonomous improvements and construction under validated commands; compare AI personalities and skill levels without hidden advantages.
6. Growing group: compare one-resort mastery and multi-resort portfolio play across repeated sessions. Measure voluntary expansion, differentiated strategies, delegation trust and repetition burden; neither playstyle should be mandatory.

Technical tests should cover conserved money and ownership transfers, permission checks, shared-pass settlement, no cross-clock arbitrage under strict rules, deterministic competition, save/load across regions and fair AI command execution. Human tests must establish whether acquisitions, integration and competition add enjoyment. No implementation or milestone change authorized by this workshop alone.

### Next workshop — scenarios and sporting events (agenda, not decisions)

Define RCT-style scenario structure, outcome-based objectives, curated difficulty and progression, rescue and expansion challenges, regional merger missions, fictional international winter sporting events, post-event legacy, replayability, and scenario authoring/validation. Consider whether completed scenarios can connect to an empire save without undermining scenario balance. Distinguish launch content, initial prototypes and eventual ambitions.

## 10. Workshop 7 — scenarios and sporting events (owner decisions)

| ID | Status | Decision / remaining question |
| --- | --- | --- |
| S-01 | Accepted | Standalone authored scenarios, dynamic empire opportunities and sporting events share a reusable objective system. |
| S-02 | Accepted with refinement | Curated thematic scenario catalogue with a somewhat structured progression. Advanced scenarios should not be the initial/default choice for inexperienced players; avoid frustration and confusion. Not a completely rigid campaign. Exact unlock, recommendation and override rules remain open. |
| S-03 | Accepted | Prefer outcome-based objectives with multiple solutions; specific facilities are appropriate when intrinsic to the premise. |
| S-04 | Qualified acceptance | Bronze/silver/gold are acceptable as a familiar convention, but the owner is not particularly enthusiastic about medals. Optional challenges/side quests are supported. Test whether tiers add value or distract from meaningful completion; do not require every scenario to use medals. |
| S-05 | Accepted direction | Proposed scenario variety fits the vision. Split the overly broad sporting-events/advanced-management category into distinct themes. Catalogue entries and launch count are proposals, not commitments. |
| S-06 | Accepted | Recurring sporting events in sandbox and empire, not solely authored scenarios. |
| S-07 | Accepted | Compete with AI operators to host events using credible bids, reputation, infrastructure, plans and funding; same rules and information for all. |
| S-08 | Accepted direction | Prioritize alpine racing, freestyle skiing and snowboarding; other disciplines can follow where justified. |
| S-09 | Accepted | Visible simulated competitions (option B): athletes, spectators, schedules and lightweight results simulation; no requirement for playable sports or team management. |
| S-10 | Accepted | Major events have a lasting legacy: reputation, future demand, reusable infrastructure, ongoing costs and post-event financial viability. |
| S-11 | Accepted | Advanced missions may have meaningful preparation, test events, execution and evaluation phases. |
| S-12 | Accepted with caution | Optional dynamic opportunities and announced developments may enrich scenarios; avoid arbitrary scripted setbacks, excessive scripting or unfair surprises. |
| S-13 | Accepted | Normally continue after success or failure, without unearned achievements; explicitly enabled insolvency/failure rules may constrain continuation. |
| S-14 | Accepted direction | Completed standalone scenarios can become new empire saves; authored challenge templates may arise in existing empires. Cross-save imports into existing competitive empires remain unresolved; preserve accounting and balance. |
| S-15 | Low priority | Authored difficulty variants and randomized replay conditions acceptable, but replaying the same scenario is not a major design priority. Prefer a varied catalogue and strong sandbox/empire replayability. |
| S-16 | Accepted direction | Given OloEngine's public development, make it easy for players to create their own missions, scenarios, maps, models and other content. Official content should use the same data-driven foundations. Editor, packaging, compatibility, distribution and scripting/security details remain open. |
| S-17 | Accepted direction | Generic declarative objective framework; exact schema and expressive capabilities to be validated against real scenarios. |
| S-18 | Accepted, high importance | Tutorial must be entirely optional, immediately engaging, and teach core mechanics through play. Avoid lengthy mandatory tutorials, delayed action, or dumbing down the game. Contextual advisors and progressively optional guidance are preferred. |
| S-19 | Accepted | Official scenarios require automated feasibility/balance checks and human evidence of comprehension, fairness and enjoyment. |
| S-20 | Accepted | Initial scenario prototypes: The First Lift, Too Popular for Its Own Good, A Resort on the Brink, Across the Ridge. Prototype order and release content remain separate decisions. |
| S-21 | Deferred | Public demo content and packaging to be discussed later; do not define it prematurely or design the game around a showcase. |
| S-22 | Owner direction | Maintain an ambitious full-game vision; owner expressed this as “Ambition is not a dirty word. Piss on compromise. Go for the throat.” Do not reduce the product vision merely to make implementation easier. Staged prototypes are learning and integration checkpoints, not a mandate to dilute the final game. Exact production scope, dates and release contents remain owner decisions. |

### Scenario catalogue (proposed, not launch promises)

- Founding and development: The First Lift (1950), A Mountain for Everyone (1965), Reach for the Summit (1975).
- Rescue and modernization: A Resort on the Brink (1985), The Aging Giant (2000), The Winter That Never Came (modern era).
- Networks and integration: Too Popular for Its Own Good (1990), Across the Ridge (1980), The Three Valleys (modern era; working name only).
- Environment and specialization: Powder Paradise (1995), The Long Night (1980), Beyond the Winter (modern era).
- Sporting events: The Winter Games (1988), Extreme Winter Festival (2005); local/regional races and international alpine/freestyle championships are further candidate formats.
- Prestige and empire management (separate from sporting events): The Crown of the Alps, The Mountain Magnate (modern era).

All titles, dates, parameters and event names are illustrative; real-world branding and event names require rights review. The game need not simulate every winter sporting discipline. Across the Ridge is an important signature gameplay validation scenario, not a predetermined public demo.

### Scenario design hypotheses and invariants

- Use the shared simulation, not a parallel scenario-specific economy or guest model. Scenario definitions contain initial state, rules, objectives, event triggers, deadlines, optional rewards and completion/continuation policy. Use stable IDs and serializable state, and version content/schema. Express conditions using thresholds, windows, deadlines, conjunctions, alternatives, events and trends as needed; resist inventing a general scripting language before actual content requires it.
- Objectives should measure intended results. Queue reduction must not be achievable merely by admitting no visitors; use appropriate observation windows, minimum sample sizes/throughput, and availability or service constraints. Higher achievement tiers, if retained, should add meaningful mastery rather than arbitrary number inflation. A bronze or basic completion is a legitimate success; optional goals should not distract from the main problem.
- Guided scenario availability should help inexperienced players find suitable content without preventing experienced users from choosing harder scenarios unnecessarily. Test recommendation, warnings, initial unlocks and bypass options. Do not conflate scenario guidance with arbitrary sandbox/empire map level locks.
- Sporting event lifecycle: opportunity, bid, award, preparation, optional test events, competition, results and legacy. Bidding requirements and future commitments must be legible; event organizers and public funding may support viable projects. Competitive bids must use symmetric rules. Missing an event deadline is a clear consequence of a known obligation, not arbitrary sabotage. Permanent vs temporary facilities, regional partnerships and reuse of ordinary pistes are candidate decisions. Model spectators and athletes visibly, keep sporting outcomes lightweight, and measure ordinary guest displacement and post-event costs as well as event revenue and reputation. No destructive natural disasters.
- Keep the standalone scenario economy independent from existing competitive empire saves. Converting to a new empire is the coherent starting point. In-empire templates use existing ownership, financial and calendar state. Define any creative-mode imports explicitly to avoid duplicated assets, cash or achievements.
- Broad modding ambition: separate content data from hardcoded rules; support documented asset/map/scenario formats, stable identifiers, explicit dependencies, validation and versioning. OloEngine being public does not itself establish the game's asset license or a public mod distribution policy; these remain to be decided. Prefer safe declarative content by default, with trust boundaries for executable mods. Official content and community content should not require unrelated bespoke simulation paths.
- Tutorial hypothesis: let the player construct a functioning lift/piste and see guests using it promptly, teach controls in context, then offer a real choice and withdraw guidance. Optional and skippable at all times. Do not simplify the underlying game just to onboard players; use progressive disclosure of information. Exact pacing targets must be tested with new players, not inferred from developer familiarity.

### Initial scenario validation (proposed)

1. The First Lift: construction, early operating loop and optional onboarding; measure time to first meaningful action, first guest and independent expansion decision.
2. Too Popular for Its Own Good: diagnose a real network bottleneck and try multiple viable interventions.
3. A Resort on the Brink: understand debt, operating costs and recovery; test whether financial distress offers real agency.
4. Across the Ridge: acquisition or cooperation, physical and commercial integration, accessibility and resilience; test satisfaction with the transformation.
5. Small sporting event prototype: test course preparation, crowd routing, funding and visible event payoff before attempting a major multi-resort championship.

Use seeded automated runs, reference strategies, objective exploit tests, save/load and deadline boundary checks, followed by blind human sessions. Do not treat numerical solvability as evidence of fun. Scenario replay variants are optional and lower priority than variety across scenarios and the full sandbox/empire experience.

### Next workshop — progression and replayability (agenda, not decisions)

Define early, mid and late game motivations, historical and organizational research, meaningful technological alternatives, self-directed goals, optional milestones, continued one-resort play versus empire growth, map diversity, long-term attachment, community content and playtest evidence of sustained interest. Do not reopen approved decisions by default or add compulsory progression gates. Public demo decisions are deferred.

## 11. Workshop 8 — progression and replayability (owner decisions)

| ID | Status | Decision / remaining question |
| --- | --- | --- |
| P-01 | Accepted | Overlapping physical, technological, economic, geographical progression and player mastery; no generic player XP gate. |
| P-02 | Accepted | Resort development stages emerge from scale, complexity, finances and reputation; not hardcoded resort levels. Milestones can recognize achievements. |
| P-03 | Not approved; defer | Owner is skeptical of persistent player-authored multi-blueprint development projects, citing possible bug abuse. Do not mandate a project-management or commitment system. This does not revoke ordinary construction previews, maps or previously agreed planning tools. If revisited, demonstrate value and exploit-safe semantics first. |
| P-04 | Accepted | Multiple interconnected research branches relevant to resort strategies, without permanently exclusive classes. |
| P-05 | Accepted direction; validate | Research budgets, priorities and operating experience are plausible, but must be playtested for boredom and monotony. Exact acquisition of expertise, pacing, automation and anti-grind incentives remain open. |
| P-06 | Accepted direction | Early adoption and participating in development of emerging technologies are desirable. Balance potential benefits against costs and uncertainty; historical windows and manufacturer/development mechanics remain hypotheses. |
| P-07 | Accepted | New equipment is not always a one-to-one superior replacement; horizontal alternatives and valid uses for simpler equipment alongside genuine upgrades. |
| P-08 | Accepted | Demonstrated operator reputation and competence improve opportunities without arbitrary experience-level locks. |
| P-09 | Accepted | Natural late-game challenges from scale, infrastructure, competition and changing conditions; successful resorts may remain stable and profitable. No automatic difficulty escalation to punish success. |
| P-10 | Accepted | Ambitious projects, landmarks and milestones offer long-term goals; not every project must be financially optimal. |
| P-11 | Partially settled | Technology tree eventually ends and play continues indefinitely. Exact end era, era locking semantics and possible post-modern or speculative improvements are deferred for further discussion; no requirement for speculative technology. |
| P-12 | Accepted | Contextual optional contracts, development opportunities and events; no compulsory quest grind. |
| P-13 | Accepted | Multiple viable identities and scales; becoming a mega-resort is neither necessary nor feasible for every mountain. |
| P-14 | Accepted | Persistent infrastructure history, preservation, refurbishment and modernization support attachment. |
| P-15 | Accepted | Historical resort archive with milestone events, statistics, historical piste maps and optional screenshots. |
| P-16 | Accepted | Different starting eras, existing infrastructure, competitors and land ownership on the same geographical map. |
| P-17 | Accepted | Reproducible sandbox randomization of weather, markets, rivals and opportunities without losing geographical identity. |
| P-18 | Accepted direction | Sensible presets, advanced settings, saved rulesets and selected in-game configuration changes. Exact safe transitions and achievement effects remain open. |
| P-19 | Accepted | Player-wide achievements and completion records rather than account-level gameplay advantages in unrelated saves. |
| P-20 | Strong acceptance | Broad moddability of data, assets, models, maps, scenarios and eventual gameplay extensions; official content uses the same documented foundations where practical. |
| P-21 | Strong acceptance | Sharing maps, building templates, blueprints, screenshots and complete resort states. Terrain-dependent imports require validation. |
| P-22 | Accepted | Assess new mechanics by the meaningful interactions they add, not just asset counts or unlock counts. |
| P-23 | Accepted | Reject artificial retention mechanics: no daily login rewards, real-world expiry pressure, mandatory grind or purposeless waiting. Players can stop after accomplishing their own goals. |
| P-24 | Accepted direction | Engagement-horizon timeline (Workshop 8 section 23) is a reasonable starting hypothesis, not a guaranteed retention curve or mandatory progression rate. |

Owner's final two numbered replies were interpreted by their explicit wording: response 23 accepts the proposed timeline; response 24 rejects artificial retention mechanics (the original P-23 question in section 25). The detailed longitudinal testing approach in section 24 remains a proposed method consistent with existing playtesting commitments.

### Progression design implications (proposed unless explicitly accepted above)

- Physical creation and observation remain the core activity at every scale. Avoid arbitrary gates, universal upgrade ladders, compulsory empire expansion or perpetual crises. A small, well-run local resort is a valid end state, and some geography cannot support a mega-resort.
- Research can be empire-wide, while equipment remains constrained by historical availability and local adoption. Candidate early-adopter agreements, trial deployments and manufacturer partnerships should have clear risks and benefits. Basic interface quality and relief from repetitive chores should not be locked behind research. Avoid rewarding pointless asset spam or repetitive clicks. Compare passive budget allocation with more interactive, meaningful choices in human playtests.
- Do not conflate P-03 with P-21 or C-10: reusable layouts/blueprints and construction previews are still useful, but a persistent multi-project management system with reserved resources, financial commitments or deferred construction is not approved. If a planning feature is revisited, keep it non-authoritative until validated commit; recheck costs, land rights, terrain, technology, ownership and capacity at commitment. No duplication of assets or money via undo, importing, save/load or plan replay.
- Heritage and archives: record stable asset identity and construction/refurbishment/replacement events, use compact historical statistics and versioned network/map snapshots rather than retaining full world snapshots indefinitely. Screenshot frequency, storage, retention and export formats are open. Historical records should be genuine outputs of the save, not invented promotional images.
- Modding/sharing: documented versioned content formats, stable IDs, dependency resolution, validation, compatibility/migration and clear trust boundaries. Data/asset mods should not require engine recompilation. Validate imported terrain-dependent designs; separate cosmetic export from authoritative game-state import. Game licensing, distribution, executable extension security and official/community achievement policies remain open.
- Custom rulesets should explain the consequences of changes. Preserve simulation invariants when changing speed, camera or display settings; explicitly define transitions for changes to ownership, clocks, economics, research or competitive rules. Scenario scoring remains based on declared rules.
- The final historical technology tree has a finite endpoint; do not terminate the save at that point. Era-lock and speculative future details remain undecided. Avoid adding fictional future content merely to prolong play.

### Progression validation (proposed)

- First five minutes: grasp the fantasy and begin building; first 30 minutes: operate infrastructure and see meaningful consequences; first two hours: develop a functioning resort; around ten hours: distinctive destination and understanding of systems; around thirty hours: major development, modernization or competition; 50–100 hours: historical/region/ambition opportunities; beyond that: mastery, creative goals and community content. These are flexible engagement hypotheses, not a required playtime per player.
- Test early sessions, three-session returns, approximately ten-hour experiences, mature-resort starting saves, later historical eras, second-region experiences and extended empire play. Include players who prefer one resort. Observe self-directed goals, meaningful technology choices, boredom, waiting, voluntary expansion and return behavior. Interview rather than inferring enjoyment from telemetry alone.
- Evaluate multiple viable resort identities under varied geography and market conditions; use automated exploit/balance checks and human validation. Test early adoption against waiting for mature equipment, and research models for perceived monotony. No retention manipulation; a satisfied player finishing a resort is a legitimate outcome.

### Next workshop — UX and art direction (agenda, not decisions)

Define the visual identity, camera and construction interaction, game HUD, map styles, overlays, causal inspection, advisors and notification controls, onboarding, accessibility, audio, animation and time/scale presentation. Ensure the game remains approachable without dumbing down the simulation, and that the 1080p target and performance budget shape readable, representative visual acceptance. Public demo and release-content decisions remain deferred.

## 12. Workshop 9 — UX and art direction (owner decisions)

| ID | Status | Decision / remaining question |
| --- | --- | --- |
| U-01 | Accepted | Romantic Alpine realism: physically believable photoreal environments with grandeur, comfort, activity and transformation; adapt to regional character. |
| U-02 | Accepted | Preserve realistic proportions; adapt information and presentation to viewing distance without changing simulation. |
| U-03 | Accepted; high priority | Excellent terrain-aware strategy camera, top-down precision, object following, bookmarks and optional cinematic/guest perspectives. |
| U-04 | Accepted direction; mockup rejected | Minimal HUD with essential configurable indicators and contextual panels. Owner dislikes the proposed space-heavy layout and colors; do not adopt that mockup as a requirement. Explore historically evolving UI themes. |
| U-05 | Accepted | Progressive disclosure: approachable summaries and optional operational, financial and historical detail. |
| U-06 | Accepted | Inspect meaningful simulation entities, relationships, comparison and bulk edits where appropriate. |
| U-07 | Accepted; high priority | Intuitive responsive construction, clear contextual validation, optional snapping/precision and consistent controls. |
| U-08 | Accepted direction; explore | Interactive resort map, selectable styles, useful construction/management interaction and shareable exports. Explore as a design problem rather than assuming the mockup is final. |
| U-09 | Accepted | Prioritize decision-useful, intuitive overlays, including historical and forecast views; avoid exposing variables merely because they exist. |
| U-10 | Accepted | Advisors have recognizable responsibilities and personalities; legitimate advice may conflict because priorities differ. Extensive suppression, disable and notification controls. Facts should remain consistent with simulation. |
| U-11 | Accepted | Policy templates and hierarchical overrides with visible inheritance and manual intervention. |
| U-12 | Accepted; accuracy requirement | Short optional playable onboarding, contextual advice and searchable in-game encyclopedia. Explicit owner concern: protect encyclopedia from becoming stale or incorrect. |
| U-13 | Accepted direction | Researched historical and regional visual identities, with existing assets retaining their character. Explore UI themes evolving with historical era too. |
| U-14 | Accepted | Optional architectural customization and reusable templates, alongside ready-made buildings and resort identity. |
| U-15 | Accepted; high priority | Believable skier and lift operation and visible consequences are core visual requirements; snowboarding, sledding and other supported activities must feel convincing too. |
| U-16 | Accepted direction | Realistic conditions with optional inspection aids; allow players to reduce cumbersome visual effects or aids. No authoritative gameplay changes from viewing options. |
| U-17 | Accepted with qualification | Expected gameplay graphics preset must look amazing; lowest presets may sacrifice visual detail and fidelity. Preserve essential decision information and simulation semantics. Existing target hardware/fps remains unchanged. Exact quality tier and acceptance imagery need validation. |
| U-18 | Accepted; high priority | Audio is fundamental to atmosphere: spatial ambience, machinery, guests, weather and configurable music. |
| U-19 | Accepted | Guest following, lift-passenger and ground-level perspectives; playable skiing remains a future decision. |
| U-20 | Accepted | Photo mode, saved viewpoints, historical comparison and sharing. |
| U-21 | Accepted | Accessibility, localization readiness, configurable controls, scaling and color-independent information. |
| U-22 | Accepted direction | Formal reusable UI design system; potentially multiple historically appropriate themes. Shared interaction conventions should remain consistent. |

### UX implementation and validation hypotheses (not additional owner-approved requirements)

- Prototype a compact, collapsible HUD and several visual directions in actual gameplay at 1080p; do not use the rejected slate-heavy mockup as a baseline. Consider era-dependent skins using shared semantic tokens, components, interaction patterns and accessibility contracts rather than duplicating UI implementations. A stable theme override is a candidate accessibility/comfort option. Exact palettes, fonts, layouts, themes and historical transition rules remain open.
- Advisors can disagree on priorities (e.g. expansion versus reserves) but should not contradict the same measured facts. Show evidence, assumptions, time horizons, alternatives and relevant map/object links. Test notification batching, suppression and user-controlled auto-pause. No forced interruption for ordinary advice.
- Treat the encyclopedia as versioned product content. Generate numerical facts, availability, names and rule-dependent values from authoritative runtime definitions/registries; authored explanations reference stable mechanic IDs and are reviewed when those contracts change. Candidate CI checks: broken references, stale schema/content versions, missing localization, documented examples vs seeded simulation tests, and a change checklist for player-facing rules. Do not assume generation alone guarantees correct conceptual explanations. Support modded/rule-dependent values and distinguish current settings from defaults. Bundle help with the matching game build.
- UI/visual quality acceptance is distinct from unit-test completion. Test camera movement on steep and large terrain, construction discoverability and correction, selection, causal inspection, map/overlay usability, advisor actionability and policy inheritance. Inspect actual captured gameplay, including believable skiing, snowboarding, sledding, lift queues and operations, snow/weather appearance and audio. Use representative hardware and test multiple presets, allowing aesthetic compromises on the lowest preset without concealing necessary gameplay information.
- Photo and inspection previews must not alter authoritative time, weather, snow or financial state. Historical images/maps are genuine saved records. Keep photo tools and gameplay input distinct.
- Test optional tutorial with first-time players; time to meaningful construction, observed use by guests and an independent decision are stronger signals than completing a click-through sequence. Avoid extensive mandatory explanation. Help, advisors and UI themes must agree on current rules.
- Proposed acceptance dimensions: interaction, comprehension, presentation, audio, technical correctness, performance and human gameplay validation. Keep debug/diagnostic tools separate from player-facing UI. Engine/game boundary remains ADR 0027.

### Next workshop — playtesting and product validation (agenda, not decisions)

Define prototype hypotheses and sequencing, automated simulation and human evidence, tester recruitment and observation, questionnaires and opt-in telemetry, reproducible feedback and triage, balancing experiments, UX/art/audio evaluation, longitudinal tests, acceptance gates and agent workflow. Consolidate the full design register into a repository-ready structure only after owner review; no public demo or release scope decisions yet.

## 13. Playtesting framework (historical proposal; superseded where revised by Workshop 10 in §15)

- Write a design hypothesis before implementing substantial mechanics. Record expected player decisions and observable outcomes.
- Use deterministic simulation experiments to detect impossible scenarios, dominant strategies, instability and economic exploits; use human sessions to establish comprehension, satisfaction, pacing and fairness.
- Historical proposal: conduct initial observed sessions with a small varied group, then repeated blind/return sessions and larger closed tests. Superseded by V-05–V-07: volunteer, primarily unassisted Steam-first testing with lightweight in-game feedback and optional direct observation.
- Collect telemetry only with informed opt-in; allow inspection and withdrawal, avoid unnecessary personal information, and keep raw feedback/save snapshots out of the public repository by default. Use private storage/access controls and retention limits.
- Track findings by build, scenario, hypothesis, evidence, severity, confidence and owner; retest changes instead of assuming a fix improved enjoyment. Publish anonymized summaries only.

## 14. Proposed repository integration

Recommended canonical location: `docs/design/kestrel/` (consistent with the existing root docs index), with `README.md` as the entry point and one document per approved topic. Link it from `docs/README.md` and the shared game/agent guidance. A local project docs directory is also possible if the owner prefers it; avoid two sources of truth. Keep approved decisions separate from hypotheses and tunables. Use stable IDs and cross-links rather than copying content into `AGENTS.md` and `CLAUDE.md`.

After each workshop, record accepted decisions, rejected alternatives and why, unresolved questions, playtest hypotheses and affected issue owners. Do not modify the north-star milestone or create implementation tasks from unapproved ideas. Where current issues conflict with an accepted design, update their scope before work starts. The owner decides scope and publishing; agents can implement, measure and suggest revisions.

Workshop format preference: put each decision question immediately after its relevant section, rather than collecting all questions at the end. Free-form responses are valid; do not require questionnaires.

Historical note: no repository write was performed for v0.11, which superseded v0.10.

## 15. Workshop 10 — playtesting and product validation (owner decisions)

Owner feedback: 2026-11-26, 23:49 CET. This completes the initial ten workshops. The owner is a solo indie developer; there is no planned budget for paying playtesters. Product decisions below supersede earlier proposals for labor-intensive moderated sessions and private executable distribution. No change to the June 2027 vertical slice is implied.

| ID | Status | Decision / remaining question |
| --- | --- | --- |
| V-01 | Accepted | Each substantial gameplay mechanic has a design hypothesis, player-facing intent and validation plan alongside technical acceptance. |
| V-02 | Accepted | Track technical correctness, simulation balance, usability and human enjoyment independently. |
| V-03 | Accepted | Reusable Gameplay Labs for focused and integrated testing, using actual authoritative KESTREL rules. |
| V-04 | Accepted with refinement | External testing begins as soon as the core loop works and a build can be easily distributed, preferably via Steam. Early external testing need not wait for finished art. |
| V-05 | Accepted with resource constraint | Recruit volunteers from friends and advertised community opportunities; seek varied management-game experience, skiing expertise and hardware, and introduce fresh testers where feasible. No budget assumed for paid participants or professional research services. Sample sizes and cohort proportions are not quotas. |
| V-06 | Revised by owner | Formal moderated sessions and facilitator-led procedures are probably too labor-intensive for a solo indie. Default to independent, unassisted testing with useful built-in observations, feedback and opt-in telemetry. Occasional direct observation or conversation is optional, not a required protocol. Collect only purpose-relevant data under V-10. |
| V-07 | Accepted with qualification | Short, neutral, optional questionnaires and mechanic-specific questions; feedback must never become a chore. Prefer low-friction prompts over mandatory post-session forms. |
| V-08 | Accepted, high priority | In-game feedback reporter, positive and negative experiences, optional reviewed diagnostic attachments and reproducible state. Integrate early enough to support the first external tests. |
| V-09 | Accepted, high priority | Opt-in, schema-versioned gameplay telemetry centered on meaningful gameplay actions, interpreted alongside qualitative reports. Do not equate engagement metrics with fun. |
| V-10 | Accepted | Explicit informed consent, purpose limitation, data minimization, restricted access, retention rules and private raw feedback/attachments. Avoid automatically publishing tester data in the public repository. Separate consent for optional sensitive attachments/recordings; review privacy requirements before collection. |
| V-11 | Accepted with refinement | Ask testers primarily to describe their experience: what they tried, expected, felt good/bad, and what actually happened. Do not solicit fixes as the primary feedback. Developers and agents diagnose causes and propose responses; unsolicited suggestions may be retained separately as hypotheses. Triage by evidence, severity, affected audience and frequency. |
| V-12 | Accepted | Persistent findings register with stable IDs, evidence, affected design decisions/build, and explicit retest status. Use lightweight storage appropriate to a solo developer. |
| V-13 | Accepted | Headless simulation/experiment framework for large-scale balancing of economic, guest, environmental, competition and scenario rules. Share authoritative gameplay code with the interactive game. |
| V-14 | Accepted | Use controlled comparisons where worthwhile; account for sampling uncertainty, learning effects and stochastic variation. No requirement for formal experiments for every minor decision. |
| V-15 | Accepted | Automated invariant checks, randomized action sequences and adversarial strategies for economy, ownership, time, scenario and other exploits. |
| V-16 | Accepted | Codex/Claude can verify, experiment, reproduce reports and analyze evidence; claims about human enjoyment require actual human evidence. Owner decides product direction. |
| V-17 | Accepted | Gameplay issues include player-facing acceptance and validation plans; technical merge/completion does not imply human design validation. Track the latter separately without requiring human approval for every PR. |
| V-18 | Accepted | Repeatable visual/audio reference scenes, automated regression checks and periodic human artistic review at representative presets. |
| V-19 | Accepted | Longitudinal feedback and representative mature-resort saves; do not infer late-game quality solely from introductory sessions. Adapt recruitment and workload to the volunteer model. |
| V-20 | Accepted | Explicit revisable experience targets for diagnosis, not a rigid numerical definition of fun or a guarantee of commercial success. |
| V-21 | Accepted with explicit distribution decision | Steam-first external playtesting as soon as practical. The only non-Steam playtesting is on the owner's own PC. No manual ZIP/executable distribution or manual upload workflow to individual testers. Establish a convenient repeatable build/publish workflow; exact Steam setup, access, automation and test branch arrangements remain implementation questions. Steam Playtest is the preferred candidate, not a confirmed configured service. |
| V-22 | Accepted with priority | Fun first. Make a great game rather than optimizing its design for marketing or engagement metrics. Commercial validation may inform presentation but must not drive game design. Owner's expectation that quality will attract some players is an aspiration, not proof of market demand or a sales guarantee. Public demo, trailer, pricing and release strategy remain deferred. |
| V-23 | Accepted | Interpret contradictory feedback by audience, underlying experience and intended design. Use configuration selectively, preserve a coherent default and avoid reflexively implementing every preference. |
| V-24 | Accepted | Owner controls approved game-design decisions. Agents and testers may provide evidence and propose revisions; no silent product changes to simplify engineering. |

### Lean solo-indie validation workflow (proposed implementation)

- Local developer testing uses the real game and reusable Gameplay Labs; external volunteer testing uses a Steam-distributed build once the core loop is established. A repeatable build/deploy path is an enabler, not a public release or demo commitment. Verify Steam onboarding/access, build upload credentials and tester access before making external testing dependent on it. No Steam configuration or deployment has been performed in this workshop.
- Prioritize one-click in-game feedback: category, optional free text, contextual snapshot of build/scenario/rules and optional screenshot/save/log attachment with consent and review. Allow submitting positive observations as easily as defects. Ask what happened and how it felt rather than what the developer should implement. Do not interrupt gameplay with mandatory questionnaires.
- Keep telemetry scoped to decisions and diagnostics: time controls, construction attempts and outcomes, route/overlay usage, simulation events, saves/returns and performance, with schema/build/content version and pseudonymous identifiers. Do not collect arbitrary personal data merely because instrumentation is cheap. Session-level data is not automatically anonymous. Prefer aggregated outcomes and bounded event buffers; make opt-in and disabling collection easy. Raw reports remain private; only sanitized findings can be shared with public issues or external agents under appropriate access rules.
- Recruit volunteers through personal contacts and relevant communities; record broad optional player profiles and avoid treating a self-selected convenience sample as representative of buyers. Use asynchronous short follow-ups or occasional observation only if needed to resolve ambiguities. Do not assume paid testers, moderators or guaranteed cohort sizes.
- Keep the process lightweight: report -> triage -> reproduce -> revise -> verify -> targeted retest. Use a small private findings store, stable IDs and generated summaries rather than building a large research platform first. Link to sanitized public implementation issues where useful.
- Headless and interactive simulation must share rules. Compare strategies with independent named random streams, common weather/demand seeds where appropriate, recorded configurations, and held-out seeds for validation. Test invariants for cash, ownership, guest conservation, time speed, camera-independent gameplay and save/reload. Search for dominant strategies without automatically choosing a single numerical definition of good design.
- Separate evidence statuses: proposed, implemented, technically verified, playtest-ready, human-tested, design-validated or revision required. Record actual evidence and limitations. No claim of human validation based solely on an agent's scripted play or telemetry.
- Visual/audio fixture scenes cover opening morning, peak day, snowfall, night operations, summer and guest perspectives. Expected graphics preset is the art reference; low presets maintain gameplay readability, not identical fidelity.
- Treat engagement/pacing targets as hypotheses. For small volunteer samples report counts, context and uncertainty; a high score from friends does not establish market-wide appeal. Track voluntary return, self-directed goals and friction, not raw hours as a success objective.

### Proposed first validation sequence (not an approved roadmap change)

1. Core construction -> functioning lifts/pistes -> guest use -> clear operational/economic outcome -> independent improvement on a small terrain.
2. Local Gameplay Labs for construction, congestion and snow; reusable authored starting states, save/reload and repeatable seeds.
3. Repeatable packaged Steam test build and basic in-game feedback/consent flow, with scoped event instrumentation and private evidence handling; external volunteer tests as soon as the core loop and distribution are ready.
4. Iterate on the core loop, add headless balance experiments and extend to mature starting saves, additional regions, research, competition and acquisitions as the mechanics become playable.

## 16. Post-workshop consolidation (pending owner review)

All ten initial workshops are recorded. The register is a decision history, not a final consolidated game design document or a mandate to implement every proposal. Before reorganizing the roadmap, synthesize the approved decisions into modular specifications, audit cross-system consistency and distinguish binding requirements from prototypes, tunables and unresolved questions. Do not silently approve assistant-generated formulas, UI sketches, illustrative financial numbers, timelines or implementation options.

The first-pass consistency audit is provided separately. No repository writes, new issues, changes to the north-star milestone, Steam setup or tester recruitment have been performed. v0.12 supersedes v0.11; earlier versions remain available.


## 17. Consistency review A — foundational contracts (2026-11-27)

Owner confirms game-design documentation belongs under `OloEditor/KestrelProject/docs/`; the engine's root documentation remains engine-focused. Root agent instructions should route relevant gameplay work to the game specifications. No repository changes or milestone changes authorized here.

| ID | Status | Decision and remaining questions |
|---|---|---|
| AUD-01 | Accepted direction; prototype details open | Owner proposes RollerCoaster Tycoon-style red (closed/construction), yellow (test run), green (open) operating states. Before the first opening, construction corrections and configurable undo refunds remain possible where transactionally safe. Yellow is a meaningful visible test without ordinary paying guests; green activates normal operational participation. Once opened, subsequent structural changes and removal use ordinary modification/demolition costs; returning to red or yellow must not reset refund eligibility. The sticky ever-opened state, short test runs, bulk opening, and simple lifecycle for decorations/buildings are proposed refinements, not separately owner-approved details. Pre-opening terrain, land, demand and dependency effects must be deferred or handled transactionally; no duplicate refunds, income or restoration. Exact refund rates and testing duration remain experimental. |
| AUD-02 | Accepted baseline | One coherent continuous calendar with pause and five speed levels; configurable intelligent fast-forward targets and stop conditions. Owner explicitly rejects player-configurable calendar compression or day/year pacing: central gameplay requires a consistently balanced model. Internal experiments may compare pacing parameters; approximately one hour per year is a playtest hypothesis, not a shipping guarantee. All intervening economic, weather, snow, guest and research consequences must be processed; speed must not silently change outcomes. Off-season presentation and acceleration remain open. |
| AUD-03 | Accepted | Preserve random state by default; optional reseeding of future uncertainty, deterministic action replay and save/load lineage for diagnostics. Owner explicitly rejects Ironman-style mode. Do not punish ordinary reloads. Independent keyed RNG domains and stable event identifiers are proposed implementation approaches. Restore authoritative state including randomness, validate camera/preset/speed/headless/save equivalence for identical actions, and record version/seed provenance. Exact reseed scope and lineage persistence remain implementation questions; no intrusive anti-cheat. |
| AUD-04 | Accepted | Separate physical corridor, directed navigation segments/junctions and named piste identity/classification. Distinguish responsibility for stranded guests: fees may apply when clear warnings were ignored; operator bears costs for operator-caused or unexpected failures. Recovery is available irrespective of payment. |
| AUD-05 | Accepted direction | Gameplay-driven multiresolution snow state independent of camera; preserve snow mass/history across refinement, and keep visual tracks separate from authoritative wear. Benchmark adaptive spatial grids and possible piste-aligned operational fields; avoid double ownership of snow mass. Numerical representation and resolution are engineering experiments, not product decisions. |
| AUD-06 | Accepted | Rigorous internal accounting for assets, liabilities, depreciation, cash, revenue, capex, debt and consolidated empire finances, with accessible management UI. Lightweight double-entry ledger is preferred. One universal currency and consistent inflation model remain as previously approved. |
| AUD-07 | Accepted | Ownership, operating responsibility, construction rights and commercial access independently representable. Acquisitions transfer relationships and agreed liabilities, not physical copies of regions/assets. |
| AUD-08 | Accepted for prototyping | Compare all three empire configurations: living empire (global calendar, detailed delegated operation), relaxed empire (global calendar, simplified operations), independent regional sandbox (local calendars, frozen inactive regions). Physically connected/directly competing resorts within one region share a clock. Cross-era transfers, expertise restrictions, debt/maintenance and mode transitions need explicit contracts. No accidental profit from changing simulation fidelity. Exact defaults remain open pending tests. |

Owner ranks the hardest foundational problems AUD-02, AUD-08 and AUD-01. These warrant targeted playable experiments. Competitor calendar observations from the preceding chat are contextual, not hands-on validation or a specification. The owner chose not to expose compressed calendar alternatives as player settings. No gameplay implementation, GitHub change, Steam setup or test has been performed here.
