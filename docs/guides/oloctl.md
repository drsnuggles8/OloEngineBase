# `oloctl` — the automation registry from a shell

`oloctl` is a command-line frontend over a **running OloEditor**. It asks the editor what
commands it has, builds its own command tree from the answer, and runs one. It carries **no
list of commands**: adding a command to the automation registry adds it to `oloctl`, with no
change to `oloctl` and no rebuild of it. (Issue #1125, Epic C of the automation control plane.)

Read-only today. Writes are a later pass and go through the same consent gate an MCP call
goes through — see [Writes are closed](#writes-are-closed).

## Quick start

```bash
# An editor must be running with its MCP diagnostics server started.
oloctl help                          # usage + the command groups this editor has
oloctl scene                         # the commands in the `scene` group
oloctl scene list-entities --help    # arguments, from the command's own schema
oloctl scene list-entities | jq '.structuredContent.entities'
oloctl catalogue | jq '.commands[].name'
```

`oloctl` is built by the `oloctl` CMake target and lands in `bin/<Config>/oloctl/oloctl.exe`.
It links neither OloEngine nor OloEditor: its whole dependency set is nlohmann/json and
cpp-httplib, so it builds in seconds and starts instantly.

## How a command is spelled

The registry names a command `olo_scene_list_entities` and files it under the toolset
`scene`. `oloctl` derives `oloctl scene list-entities` from those two facts, mechanically:

| | rule |
|---|---|
| **group** | the toolset, lowercased, `_` and `.` as `-`; failing that the name's first segment; failing that `misc` |
| **command** | the name minus a leading `olo`, minus the group's segments when the name repeats them, joined with `-` |

`oloctl call <registry-name>` addresses a command by the name the registry knows it by and
always works — including for a command whose derived spelling is claimed by two commands, in
which case `oloctl` reports the ambiguity instead of picking one.

`oloctl catalogue` prints the whole mapping as JSON, so a script never has to guess it.

## Arguments

Options come from the command's declared `inputSchema`, and values are typed by it:

```bash
oloctl diagnostics log-tail --count 50            # integer, because the schema says so
oloctl camera set-pose --position '[0,2,-8]'      # a JSON array for an array property
oloctl render capture-target --target GBufferA --max-width 512
oloctl scene list-entities --arguments-json '{"limit":10}'
```

- A property `sinceId` is spelled `--since-id`; the verbatim `--sinceId` also works.
- A boolean property takes `--flag`, `--no-flag`, or `--flag true|false`.
- Repeat an option to build an array; a JSON array literal works too.
- A property whose schema declares no type takes JSON when the text parses as JSON, and the
  raw string otherwise.
- `--arguments-json` seeds the whole object; named options override its keys.
- An argument whose flag spelling is one of `oloctl`'s own options (`--port`, `--url`,
  `--token`, `--timeout`, `--json`, `--structured`, `--compact`, `--verbose`, `--help`,
  `--version`, `--arguments-json`) can only be given through `--arguments-json`. `oloctl`
  says so, and **refuses the call** rather than running it with the argument quietly
  missing. No command collides today.

**`oloctl` validates nothing beyond the type.** `required`, `enum`, ranges and
`additionalProperties` are enforced by the editor, through the same validator an MCP
`tools/call` goes through, so the two frontends cannot disagree about what a command accepts.
The only local checks are the ones the editor could never make, because the request would not
have been built: an unknown option, a missing value, a value that is not of the declared type,
and a non-finite number.

## Output

**The payload goes to stdout and nothing else ever does.** Warnings, the connection report and
every error go to stderr, so `oloctl … | jq` keeps working on the run that had something to
warn about.

By default stdout carries the editor's `tools/call` result object — `content`, `isError`, and
`structuredContent` when the command returned a typed payload — indented. `--compact` puts it
on one line; `--structured` prints just `structuredContent`.

| exit | meaning |
|---|---|
| 0 | the command ran |
| 1 | the command ran and reported an error (`isError`) |
| 2 | usage: unknown group/command/option, ambiguous spelling, bad value |
| 3 | the editor could not be reached, or stopped answering mid-call |
| 4 | refused: the command mutates the project (see below) |
| 5 | `--structured`, and the command SUCCEEDED but returned no typed payload |

Exit 5 means only that: a command that *failed* has its message in `content` and no
`structuredContent`, so under `--structured` it exits 1 with the message on stderr rather
than reporting a missing payload.

`oloctl` resolves nothing against the working directory, so it can be run from anywhere. A
**path in an argument is resolved by the editor**, relative to the editor's own working
directory (`OloEditor/`) — the same as for an MCP call.

## Finding the editor

The editor writes a discovery file (host, port, token, URL) when its MCP server starts, and
`oloctl` reads it. Precedence, in order:

1. `--url` + `--token` — bypasses discovery entirely; both are in the editor's MCP panel.
2. `--discovery-file <path>`
3. `--port <n>` — the per-worktree port the `run-oloengine` skill derives.
4. `OLO_MCP_DISCOVERY_FILE`
5. the system temp directory, searched.

**Everything you type beats the environment**, and that ordering is load-bearing: the
`run-oloengine` driver exports `OLO_MCP_DISCOVERY_FILE` in the shell it attaches from, so a
worktree session inherits it. If the variable came first, `oloctl --port <other editor>` would
quietly attach to *this* worktree's editor and answer about the wrong scene with exit 0.

The search in (5) never picks between candidates:

- **one** `oloengine-mcp*.json` → it attaches to that editor;
- **several** → it lists them all and stops. Attaching to the wrong editor answers every
  question about the wrong scene, and the answer looks correct.
- **none** → it says so and how to fix it.

`--verbose` reports which editor was attached to, and via which route, on stderr.

## Why the whole registry, not the listed profile

`tools/list` shows a curated core set by default (#1124): the full catalogue is ~265 KB of
schemas, and a third of an agent's context window is a real cost. A CLI has no context window,
so `oloctl` takes the **whole registry** — via `olo_tool_search`, which reports every
registered command regardless of the profile. Measured against a stock editor: `oloctl` sees
99 commands where `tools/list` advertises 18. A CLI that silently offered 18 of 99 would look
exactly like a working one.

## Writes are closed

The registry's authority class (`projectWrite`) travels in the catalogue, and `oloctl` refuses
a project-mutating command **before it builds a request** — nothing about it reaches the
editor. Three things hold, and they are independent:

1. `oloctl` refuses at resolution time (exit 4).
2. Nothing in `oloctl` can assert write consent. `ICommandSource` has no parameter for it.
3. The editor's session write consent is off by default, so an MCP `tools/call` for such a
   command is refused at the far end too.

A command whose authority class the catalogue does **not** report is refused as well, rather
than assumed safe — that is an editor older than this field, and guessing would be a way
around the consent model rather than a convenience.

When writes land, they go through the editor's consent gate, the same one an MCP call goes
through. There will not be a CLI-specific bypass.

## What it does not do

- **It does not start an editor.** Driving a headless host is its own epic (#1132).
- **It does not stream progress.** A long-running command blocks until it answers; the
  editor's progress notifications need the SSE transport, which this client does not speak.
- **It does not replace `.claude/skills/run-oloengine/driver.ps1`.** The driver launches the
  editor, waits for its window, screenshots it through Win32, and registers the MCP server
  with Claude Code. `oloctl` subsumes only the part after that — *talking* to a running
  editor from a shell, which the driver never did.

## See also

- [mcp-diagnostics-server.md](mcp-diagnostics-server.md) — the endpoint `oloctl` attaches to,
  the write-consent model, and the exposure profiles.
- `OloEditor/src/Automation/AutomationRegistry.h` — the registry both frontends adapt.
- `OloCtl/CMakeLists.txt` — why the target links nothing from the engine.
