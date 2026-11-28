# KESTREL — game design register

Status: working draft v0.6, 2026-11-26 (Workshop 4 owner decisions on economy and business recorded). This records the discussion with the project owner; it is not an implementation mandate. Proposed mechanics must be explicitly accepted before becoming requirements. Balancing examples are not final numbers.

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

### Next workshop — snow, weather and geography (agenda, not decisions)

Discuss regional climate, altitude/aspect/exposure, local weather and forecast uncertainty, spatial snowpack, snowmaking water and operating constraints, grooming, closures and avalanche abstractions, climate trends, land/environment constraints, map data and how each mechanic creates actionable decisions. Keep simulation detail proportional to gameplay and ensure weather is fair, explainable and consistent across speed and camera. Prioritize prototypes before production scope.

## 8. Playtesting framework (proposed, detail to refine)

- Write a design hypothesis before implementing substantial mechanics. Record expected player decisions and observable outcomes.
- Use deterministic simulation experiments to detect impossible scenarios, dominant strategies, instability and economic exploits; use human sessions to establish comprehension, satisfaction, pacing and fairness.
- Conduct initial observed sessions with a small varied group, then repeated blind/return sessions and larger closed tests. Use behavior, short questionnaires and interviews together.
- Collect telemetry only with informed opt-in; allow inspection and withdrawal, avoid unnecessary personal information, and keep raw feedback/save snapshots out of the public repository by default. Use private storage/access controls and retention limits.
- Track findings by build, scenario, hypothesis, evidence, severity, confidence and owner; retest changes instead of assuming a fix improved enjoyment. Publish anonymized summaries only.

## 9. Proposed repository integration

Recommended canonical location: `docs/design/kestrel/` (consistent with the existing root docs index), with `README.md` as the entry point and one document per approved topic. Link it from `docs/README.md` and the shared game/agent guidance. A local project docs directory is also possible if the owner prefers it; avoid two sources of truth. Keep approved decisions separate from hypotheses and tunables. Use stable IDs and cross-links rather than copying content into `AGENTS.md` and `CLAUDE.md`.

After each workshop, record accepted decisions, rejected alternatives and why, unresolved questions, playtest hypotheses and affected issue owners. Do not modify the north-star milestone or create implementation tasks from unapproved ideas. Where current issues conflict with an accepted design, update their scope before work starts. The owner decides scope and publishing; agents can implement, measure and suggest revisions.

Workshop format preference: put each decision question immediately after its relevant section, rather than collecting all questions at the end. Free-form responses are valid; do not require questionnaires.

No repository write has been performed for this draft. v0.6 supersedes v0.5; previous versions are retained for comparison.
