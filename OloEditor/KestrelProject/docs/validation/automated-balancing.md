# Automated balancing, replay and acceptance

Status: consolidated v1.0 draft for repository adoption, 2026-11-27. Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

One authoritative simulation for game, headless runner, labs and replay. Use versioned fixtures, seeds, configurations and action traces; compare alternative strategies with common random numbers where appropriate and held-out validation seeds. Check invariants and adversarial action sequences, including money, guests, ownership, time, snow and scenario completion. Never optimize one score as a substitute for design or human feedback.

Separate technical verification, playtest readiness, human testing and design validation. Technical PRs can merge without pretending human playtesting took place. Track artistic/audio review alongside regressions and performance.

## Decision records

<a id="v-13"></a>

### V-13

**Disposition:** Accepted.

Headless simulation/experiment framework for large-scale balancing of economic, guest, environmental, competition and scenario rules. Share authoritative gameplay code with the interactive game.

<a id="v-14"></a>

### V-14

**Disposition:** Accepted.

Use controlled comparisons where worthwhile; account for sampling uncertainty, learning effects and stochastic variation. No requirement for formal experiments for every minor decision.

<a id="v-15"></a>

### V-15

**Disposition:** Accepted.

Automated invariant checks, randomized action sequences and adversarial strategies for economy, ownership, time, scenario and other exploits.

<a id="v-16"></a>

### V-16

**Disposition:** Accepted.

Codex/Claude can verify, experiment, reproduce reports and analyze evidence; claims about human enjoyment require actual human evidence. Owner decides product direction.

<a id="v-17"></a>

### V-17

**Disposition:** Accepted.

Gameplay issues include player-facing acceptance and validation plans; technical merge/completion does not imply human design validation. Track the latter separately without requiring human approval for every PR.

<a id="v-18"></a>

### V-18

**Disposition:** Accepted.

Repeatable visual/audio reference scenes, automated regression checks and periodic human artistic review at representative presets.

<a id="aud-19"></a>

### AUD-19

**Disposition:** Accepted.

Interactive game, headless runner, Gameplay Labs and deterministic replay execute the same authoritative gameplay rules and command validation. Test deterministic invariants across supported execution modes; separately track technical correctness, performance, human playtesting and design validation. Automated balancing informs rather than determines design.

## Validation and acceptance (proposed test focus)

Headless/rendered/replay equivalence, statistical experiments, exploit tests and human validation records. See acceptance template.

## Related design decisions

[AUD-03](../gameplay/time-and-history.md#aud-03), [AUD-05](../gameplay/snow-and-weather.md#aud-05), [AUD-06](../gameplay/economy.md#aud-06), [V-01](playtesting.md#v-01), [V-02](playtesting.md#v-02)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
