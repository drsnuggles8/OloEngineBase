# Scenarios and sporting events

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Offer authored challenges, dynamic opportunities and special events through reusable objective infrastructure. Guide newcomers through a somewhat structured scenario catalogue, not a completely rigid campaign. Outcome-based goals should allow multiple solutions. Medals are a qualified convention, not the core reward; optional objectives matter more. Scenario replayability is lower priority than sandbox replayability.

Sporting events are recurring opportunities, with fair bids, preparations, visibly simulated athletes/spectators, evaluation and long-term legacy. Prioritize alpine racing and freestyle/snowboarding; other disciplines remain open. No separate playable sports game is required. Continue after success or failure except explicit insolvency rules. Completed standalone scenarios can become new empire saves; standard cross-save asset import is unresolved.

Customized scenarios remain playable. Disclose gameplay-affecting changes and official-completion eligibility before changes where possible; no intrusive anti-cheat. Cosmetic-only changes should not disqualify completion.

## Decision records

<a id="s-01"></a>

### S-01

**Disposition:** Accepted.

Standalone authored scenarios, dynamic empire opportunities and sporting events share a reusable objective system.

<a id="s-02"></a>

### S-02

**Disposition:** Accepted with refinement.

Curated thematic scenario catalogue with a somewhat structured progression. Advanced scenarios should not be the initial/default choice for inexperienced players; avoid frustration and confusion. Not a completely rigid campaign. Exact unlock, recommendation and override rules remain open.

<a id="s-03"></a>

### S-03

**Disposition:** Accepted.

Prefer outcome-based objectives with multiple solutions; specific facilities are appropriate when intrinsic to the premise.

<a id="s-04"></a>

### S-04

**Disposition:** Qualified acceptance.

Bronze/silver/gold are acceptable as a familiar convention, but the owner is not particularly enthusiastic about medals. Optional challenges/side quests are supported. Test whether tiers add value or distract from meaningful completion; do not require every scenario to use medals.

<a id="s-05"></a>

### S-05

**Disposition:** Accepted direction.

Proposed scenario variety fits the vision. Split the overly broad sporting-events/advanced-management category into distinct themes. Catalogue entries and launch count are proposals, not commitments.

<a id="s-06"></a>

### S-06

**Disposition:** Accepted.

Recurring sporting events in sandbox and empire, not solely authored scenarios.

<a id="s-07"></a>

### S-07

**Disposition:** Accepted.

Compete with AI operators to host events using credible bids, reputation, infrastructure, plans and funding; same rules and information for all.

<a id="s-08"></a>

### S-08

**Disposition:** Accepted direction.

Prioritize alpine racing, freestyle skiing and snowboarding; other disciplines can follow where justified.

<a id="s-09"></a>

### S-09

**Disposition:** Accepted.

Visible simulated competitions (option B): athletes, spectators, schedules and lightweight results simulation; no requirement for playable sports or team management.

<a id="s-10"></a>

### S-10

**Disposition:** Accepted.

Major events have a lasting legacy: reputation, future demand, reusable infrastructure, ongoing costs and post-event financial viability.

<a id="s-11"></a>

### S-11

**Disposition:** Accepted.

Advanced missions may have meaningful preparation, test events, execution and evaluation phases.

<a id="s-12"></a>

### S-12

**Disposition:** Accepted with caution.

Optional dynamic opportunities and announced developments may enrich scenarios; avoid arbitrary scripted setbacks, excessive scripting or unfair surprises.

<a id="s-13"></a>

### S-13

**Disposition:** Accepted.

Normally continue after success or failure, without unearned achievements; explicitly enabled insolvency/failure rules may constrain continuation.

<a id="s-14"></a>

### S-14

**Disposition:** Accepted direction.

Completed standalone scenarios can become new empire saves; authored challenge templates may arise in existing empires. Cross-save imports into existing competitive empires remain unresolved; preserve accounting and balance.

<a id="s-15"></a>

### S-15

**Disposition:** Low priority.

Authored difficulty variants and randomized replay conditions acceptable, but replaying the same scenario is not a major design priority. Prefer a varied catalogue and strong sandbox/empire replayability.

<a id="s-17"></a>

### S-17

**Disposition:** Accepted direction.

Generic declarative objective framework; exact schema and expressive capabilities to be validated against real scenarios.

<a id="s-18"></a>

### S-18

**Disposition:** Accepted, high importance.

Tutorial must be entirely optional, immediately engaging, and teach core mechanics through play. Avoid lengthy mandatory tutorials, delayed action, or dumbing down the game. Contextual advisors and progressively optional guidance are preferred.

<a id="s-19"></a>

### S-19

**Disposition:** Accepted.

Official scenarios require automated feasibility/balance checks and human evidence of comprehension, fairness and enjoyment.

<a id="s-20"></a>

### S-20

**Disposition:** Accepted.

Initial scenario prototypes: The First Lift, Too Popular for Its Own Good, A Resort on the Brink, Across the Ridge. Prototype order and release content remain separate decisions.

<a id="s-21"></a>

### S-21

**Disposition:** Deferred.

Public demo content and packaging to be discussed later; do not define it prematurely or design the game around a showcase.

<a id="s-22"></a>

### S-22

**Disposition:** Owner direction.

Maintain an ambitious full-game vision; owner expressed this as “Ambition is not a dirty word. Piss on compromise. Go for the throat.” Do not reduce the product vision merely to make implementation easier. Staged prototypes are learning and integration checkpoints, not a mandate to dilute the final game. Exact production scope, dates and release contents remain owner decisions.

<a id="aud-12"></a>

### AUD-12

**Disposition:** Accepted with transparency requirement.

No intrusive anti-cheat. Some gameplay-affecting modifications, rules changes or objective alterations can disqualify official scenario completion. Inform players clearly of eligibility rules and consequences before changes where possible; keep ordinary save continuation and customized completion available. Cosmetics/UI/audio should not invalidate completion when they have no gameplay effect. The exact eligibility matrix, permitted reseeding, content version compatibility and classification of mixed mods remain open. Do not imply tamper-proof certification in a single-player moddable game.

## Validation and acceptance (proposed test focus)

Four initial scenario prototypes; test objective exploits, solvability, event readiness/legacy, guidance, customized eligibility and continuation.

## Related design decisions

[T-09](time-and-history.md#t-09), [P-19](progression-and-research.md#p-19), [AUD-17](../content/modding-and-sharing.md#aud-17), [V-01](../validation/playtesting.md#v-01), [V-19](../validation/playtesting.md#v-19)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
