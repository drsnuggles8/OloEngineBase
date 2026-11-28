# Steam distribution, feedback and privacy

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

External playtesting uses Steam only; local owner testing is the sole non-Steam exception. Prepare repeatable packaging and deployment very early; do not wait for the vertical slice. Owner controls external build promotion. A separate Steam Playtest app is approved ([AUD-26](#aud-26)); the Steamworks partner account exists but its onboarding is unfinished, and app IDs, credentials and CI access remain unverified.

Early in-game positive/negative feedback, optional reviewed reproducible attachments and opt-in meaningful telemetry are high priority. Version events and content/build context. Keep raw reports private, minimize data, explain purpose, separate attachment consent, restrict access, define retention and privacy review. Publish sanitized findings only. Keep infrastructure lightweight; storage choices are proposals, not requirements.

## Decision records

<a id="v-08"></a>

### V-08

**Disposition:** Accepted, high priority.

In-game feedback reporter, positive and negative experiences, optional reviewed diagnostic attachments and reproducible state. Integrate early enough to support the first external tests.

<a id="v-09"></a>

### V-09

**Disposition:** Accepted, high priority.

Opt-in, schema-versioned gameplay telemetry centered on meaningful gameplay actions, interpreted alongside qualitative reports. Do not equate engagement metrics with fun.

<a id="v-10"></a>

### V-10

**Disposition:** Accepted.

Explicit informed consent, purpose limitation, data minimization, restricted access, retention rules and private raw feedback/attachments. Avoid automatically publishing tester data in the public repository. Separate consent for optional sensitive attachments/recordings; review privacy requirements before collection.

<a id="v-21"></a>

### V-21

**Disposition:** Accepted with explicit distribution decision.

Steam-first external playtesting as soon as practical. The only non-Steam playtesting is on the owner's own PC. No manual ZIP/executable distribution or manual upload workflow to individual testers. Establish a convenient repeatable build/publish workflow; exact Steam setup, access, automation and test branch arrangements remain implementation questions. Steam Playtest is the preferred candidate, not a confirmed configured service.

<a id="aud-16"></a>

### AUD-16

**Disposition:** Accepted; very high early priority.

Automated Steam-first external test distribution, owner-controlled release promotion, lightweight private feedback and consent-based diagnostics/telemetry. Anything that materially reduces friction in playtesting and feedback is high priority. Start packaging/distribution preparation early, not after the vertical slice; external human testing begins when the core loop is established and distribution works. Local owner testing remains the only non-Steam playtesting. Actual Steam access, app IDs, credentials, CI/deployment setup, data hosting and privacy review still need verification. No paid tester budget.

<a id="aud-26"></a>

### AUD-26

**Disposition:** Accepted (owner decision, 2026-11-28).

A Steamworks partner account exists; bank and tax onboarding is unfinished and is never recorded in an issue, the repository, chat or logs. App, depot and Playtest IDs are unconfirmed. A **separate Steam Playtest app** is approved. Automated candidate packaging and protected publishing are approved in principle; the first live upload may use the existing local upload script with the owner's authorisation, before protected automation from a trusted environment. Steam credentials are never readable by ordinary PR execution, including self-hosted runner jobs. Promotion to external testers stays under owner control. Private feedback and telemetry ingestion may be hosted on owner-run infrastructure only after an explicit assessment of isolation, deployment and operations; the host is not assumed suitable and is not exposed without authorisation, and a hosted alternative is acceptable. The CP3 First Fun Prototype is the gate for the first external Steam playtest; CP1 and CP2 are tested only by the owner on the owner's PC; Steam and feedback work starts early and in parallel. The external test needs approved Steam deployment, functional feedback submission and a safe, approved collection setup, not a complete analytics platform. Refines [V-21](#v-21) and [AUD-16](#aud-16). Provenance: [owner decisions of 2026-11-28 on #1727](https://github.com/drsnuggles8/OloEngineBase/issues/1727#issuecomment-7241796309), Q4, Q5 and Q7.

## Validation and acceptance (proposed test focus)

Verify build installation/update, consent/disable behavior, attachment review, diagnostic reproducibility and sanitized findings. No public demo/launch strategy implied.

## Related design decisions

[AUD-17](../content/modding-and-sharing.md#aud-17), [V-04](playtesting.md#v-04), [V-05](playtesting.md#v-05), [V-11](playtesting.md#v-11), [V-12](playtesting.md#v-12)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
