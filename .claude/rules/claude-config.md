---
paths:
  - ".claude/**"
  - "docs/process/**"
---

# Claude Code configuration in this repo

- `.claude/settings.json` holds the shared hooks (build lock, push-format guard, issue-create guard,
  the `Stop` pre-commit run). Edits to it need the user's explicit go-ahead.
- The `olo-*` mods in `.claude/mods/` are not enabled by any file in the repo: each machine installs
  them once ([claude-code-mods.md](../../docs/agent-rules/claude-code-mods.md#installing-them)).
  They run in the main session and inside subagents.
- Skills in `.claude/skills/` with side effects carry `disable-model-invocation: true`; keep each
  `SKILL.md` under 500 lines and move reference material and scripts into the skill's directory.
- Subagents in `.claude/agents/` do not get auto memory: name the memory notes they need by path.
  Keep their `description` short; detail goes in the body, which loads only when they run.
- Deterministic steps belong in scripts (`.claude/scripts/`, a skill's `scripts/`), not in prose
  the model retypes each run.
