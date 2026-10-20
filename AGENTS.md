# AGENTS.md

The rules for working in this repository are in [CLAUDE.md](CLAUDE.md). Read it first. Do not copy
it into this file: a copy drifts within days.

Two parts of the setup are specific to Claude Code. If you are another agent, replace them yourself:

- **Path-scoped rules.** Claude Code loads each file in [.claude/rules/](.claude/rules/) when it
  touches a file matching that rule's `paths:` frontmatter. Read every rule whose globs match the
  files you are about to change before you change them.
- **Hooks and mods.** Claude Code runs some rules for you:
  - run `pre-commit run --all-files` at the end of a change and before every push;
  - route every build through `.claude/skills/run-oloengine/build-lock.ps1`;
  - give every issue filed from a task worktree the `Not fixed in … because:` line and `olo-score`
    block from [task-loop Phase 1a](docs/process/task-loop.md#1a-bugs-you-find-on-the-way--fix-them-here);
  - never run a gated action from `CLAUDE.md` → *Committing and publishing* without the user's
    explicit approval for that action (the `olo-command-guards` mod refuses them in Claude Code).

The `/start-work`, `/cleanup-worktree` and `/resume-worktrees` skills live in `.claude/skills/`;
Codex wrappers for them are in `.codex/skills/`. Everything else applies as written.
