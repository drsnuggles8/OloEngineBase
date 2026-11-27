# KESTREL agent reading guide (integration proposal)

This is a project-local guide, not a change to root AGENTS.md or CLAUDE.md. On KESTREL work, follow existing repository instructions and architecture; read README, CONSTITUTION, VISION and only the relevant topic specifications, plus cross-system contracts. Engine-only work should not load the entire game design.

- Reference stable decision IDs in plans, issue acceptance, implementation and review. Use DECISIONS.md to locate the current record; do not treat archive proposals as current requirements.
- Before implementation check existing code, issue owners and dependencies. Do not duplicate existing systems, rewrite issues or silently change the vertical slice.
- Treat approved design, preferred implementation, hypotheses/tunables and rejected alternatives differently. Ask owner when a product choice cannot be resolved from the specification; present evidence and options rather than silently changing it.
- Use authoritative gameplay commands and state; preserve camera/speed invariance, accounting, identity/ownership, navigation, deterministic replay and content provenance where applicable.
- Include technical and player-facing acceptance and a validation plan; report actual checks and remaining human evidence separately. Never label agent play as human testing.
- Keep raw tester data and credentials out of public issues, repository and logs; only sanitized evidence is shared.

See [acceptance template](validation/acceptance-template.md). A later repository integration task should reconcile this guide with the actual root agent instructions and start-work skills. No edits to them are included here.
