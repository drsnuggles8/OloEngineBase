# Modding, content integrity and sharing

Status: consolidated v1.0 draft for repository adoption, 2026-11-27. Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Official and community content use a documented parseable versioned pipeline: stable identifiers, validation, dependencies and migration. Support maps, scenarios, equipment, models, buildings, configuration and shareable creations; gameplay extension API and licensing remain open. Data/asset modding should not require recompilation. Never silently delete incompatible content from a player’s resort.

Content integrity is distinct from anti-cheat. Permit customization and explain official scenario eligibility; do not obscure data just to discourage edits. Candidate implementation: build-generated manifest, deterministic semantic fingerprint of effective gameplay definitions, content provenance in saves/replays/reports and scenario-specific eligibility history. Hashing, signing, encoding and eligibility matrices are proposals. Client-side checks are not tamper-proof; no mandatory online enforcement or intrusive anti-cheat.

## Decision records

<a id="s-16"></a>

### S-16

**Disposition:** Accepted direction.

Given OloEngine's public development, make it easy for players to create their own missions, scenarios, maps, models and other content. Official content should use the same data-driven foundations. Editor, packaging, compatibility, distribution and scripting/security details remain open.

<a id="p-20"></a>

### P-20

**Disposition:** Strong acceptance.

Broad moddability of data, assets, models, maps, scenarios and eventual gameplay extensions; official content uses the same documented foundations where practical.

<a id="p-21"></a>

### P-21

**Disposition:** Strong acceptance.

Sharing maps, building templates, blueprints, screenshots and complete resort states. Terrain-dependent imports require validation.

<a id="aud-17"></a>

### AUD-17

**Disposition:** Accepted content direction; integrity mechanism proposed.

Official and community content use a documented, parseable, versioned data-driven pipeline with stable identifiers, validation and save migration. Owner asks how to prevent cheating by editing content files; reconcile with AUD-12 (no intrusive anti-cheat, transparent official-completion eligibility). Human-readable authoring is not to be sacrificed for obscurity. See proposed content-integrity design below; exact encoding, packaging, fingerprint format and achievement eligibility remain open. Native extension API, game/content licensing and distribution rights remain separate decisions.

## Validation and acceptance (proposed test focus)

Validate references, malformed packages, migrations, cosmetic versus gameplay changes, modified-then-restored saves and reproducibility.

## Related design decisions

[AUD-12](../gameplay/scenarios-and-events.md#aud-12), [AUD-13](../gameplay/construction.md#aud-13), [AUD-19](../validation/automated-balancing.md#aud-19), [V-09](../validation/steam-and-feedback.md#v-09)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
