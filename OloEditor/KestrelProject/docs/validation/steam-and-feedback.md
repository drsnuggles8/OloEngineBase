# Steam distribution, feedback and privacy

Status: consolidated v1.0 draft for repository adoption, 2026-11-27. Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

External playtesting uses Steam only; local owner testing is the sole non-Steam exception. Prepare repeatable packaging and deployment very early; do not wait for the vertical slice. Owner controls external build promotion. Actual Steam setup, IDs, credentials and CI access remain unverified.

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

## Validation and acceptance (proposed test focus)

Verify build installation/update, consent/disable behavior, attachment review, diagnostic reproducibility and sanitized findings. No public demo/launch strategy implied.

## Related design decisions

[AUD-17](../content/modding-and-sharing.md#aud-17), [V-04](playtesting.md#v-04), [V-05](playtesting.md#v-05), [V-11](playtesting.md#v-11), [V-12](playtesting.md#v-12)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
