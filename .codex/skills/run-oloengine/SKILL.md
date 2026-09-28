---
name: run-oloengine
description: Build, run, screenshot, or smoke-test OloEngine on Windows. Use for OloEditor, OloRuntime, OloServer, or the engine test executable; not for source-only changes.
---

# Run OloEngine

The project-local Claude skill remains the operational source of truth. Before
running or building anything, read `.claude/skills/run-oloengine/SKILL.md` from
the repository root and follow its applicable guidance.

Use its PowerShell scripts directly from the repository root. In particular,
every `cmake --build`, `ninja`, or `msbuild` invocation must go through
`\.claude/skills/run-oloengine/build-lock.ps1`; the Claude-only pre-tool hook
does not protect Codex sessions.

## Codex boundary

`driver.ps1 -Action attach` registers the live-editor MCP server with the
Claude CLI. It does not add tools to the active Codex task. Do not use that
action in Codex. The launch, capture, shot, stop, server, and test workflows
are agent-neutral. Configure an OloEditor MCP server in Codex separately before
attempting live-editor MCP inspection.

For controlled editor performance captures, use the `editor-mcp` host in
`scripts/perf/controlled-benchmark.py`. It launches a fresh Release editor with
a private discovery file and calls MCP directly through the local Python client,
without registering a connector. Follow `docs/guides/controlled-performance.md`
for the plan, preference checks, and idle-host requirements.
