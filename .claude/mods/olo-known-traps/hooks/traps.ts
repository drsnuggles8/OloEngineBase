// Error signatures of traps this repo has already paid for, each with the fix its memory
// note records. Adding one: a new entry here plus a test line in tests/traps.test.ts.
//
// Keep a signature specific: the advice is attached the moment the text appears, and a
// note that fires on ordinary output trains everyone to ignore it.

export type TrapContext = {
  tool: string
  /** The shell command, for Bash / PowerShell; empty otherwise. */
  command: string
  /** What the tool printed (or its error). */
  text: string
  isError: boolean
}

export type Trap = {
  id: string
  title: string
  /** The memory note that records it, by file name. */
  memory: string
  matches: (c: TrapContext) => boolean
  advice: (c: TrapContext) => string
}

const isShell = (c: TrapContext) => c.tool === 'Bash' || c.tool === 'PowerShell'
const heredocPython = (c: TrapContext) => /<<-?\s*['"]?\w+['"]?/.test(c.command) && /\bpython\d?(?:\.exe)?\b/.test(c.command)

export const TRAPS: readonly Trap[] = [
  {
    id: 'heredoc-backslash',
    title: 'backslashes lost in a heredoc Python edit',
    memory: 'bash-heredoc-python-backslashes-get-unescaped.md',
    matches: c =>
      isShell(c) &&
      heredocPython(c) &&
      /SyntaxWarning: invalid escape sequence|Unterminated string literal|unterminated string|missing terminating ['"] character|error C2001: newline in string literal/.test(c.text),
    advice: () =>
      'The Bash tool collapses backslash escapes in a heredoc before Python sees them (`\\n` and `\\\\n` both become a real newline). ' +
      'Look at the line that was actually written (`grep -n`/`cat -A`) before trusting the edit; build backslashes with `chr(92)`, or use the Edit tool.',
  },
  {
    id: 'newline-in-string-literal',
    title: 'a raw newline inside a string literal',
    memory: 'raw-newline-in-string-literal-is-clang-warning-msvc-error.md',
    matches: c => /error C2001: newline in string literal|warning: missing terminating ['"] character/.test(c.text),
    advice: () =>
      'A real line break landed inside a "..." literal: clang-cl only warns, MSVC (Windows CI) fails about an hour in. ' +
      'It is usually a heredoc edit writing `\\n` as a newline; fix the literal and scan the changed files for split literals.',
  },
  {
    id: 'edit-eperm-during-build',
    title: 'Edit/Write refused while a build reads the file',
    memory: 'edit-tool-blocked-while-build-reads-header.md',
    matches: c => (c.tool === 'Edit' || c.tool === 'Write') && /EPERM|atomic write failed|ftruncate/i.test(c.text),
    advice: () =>
      'A running build holds the header open, so the atomic rename-over is refused; the file is untouched (check `git diff --stat`). ' +
      'Wait for the build, or write in place from Python: `open(p, "r+b")`, `seek(0)`, `write`, `truncate()` (keep CRLF if the file has it).',
  },
  {
    id: 'mcp-port-excluded',
    title: 'the editor could not bind its MCP port',
    memory: 'mcp-port-in-hyperv-excluded-range.md',
    matches: c => /\[MCP\] Failed to bind 127\.0\.0\.1:\d+/.test(c.text),
    advice: c => {
      const port = /\[MCP\] Failed to bind 127\.0\.0\.1:(\d+)/.exec(c.text)?.[1] ?? '?'
      return (
        `Port ${port} is probably in a Hyper-V excluded range (34045–34944, 42271–42370, 49726–50464, …; \`netsh int ipv4 show excludedportrange protocol=tcp\`) ` +
        "or another session's editor holds it. Stop only YOUR editor and relaunch with `-McpPort` outside every range (7391 or 7397 worked). Never delete another port's discovery file."
      )
    },
  },
  {
    id: 'unmerged-index',
    title: 'conflicted paths in the index',
    memory: 'merge-state-can-be-lost-leaving-a-bare-uu-entry.md',
    matches: c => isShell(c) && /\bgit\b[^\n]*\bstatus\b/.test(c.command) && /^UU /m.test(c.text) && !/still merging|unmerged paths/i.test(c.text),
    advice: () =>
      'First find which operation left it: `git rev-parse -q --verify` MERGE_HEAD, CHERRY_PICK_HEAD and REVERT_HEAD, and `rebase-merge/` or `rebase-apply/` under ' +
      '`git rev-parse --absolute-git-dir`. A cherry-pick, revert or rebase in progress is not this trap: finish it with its own `--continue`. ' +
      'Only if you were MERGING and none of those exists has the merge marker been lost while stages 1/2/3 are intact, and committing would record no merge parent. ' +
      "Then confirm the merge target from `git ls-files -u` (stage 3 must match that branch's blob), write THAT commit back as MERGE_HEAD, and finish the merge. No reset needed.",
  },
  {
    id: 'worktree-add-powershell',
    title: '`git worktree add` failing from the PowerShell tool',
    memory: 'git-worktree-add-fails-from-powershell-tool.md',
    matches: c => c.tool === 'PowerShell' && /worktree\s+add/.test(c.command) && /fatal: invalid reference/.test(c.text),
    advice: () => 'The ref is fine: the PowerShell tool mangles the argument list. Run the same `git worktree add` from the Bash tool, and do not create the memory junction for a worktree that failed.',
  },
  {
    id: 'gh-pr-checks-json',
    title: '`gh pr checks` has no --json here',
    memory: 'gh-pr-checks-has-no-json-flag.md',
    matches: c => isShell(c) && /gh\s+pr\s+checks/.test(c.command) && /unknown flag: --json/.test(c.text),
    advice: () =>
      'It exits 0 with this error, so a poll loop on it waits forever. Use `gh pr view <n> --json statusCheckRollup --jq ...` or the check-runs API (`gh api repos/<o>/<r>/commits/<sha>/check-runs`).',
  },
  {
    id: 'no-jq',
    title: 'jq is not installed here',
    memory: 'no-jq-on-this-box.md',
    matches: c => /jq: command not found|'jq' is not recognized|jq: not found/.test(c.text),
    advice: () => "Use gh's own `--jq` flag or a `python -c` one-liner. A watch script that pipes into jq reports nothing, silently, inside a Monitor.",
  },
  {
    id: 'gh-projects-classic',
    title: 'the Projects (classic) GraphQL error',
    memory: 'gh-cli-gotchas.md',
    matches: c => /Projects \(classic\) is being deprecated/.test(c.text),
    advice: () =>
      'Read with explicit fields (`gh issue view <n> --json title,body ...`), which skips projectCards. For writes, `gh pr edit --add-label` and `--body-file` exit 0 WITHOUT applying: use the REST API.',
  },
  {
    id: 'gh-unknown-json-field',
    title: 'a JSON field this gh build does not have',
    memory: 'gh-cli-gotchas.md',
    matches: c => /Unknown JSON field: "(?:closingIssuesReferences|stateReason)"/.test(c.text),
    advice: () => 'This gh build lacks the field. Closing references: `gh api graphql` (pullRequest.closingIssuesReferences). Close reason: the REST issue object (`gh api repos/<o>/<r>/issues/<n>`, `state_reason`).',
  },
  {
    id: 'worktree-dir-busy',
    title: 'an emptied worktree directory still held',
    memory: 'worktree-remove-blocked-by-fsmonitor.md',
    matches: c => isShell(c) && /worktree\s+remove|rmdir|Remove-Item/.test(c.command) && /Permission denied|Device or resource busy/.test(c.text),
    advice: () =>
      'It is not the fsmonitor daemon. Run `.claude/scripts/free-locked-dir.ps1 -Path <dir>` (report first, then -Kill); if it finds no cwd holder, search `Win32_Process` CommandLines for the slug: leftover loops of the removed worktree\'s own session hold it.',
  },
  {
    id: 'editor-holds-link-output',
    title: 'a running binary blocks the link',
    memory: 'build-file-locks.md',
    matches: c => /lld-link: error: failed to write output '[^']*(?:OloEditor|OloEngine-Tests|OloRuntime|OloServer)\.exe': permission denied|LNK1104: cannot open file '[^']*\.exe'/i.test(c.text),
    advice: () =>
      "Every TU compiled; the link failed because this worktree's editor or test binary is still running (check the exe path before killing anything: sibling worktrees run the same names). " +
      'ninja stopped at that point, so other targets of the same build did not link either: rebuild after stopping it.',
  },
  {
    id: 'llvmgold-noise',
    title: 'the LLVMgold warning in a Linux configure',
    memory: 'llvmgold-warning-is-noise-read-past-it.md',
    matches: c => /LLVMgold\.so: cannot open shared object file/.test(c.text),
    advice: () => "Not the failure: it is check_ipo_supported's own probe, and configure continues (`IPO/LTO supported by this toolchain: NO`). Read past it for the real error.",
  },
  {
    id: 'far-near-macro',
    title: '`far` / `near` are macros under <windows.h>',
    memory: 'win32-far-near-macros-break-locals.md',
    matches: c => /expected unqualified-id/.test(c.text) && /\b(?:f32|f64|float|double|auto|const|int|u32)\s+(?:far|near)\b/.test(c.text),
    advice: () => '<windows.h> defines `far` and `near` to nothing; a local of that name breaks, and the second error lands at its next use. Rename it (`farField`, `nearClip`).',
  },
  {
    id: 'tmp-path-mismatch',
    title: "Git Bash's /tmp is not Python's /tmp",
    memory: 'git-bash-tmp-is-not-python-tmp.md',
    matches: c => isShell(c) && /No such file or directory: '\/tmp\/|FileNotFoundError: [^\n]*'\/tmp\//.test(c.text),
    advice: () => "In a Bash command /tmp is Git Bash's mount (%TEMP%); inside Python it is C:\\tmp. Use the session scratchpad, or `cygpath -w /tmp/x` to hand Python the real path. `set -e` did not stop after a failing heredoc either.",
  },
]

export function matchTraps(c: TrapContext): Trap[] {
  return TRAPS.filter(t => {
    try {
      return t.matches(c)
    } catch {
      return false
    }
  })
}

export function formatTraps(c: TrapContext, traps: readonly Trap[]): string {
  const lines = traps.map(t => `- ${t.title} (memory: ${t.memory}): ${t.advice(c)}`)
  return `olo-known-traps: this output matches a trap already recorded here:\n${lines.join('\n')}`
}
