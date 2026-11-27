# Gameplay feature acceptance template

Status: adopted 2026-11-27 as the design-acceptance block for KESTREL gameplay work. A gameplay task's `HANDOVER.md` carries it as *Design context* (written by `/start-work` §5a) and its PR body as *Design validation* (task loop Phase 4); the [agent guide](../AGENT_GUIDE.md#the-design-acceptance-block) says when a task counts as gameplay work. It does not change the repository's GitHub issue templates.

- Design references: stable decision IDs and relevant specifications; check latest dispositions.
- Player experience and hypothesis: what action/decision should be enjoyable, understandable and meaningful?
- Scope/checkpoint: current playable capability; distinguish future full-game goals and existing milestone.
- Interactions/invariants: authoritative state, commands, cost/rights, time, RNG, ownership, saves, mod versions, accessibility.
- Technical acceptance: functional/regression/invariant tests, performance on representative hardware, save/load, headless equivalence where relevant.
- Player-facing acceptance: responsive intuitive controls, causal explanation, visuals/audio, localization/readability, encyclopedia accuracy.
- Validation plan: lab/fixture, seeds/build, human playtest question, telemetry/feedback with consent, retest after change.
- Evidence: actual results and limitations; never claim human testing from an agent run.
- Status: technical (proposed/implemented/verified); design (not ready/playtest-ready/human-tested/design-validated/revision required).
- Outstanding risks, assumptions and owner approval needed for changed design.

This template is the integration mechanism for V-01, V-02, V-16, V-17, AUD-18 and AUD-19. It was proposed in the v1.0 draft package and adopted with the agent workflow; the fields above are unchanged from that proposal.
