# Fair competition and AI operators

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Full-game competitors are autonomous builders and business operators, eventually managing their own empires. Human and AI commands use the same costs, permissions, demand, finances, forecasts and rules. No sabotage, hidden player-targeting, subsidies, privileged future information or arbitrary AI bonuses.

Difficulty and opponent selection should reflect Anno-like personalities: friendly/cooperative to aggressive/ambitious. Skill is decision quality, separate from ambition and risk preference. Rivals may win scarce land, acquisitions and event bids. Inform players through configurable alerts rather than reserving every opportunity. Exact AI implementation, deadlines and preset mapping remain open.

## Decision records

<a id="a-08"></a>

### A-08

**Disposition:** Accepted.

Fully autonomous AI builders and empire operators are a full-game goal: they develop actual resorts, acquire land and businesses, and manage their portfolios. Distinct ambition, aggressiveness and skill, inspired by Anno competitors. Deliver in stages; not a requirement for the initial slice.

<a id="a-09"></a>

### A-09

**Disposition:** Accepted.

AI difficulty improves decision quality rather than granting hidden advantages. Personality and competence are separate dimensions. AI uses the same authoritative gameplay commands, information, finances, permissions and rules as human players.

<a id="a-10"></a>

### A-10

**Disposition:** Accepted.

Genuine competition for guests, land, opportunities and acquisitions; no sabotage or human-targeted artificial hostility. Competition intensity is configurable.

<a id="aud-10"></a>

### AUD-10

**Disposition:** Accepted with owner emphasis.

Players choose the kind of competitors they face through difficulty/presets, with an Anno-like spectrum from friendly, relaxed operators to ambitious and aggressive rivals. Real competition, scarce opportunities and competing offers remain valid; no AI cheating, sabotage or hidden targeting. Filtered opportunity notifications and configurable intelligent advancement remain consistent with AUD-02. Separate personality (preferences, willingness to cooperate, ambition, risk appetite) from skill (decision quality); preset mappings, opponent selection UI, bid deadlines and escalation behavior are proposed refinements to playtest, not finalized settings. Friendly rivals still pursue their own interests.

## Validation and acceptance (proposed test focus)

Compare decisions across personalities and skill under common seeds; audit command validation, budgets and information symmetry; test competition without constant interruption.

## Related design decisions

[D-007](../VISION.md#d-007), [A-04](empire-and-acquisitions.md#a-04), [A-05](empire-and-acquisitions.md#a-05), [S-07](scenarios-and-events.md#s-07), [AUD-02](time-and-history.md#aud-02)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
