import { expect, test } from 'claude-code/testing'

import {
  checkCommand,
  commentOf,
  contenders,
  isTimingRun,
  loadNote,
  localChecksRunning,
  parseGpuSample,
  parseProcessRows,
  publishes,
  publishingApproved,
  refusals,
  splitStatements,
  tokenize,
} from '../hooks/guards'

const rules = (command: string, branch?: string) => checkCommand(command, branch).map(v => v.rule)

test('the statement splitter respects quotes and records separators', () => {
  const s = splitStatements('git commit -m "a; b && c" && git push -u origin feature/x; echo done')
  expect(s.map(x => x.separator)).toEqual(['start', '&&', ';'])
  expect(s[0]?.words).toEqual(['git', 'commit', '-m', 'a; b && c'])
  expect(tokenize("echo 'x y' # trailing")).toEqual(['echo', 'x y'])
})

test('commands this repo runs every day pass untouched', () => {
  const ordinary = [
    'git -C /c/repos/OloEngineBaseBase worktree remove /c/repos/OloEngine-foo',
    'git push -u origin feature/integrated-aaa-acceptance-scene-1259',
    'git -C "C:\\repos\\x" worktree add "C:\\repos\\OloEngine-y" -b feature/y --no-track origin/master',
    'git log --oneline origin/master..feature/x',
    'git commit --amend --no-edit',
    'git add -u && git commit --amend --no-edit',
    'gh issue edit 1359 --body-file body.md',
    'gh pr create --title t --body-file b.md',
    'cmake --build build-cached --target OloEngine-Tests 2>&1 | tail -5',
    'git show abc:path/File.glsl > path/File.glsl',
    'Get-Content x.txt | Select-String foo',
    'echo "git push" && grep -c "git reset --hard" notes.md',
  ]
  for (const command of ordinary) {
    expect({ command, rules: rules(command, 'feature/x') }).toEqual({ command, rules: [] })
  }
})

test('a chained --amend after ; || or a newline is refused, && is fine', () => {
  expect(rules('git revert -q abc ; git commit --amend -m x')).toEqual(['chained-amend'])
  expect(rules('git revert abc || git commit --amend')).toEqual(['chained-amend'])
  expect(rules('git revert abc\ngit commit --amend')).toEqual(['chained-amend'])
  expect(rules('git revert --no-edit abc && git commit --amend -m x')).toEqual([])
})

test('bare pushes are refused with the branch named; whole-repo forms are not bare', () => {
  const [bare] = checkCommand('git push', 'feature/foo')
  expect(bare?.rule).toBe('bare-push')
  expect(bare?.reason).toContain('git push -u origin feature/foo')
  expect(rules('git push origin')).toEqual(['bare-push'])
  expect(rules('git push -u')).toEqual(['bare-push'])
  expect(rules('git push origin --tags')).toEqual([])
  expect(rules('git push origin --delete feature/old')).toEqual([])
})

test('pushes to master and force pushes are gated', () => {
  expect(rules('git push origin master')).toEqual(['push-to-master'])
  expect(rules('git push origin HEAD:master')).toEqual(['push-to-master'])
  expect(rules('git push origin HEAD:refs/heads/main')).toEqual(['push-to-master'])
  expect(rules('git push origin HEAD', 'master')).toEqual(['push-to-master'])
  expect(rules('git push --force-with-lease origin feature/x')).toEqual(['force-push'])
  expect(rules('git push origin +feature/x')).toEqual(['force-push'])
  expect(checkCommand('git push -f origin master').every(v => v.isGated)).toBe(true)
})

test('other gated actions: reset --hard, --no-verify, gh pr merge, gh issue close', () => {
  expect(rules('git reset --hard origin/master')).toEqual(['reset-hard'])
  expect(rules('git commit --no-verify -m x')).toEqual(['no-verify'])
  expect(rules('gh pr merge 1577 --squash')).toEqual(['pr-merge'])
  expect(rules('gh issue close 1528 --reason completed')).toEqual(['issue-close'])
})

test('the approval marker lets gated actions through, never ungated ones', () => {
  expect(refusals('gh issue close 1528 # OLO_USER_APPROVED')).toEqual([])
  expect(refusals('git push origin master # OLO_USER_APPROVED').map(v => v.rule)).toEqual([])
  expect(refusals('git push # OLO_USER_APPROVED').map(v => v.rule)).toEqual(['bare-push'])
  expect(refusals('git revert x ; git commit --amend # OLO_USER_APPROVED').map(v => v.rule)).toEqual(['chained-amend'])
})

test('prefixes and shell wrappers do not hide a command from the rules', () => {
  expect(rules('GH_TOKEN=x gh pr merge 1585')).toEqual(['pr-merge'])
  expect(rules('env gh issue close 1')).toEqual(['issue-close'])
  expect(rules('timeout 60 git push origin master')).toEqual(['push-to-master'])
  expect(rules('pwsh -NoProfile -Command "git push origin master"')).toEqual(['push-to-master'])
  expect(rules("bash -c 'git revert x ; git commit --amend'")).toEqual(['chained-amend'])
  expect(rules('cmd /c git reset --hard')).toEqual(['reset-hard'])
  // The wrapper's own trailing comment approves what it runs; a quoted mention does not.
  expect(refusals('pwsh -Command "gh pr merge 1" # OLO_USER_APPROVED')).toEqual([])
  expect(refusals('pwsh -Command "gh pr merge 1 --body OLO_USER_APPROVED"').map(v => v.rule)).toEqual(['pr-merge'])
  expect(publishes('GH_TOKEN=x gh pr create --fill')).toBe(true)
  expect(publishes('pwsh -Command "git push -u origin feature/x"')).toBe(true)
  expect(publishingApproved('pwsh -Command "git push -u origin feature/x" # OLO_USER_APPROVED')).toBe(true)
  // Ordinary wrapped commands stay quiet.
  expect(rules('pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command "cmake --build build-cached"')).toEqual([])
})

test('the marker counts only in its own statement\'s trailing comment, never inside quotes', () => {
  expect(refusals('gh issue close 1 --comment "OLO_USER_APPROVED"').map(v => v.rule)).toEqual(['issue-close'])
  expect(refusals("gh issue close 1 --comment 'see # OLO_USER_APPROVED'").map(v => v.rule)).toEqual(['issue-close'])
  expect(refusals('git push origin master -o "OLO_USER_APPROVED"').map(v => v.rule)).toEqual(['push-to-master'])
  // One marker on a chain approves the statement it ends, not the ones before it.
  expect(refusals('gh issue close 1 && git reset --hard # OLO_USER_APPROVED').map(v => v.rule)).toEqual(['issue-close'])
  expect(commentOf('gh issue close 1 --comment "a # b" # OLO_USER_APPROVED')).toBe(' OLO_USER_APPROVED')
  expect(publishingApproved('gh pr create --body "OLO_USER_APPROVED"')).toBe(false)
  expect(publishingApproved('git push -u origin feature/x # OLO_USER_APPROVED')).toBe(true)
})

test('a pipe into Set-Content -NoNewline is refused; a plain Set-Content is not', () => {
  expect(rules('git show abc:a.glsl | Set-Content -NoNewline a.glsl')).toEqual(['set-content-nonewline'])
  expect(rules('Get-Content a | set-content -nonewline b')).toEqual(['set-content-nonewline'])
  expect(rules("Set-Content -Path a.txt -Value 'x' -NoNewline")).toEqual([])
  expect(rules('Get-Content a | Set-Content b')).toEqual([])
})

test('publishing commands are recognised; dry runs are not', () => {
  expect(publishes('git push -u origin feature/x')).toBe(true)
  expect(publishes('gh pr create --fill')).toBe(true)
  expect(publishes('git push --dry-run origin feature/x')).toBe(false)
  expect(publishes('gh pr view 12')).toBe(false)
})

test('timing runs are recognised by flag or filter', () => {
  expect(isTimingRun('build\\OloEngine\\tests\\Debug\\OloEngine-Tests.exe --gtest_filter=FoliageCostBaselineTest.*')).toBe(true)
  expect(isTimingRun('OloEngine-Tests.exe --olo-capture-manifest=m.json')).toBe(true)
  expect(isTimingRun('OloEngine-Tests.exe --gtest_filter=MotionVectorMath.*')).toBe(false)
  expect(isTimingRun('grep PerfRegression notes.md')).toBe(false)
})

test('GPU load: utilisation alone is not busy; clock or power is', () => {
  const idle = parseGpuSample('210, 16.4, 31\n')
  expect(idle).toEqual({ clockMHz: 210, powerW: 16.4, utilization: 31 })
  expect(loadNote(idle, [])).toBeUndefined()
  expect(loadNote(parseGpuSample('2520, 180.2, 97'), [])).toContain('2520 MHz')
  expect(loadNote(idle, ['ninja.exe'])).toContain('ninja.exe')
  expect(parseGpuSample('')).toBeUndefined()
})

test('contenders are counted from tasklist CSV', () => {
  const csv = ['"System","4","Services","0","1 K"', '"ninja.exe","10","Console","1","9 K"', '"ninja.exe","11","Console","1","9 K"', '"WowB.exe","12","Console","1","9 K"'].join('\r\n')
  expect(contenders(csv)).toEqual(['ninja.exe x2'])
})

test('local checks are attributed to this worktree only, not a sibling with a longer name', () => {
  const rows = parseProcessRows(
    JSON.stringify([
      { Name: 'ninja.exe', ProcessId: 1, CommandLine: 'ninja -C C:\\repos\\OloEngine-foo\\build-cached', ExecutablePath: 'C:\\tools\\ninja.exe' },
      { Name: 'ninja.exe', ProcessId: 2, CommandLine: 'ninja -C C:\\repos\\OloEngine-foo-bar\\build-cached', ExecutablePath: 'C:\\tools\\ninja.exe' },
      { Name: 'OloEngine-Tests.exe', ProcessId: 3, CommandLine: 'x', ExecutablePath: 'C:\\repos\\OloEngine-foo\\build\\OloEngine-Tests.exe' },
    ]),
  )
  expect(localChecksRunning(rows, 'C:/repos/OloEngine-foo').map(r => r.pid)).toEqual([1, 3])
  expect(parseProcessRows('')).toEqual([])
  expect(parseProcessRows(JSON.stringify({ Name: 'ctest.exe', ProcessId: 9, CommandLine: null, ExecutablePath: null }))[0]?.pid).toBe(9)
})
