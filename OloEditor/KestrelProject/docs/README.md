# KESTREL — Game design documentation

Consolidated v1.0 draft • 2026-11-27 • adopted into the repository at `OloEditor/KestrelProject/docs/` on 2026-11-27. Synthesized from owner-reviewed register v0.15 and consistency audit v0.4. Adoption is not evidence of implementation or human playtesting, and it does not change the existing milestone.

## Start here

- [Vision](VISION.md) — product identity, core loop and milestone boundary.
- [Constitution](CONSTITUTION.md) — approved overarching principles.
- [Decision index](DECISIONS.md) — canonical location and status for every recorded decision.
- [Open questions](OPEN_QUESTIONS.md) — remaining experiments, tunables and decisions.
- [Agent guide](AGENT_GUIDE.md) — when these documents apply to a task, what to read, and what gameplay plans, PRs and reviews must contain.

## Authority and precedence

The latest owner-approved dispositions supersede older conflicting proposals. The topic documents contain current contracts and individually identified decisions; an accepted direction does not approve numerical examples, implementation options or unresolved parts. `OPEN_QUESTIONS.md` and sections explicitly labeled proposed are not requirements. The archive is historical evidence only. Existing repository instructions and north-star milestone remain in force; resolve conflicts with the owner, not by silently changing either. In the repository those are [CLAUDE.md](../../../CLAUDE.md), the [north star](../../../docs/process/north-star.md) (the vertical slice's scope and date) and [ADR 0027](../../../docs/adr/0027-kestrel-lives-in-the-engine-repo-as-a-second-project.md) (the engine/game boundary).

## Topic specifications

- [Construction and terrain editing](gameplay/construction.md)
- [Time, calendar and historical operation](gameplay/time-and-history.md)
- [Guests, transport and piste networks](gameplay/guests-and-networks.md)
- [Economy, finance and recovery](gameplay/economy.md)
- [Snow, mountain operations and weather](gameplay/snow-and-weather.md)
- [Geography and land development](gameplay/geography.md)
- [Empire, ownership and acquisitions](gameplay/empire-and-acquisitions.md)
- [Fair competition and AI operators](gameplay/competition.md)
- [Scenarios and sporting events](gameplay/scenarios-and-events.md)
- [Progression, research and personal mastery](gameplay/progression-and-research.md)
- [User experience and interaction](experience/UX.md)
- [Advisors, automation and onboarding](experience/advisors-and-onboarding.md)
- [Art direction, animation and audio](experience/art-direction.md)
- [Modding, content integrity and sharing](content/modding-and-sharing.md)
- [Regional and historical data](content/regional-and-historical-data.md)
- [Human playtesting and findings](validation/playtesting.md)
- [Automated balancing, replay and acceptance](validation/automated-balancing.md)
- [Steam distribution, feedback and privacy](validation/steam-and-feedback.md)
- [Integrated checkpoints and roadmap reconciliation](validation/production-checkpoints.md)
- [Scenario catalogue](content/scenario-catalogue.md) — candidate scenarios.
- [Gameplay feature acceptance template](validation/acceptance-template.md) — the design-acceptance block for gameplay plans and PRs.

## Archive

[Workshop and audit history](archive/README.md). Prior versions are unmodified.

## Integration status

The v1.0 draft package was prepared without repository or Steam writes. On 2026-11-27 it was adopted here unchanged except for mechanical integration: status lines, navigation (this file, the archive index), links to the repository's own documents, and the [agent guide](AGENT_GUIDE.md) and [acceptance template](validation/acceptance-template.md), which were reconciled with the repository's agent workflow. Every archived snapshot is byte-identical to the package.

The repository's agent instructions now route KESTREL gameplay work here: `CLAUDE.md` names this directory, `.claude/rules/kestrel-design.md` loads the agent guide when a file under `OloEditor/KestrelProject/` is touched, `/start-work` §5a puts a *Design context* section in a gameplay task's `HANDOVER.md`, and the task loop requires a *Design validation* section in its PR. Codex reaches the same files through `AGENTS.md`. Engine-only work does not load this corpus.

Not done by adoption: no code, asset, issue, milestone or Steam setting changed, no decision was revised and no open question was resolved. The requirements-to-code-and-backlog audit ([AUD-18](validation/production-checkpoints.md#aud-18), EXP-17) is still outstanding; until it lands, existing issues may not reflect this design.
