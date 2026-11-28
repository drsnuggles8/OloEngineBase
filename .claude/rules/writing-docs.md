---
paths:
  - "docs/**/*.md"
  - "CLAUDE.md"
  - "AGENTS.md"
  - ".claude/rules/**"
  - ".claude/skills/**/*.md"
  - ".claude/agents/**"
---

# Writing docs and agent instructions here

Every PR may add a postmortem, so style and size matter more than completeness:

- **Rule first, story second.** State what to do in the first sentence; the failure that taught it
  follows. An index entry is one plain sentence saying the rule, not a teaser.
- **Say what you mean.** No metaphor where a literal phrase exists.
- **Keep a guide under about 10 KB.** `<name>.md` lists the rules, each linking to its section in
  the `<name>-details.md` companion, which holds the stories, measurements and derivations. Add a
  new rule to both; add a story only to the details file.
- **Never add a lesson to `CLAUDE.md`.** It loads into every session. Add the guide, then one line
  in [docs/agent-rules/README.md](../../docs/agent-rules/README.md) under its subsystem and, for a
  postmortem, one row in [failure-modes.md](../../docs/agent-rules/failure-modes.md). A rule that
  should load when certain files are touched gets a line in the matching `.claude/rules/*.md`.
- `CLAUDE.md` stays under 200 lines; path-specific instructions belong in `.claude/rules/`.
- KESTREL's game design is not engine documentation: it lives in `OloEditor/KestrelProject/docs/` under
  its own `README.md`, never in `docs/`.
- `pre-commit`'s `docs-consistency` hook fails on a dead link or anchor and on a doc unreachable
  from `docs/README.md` (a KESTREL design doc: from its own `README.md`), and on a KESTREL decision
  without exactly one record and one `DECISIONS.md` row.
- To audit instruction files for stale or contradictory content, run `/doctor prompt-audit`.
