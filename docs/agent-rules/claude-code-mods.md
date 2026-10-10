# A rule the agent keeps breaking belongs in a mod

**Rule.** When an agent-process rule has been broken more than once, hold it in a Claude Code mod
under `.claude/mods/`, where it acts at the moment the tool call happens. A memory note or a line
in `CLAUDE.md` works only when someone remembers to read it at the right time. A mod is a
TypeScript function-hook plugin. It can refuse or rewrite a tool call, annotate a result before the
model reads it, and keep a record across sessions. The settings hooks in `.claude/hooks/` can only
allow or block.

**What stayed green.** Each mod below answers a failure that every check passed at the time:

- a build reported exit code 0 while ninja had stopped;
- a `--gtest_filter` that matched nothing printed `PASSED`;
- a PR body claimed a Debug run that had not happened;
- a chained `--amend` rewrote a pushed commit;
- a `| Set-Content -NoNewline` flattened a shader into one line.

## The mods

| Mod | Holds |
|---|---|
| `olo-output-truth` | Reads Bash, PowerShell and Read output and the log behind a background task's "exit code 0" notification. It flags a failed build, a filter that selected no tests, a run that ended inside a test, an all-skipped run and failing tests (counted with gtest's two-space `[  FAILED  ]`). |
| `olo-command-guards` | Refuses a bare `git push`, a `--amend` after `;`/`\|\|`/newline and a pipe into `Set-Content -NoNewline`. Refuses CLAUDE.md's gated actions (push to master, force push, `reset --hard`, `--no-verify`, `gh pr merge`, `gh issue close`) unless the user approved them. Refuses a push or `gh pr create` while this worktree's build or tests still run, and flags a timing run started on a busy GPU. |
| `olo-read-gates` | Before an edit to a shader, a header declaring a `*Component`, or a new test `.cpp`, the matching guide must have been read in the session. Refuses edits to vendor, vcpkg and OloHeaderTool-generated files. Says when a new test source is missing from `OloEngine/tests/CMakeLists.txt` or lacks `OLO_TEST_LAYER`. |
| `olo-evidence` | Records every build, test run, evidence PNG read and live editor capture per branch, across sessions. `/evidence` shows the record. `gh pr create` is refused while the record does not back the PR: failing or unfinished runs (gtest or ctest), runs whose outcome is unknown, a Debug/Release claim with no passing run, or a renderer change with no frame looked at. A run counts only when the statement runs the build or test, and an earlier run keeps the outcome its log held before a later run overwrote that log. |
| `olo-known-traps` | Recognises the error text of traps already recorded in memory notes and attaches the recorded fix, once per 20 minutes per trap. The table is `hooks/traps.ts`. |
| `olo-context-saver` | Collapses ninja progress lines and passing gtest RUN/OK pairs in large logs before they are stored in the conversation; every other line stays verbatim. |
| `olo-fleet` | `/fleet`: every worktree with its changes, PR, CI, open review threads, and who holds and waits for the build lock. A notification when a PR's CI settles. |

## Markers

Text in the command is the only channel a guard has, so two literal markers exist. Each is meant
to be used only when it is true.

**A marker counts only in the trailing `#` comment of the statement it approves.** Inside a quoted
argument it is data: a PR body or an issue comment that mentions it approves nothing. On a chain it
approves only the statement it ends. For example, `gh issue close 1 && git reset --hard # MARKER`
approves the reset and still refuses the close.

- `OLO_USER_APPROVED`: the user approved this specific gated action in this conversation. It lets
  `olo-command-guards` and `olo-evidence` through, and is logged in the mod's store. A past approval
  does not carry forward.
- `OLO_LEDGER_INCOMPLETE`: the runs a PR relies on happened outside the record (an earlier session,
  another machine). It waives only "no runs with a known outcome", "unproven run" and "unbacked
  claim"; say so in the PR body.

Two things in the PR body clear a check without a marker:

- A failing test named in the body (as pre-existing, with evidence) clears that failure from
  `olo-evidence`'s check.
- A body that says "could not inspect a frame" clears the visual rule, as CLAUDE.md asks.

## Installing them

The folder is a plugin marketplace that is read in place, so an edit or a pull reaches sessions on
`/reload-plugins` with no reinstall. Once per machine, from the root of the base checkout (the main
one, not a worktree: worktrees come and go):

```text
claude plugin marketplace add ./.claude/mods
```

Then, in a terminal Claude Code session, `/plugin install olo-output-truth@olo-mods`, and the same
for each other mod, at the user scope.

## What the VS Code extension shows

Measured on 2026-10-10 with extension 2.1.295:

- **Panes and status lines are not drawn.** `$.ui.open` resolved `isPlaced: true`, yet no pane
  appeared, and `$.ui.status` reached only the debug log. A command must therefore answer in its
  own transcript row. `/fleet` and `/evidence` return markdown, and `olo-fleet` also draws the
  `CommandOutput` row as a tree.
- **Context notes, denials and native notifications do arrive.**

## Writing or changing one

- **Develop it in the session's mods folder**, which the `plugin-authoring` skill names and
  hot-reloads, then copy it here. Never commit the engine-written `.claude-plugin/types/` or
  `tsconfig.json` (`.gitignore` excludes them).
- **`$` can only be passed to a function declared at the top level of the same file.** A closure
  that receives `$` makes the module fail to load. Keep shared state at module level, and accept
  that a reload resets it.
- **Keep the logic in a pure file with its own tests.** `register.ts` only wires events. In
  `claude plugin test`:
  - The kit has no clock, store, file system or process access. Use `mock.clock(on)`.
  - Answer `store.get`/`store.set`/`fs.*`/`process.run` yourself with `{ value }` objects.
  - Mount every component on `terminal` and `vscode` alike.
- **Write mod files with the Write and Edit tools, never a Bash heredoc.** The heredoc collapsed
  `\\` in three test files while these were written; see the memory note on heredoc backslashes.
- **Check before committing:**
  - `claude plugin validate .claude/mods` (the marketplace and every mod)
  - `claude plugin test .claude/mods/<mod>`
  - `tsc` with the tsconfig the engine lays beside the mod. It must be TypeScript 5.4 or newer.
  - Use the `claude` binary that matches the running engine. The one on PATH may lag the VS Code
    extension's own.
- **A guard fails one way, chosen deliberately.**
  - `olo-command-guards` falls back to its pure text rules when its process queries fail.
  - `olo-read-gates` and `olo-evidence` fail open, because a broken reminder must not block every
    edit.
- **Adding a known trap:** add an entry to `TRAPS` in `olo-known-traps/hooks/traps.ts` with its
  memory note, and a positive case in its test. The test fails if a trap has none. Keep the
  signature to text that only that failure prints.
