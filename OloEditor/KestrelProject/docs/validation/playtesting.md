# Human playtesting and findings

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Solo indie constraints govern process: unpaid volunteers, primarily independent/unassisted play, short optional feedback and no mandatory moderated sessions. Test as soon as the core loop and Steam distribution are ready. Ask what happened and how it felt, not primarily how to fix it. Keep evidence, hypotheses and retest status distinct. Recruit different backgrounds and hardware where feasible; convenience samples are not representative population estimates.

Fun first. Human enjoyment requires actual human evidence; agents assist reproduction and analysis. Track longitudinal/mature saves as well as onboarding. Owner controls approved design changes.

## Decision records

<a id="v-01"></a>

### V-01

**Disposition:** Accepted.

Each substantial gameplay mechanic has a design hypothesis, player-facing intent and validation plan alongside technical acceptance.

<a id="v-02"></a>

### V-02

**Disposition:** Accepted.

Track technical correctness, simulation balance, usability and human enjoyment independently.

<a id="v-03"></a>

### V-03

**Disposition:** Accepted.

Reusable Gameplay Labs for focused and integrated testing, using actual authoritative KESTREL rules.

<a id="v-04"></a>

### V-04

**Disposition:** Accepted; V-21 and AUD-16 control distribution.

External volunteer testing starts once a core loop exists and Steam distribution is easy, before final graphics or full vertical slice. Local owner-only testing outside Steam. No manual executable distribution.

<a id="v-05"></a>

### V-05

**Disposition:** Accepted with resource constraint.

Recruit volunteers from friends and advertised community opportunities; seek varied management-game experience, skiing expertise and hardware, and introduce fresh testers where feasible. No budget assumed for paid participants or professional research services. Sample sizes and cohort proportions are not quotas.

<a id="v-06"></a>

### V-06

**Disposition:** Revised by owner.

Formal moderated sessions and facilitator-led procedures are probably too labor-intensive for a solo indie. Default to independent, unassisted testing with useful built-in observations, feedback and opt-in telemetry. Occasional direct observation or conversation is optional, not a required protocol. Collect only purpose-relevant data under V-10.

<a id="v-07"></a>

### V-07

**Disposition:** Accepted with qualification.

Short, neutral, optional questionnaires and mechanic-specific questions; feedback must never become a chore. Prefer low-friction prompts over mandatory post-session forms.

<a id="v-11"></a>

### V-11

**Disposition:** Accepted with refinement.

Ask testers primarily to describe their experience: what they tried, expected, felt good/bad, and what actually happened. Do not solicit fixes as the primary feedback. Developers and agents diagnose causes and propose responses; unsolicited suggestions may be retained separately as hypotheses. Triage by evidence, severity, affected audience and frequency.

<a id="v-12"></a>

### V-12

**Disposition:** Accepted.

Persistent findings register with stable IDs, evidence, affected design decisions/build, and explicit retest status. Use lightweight storage appropriate to a solo developer.

<a id="v-19"></a>

### V-19

**Disposition:** Accepted.

Longitudinal feedback and representative mature-resort saves; do not infer late-game quality solely from introductory sessions. Adapt recruitment and workload to the volunteer model.

<a id="v-20"></a>

### V-20

**Disposition:** Accepted.

Explicit revisable experience targets for diagnosis, not a rigid numerical definition of fun or a guarantee of commercial success.

<a id="v-22"></a>

### V-22

**Disposition:** Accepted with priority.

Fun first. Make a great game rather than optimizing its design for marketing or engagement metrics. Commercial validation may inform presentation but must not drive game design. Owner's expectation that quality will attract some players is an aspiration, not proof of market demand or a sales guarantee. Public demo, trailer, pricing and release strategy remain deferred.

<a id="v-23"></a>

### V-23

**Disposition:** Accepted.

Interpret contradictory feedback by audience, underlying experience and intended design. Use configuration selectively, preserve a coherent default and avoid reflexively implementing every preference.

<a id="v-24"></a>

### V-24

**Disposition:** Accepted.

Owner controls approved game-design decisions. Agents and testers may provide evidence and propose revisions; no silent product changes to simplify engineering.

## Validation and acceptance (proposed test focus)

Track comprehension, friction, fairness, self-directed goals, continued interest and observed enjoyment; report limitations and retest after revisions.

## Related design decisions

[AUD-16](steam-and-feedback.md#aud-16), [AUD-19](automated-balancing.md#aud-19), [V-08](steam-and-feedback.md#v-08), [V-09](steam-and-feedback.md#v-09), [V-10](steam-and-feedback.md#v-10)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
