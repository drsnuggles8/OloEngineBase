# Integrated checkpoints and roadmap reconciliation

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Preserve full-game ambition and the current vertical-slice milestone unless explicitly revised. Build progressively playable integrated experiences rather than only isolated subsystems. Existing open and closed issues may conflict with the new design; audit actual code and issue acceptance criteria before proposing revisions. Reuse sound work and avoid duplicate ownership.

Suggested sequence, not approved issue order or dates: first descent → functioning business → congestion problem → difficult winter → entire year → different mountain → connected resorts → growing empire. Steam/feedback preparation starts early in parallel where possible. The First Fun Prototype is a validation checkpoint, not a replacement milestone.

Checkpoint plan reconciled with the backlog on 2026-11-28 (EXP-17 v0.3, owner decisions [AUD-21](../gameplay/guests-and-networks.md#aud-21) to [AUD-28](../gameplay/geography.md#aud-28)): CP0 foundation, CP1 First Descent ([#1865](https://github.com/drsnuggles8/OloEngineBase/issues/1865)), CP2 First Business ([#1866](https://github.com/drsnuggles8/OloEngineBase/issues/1866)), CP3 congestion as the First Fun Prototype ([#1867](https://github.com/drsnuggles8/OloEngineBase/issues/1867)) and the first external Steam playtest ([#1868](https://github.com/drsnuggles8/OloEngineBase/issues/1868), [AUD-26](steam-and-feedback.md#aud-26)), CP4 difficult winter, CP5 whole year on the real mountain, CP6 the vertical slice. Checkpoints set no dates; each carries representative-scale evidence from the start ([AUD-21](../gameplay/guests-and-networks.md#aud-21)).

## Decision records

<a id="aud-18"></a>

### AUD-18

**Disposition:** Accepted with owner concern.

Preserve ambitious full-game vision and existing June 30, 2027 vertical slice unless explicitly revised. Organize work around progressively playable integrated checkpoints. Owner notes existing GitHub issues no longer fully reflect approved design; audit both completed and open work against consolidated specifications and actual repository state, not just issue titles. Reuse sound work, document contradictions and dependencies, and propose changes before revising milestone or backlog. No new issue order/dates approved here. Public demo/release strategy remains deferred.

## Validation and acceptance (proposed test focus)

For each checkpoint define an executable play loop, technical evidence, human test plan and remaining risks. No backlog or milestone modifications in this package.

## Related design decisions

VISION.md, [V-01](playtesting.md#v-01), [V-03](playtesting.md#v-03), [V-17](automated-balancing.md#v-17), [AUD-16](steam-and-feedback.md#aud-16)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
