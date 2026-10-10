// Collapses the lines of a big build or test log that carry no information once you know
// how many there were: ninja's progress lines and gtest's passing RUN/OK pairs. Every other
// line (warnings, errors, failures, a test's own output, summaries) is kept verbatim and
// in order, so nothing a reader acts on is lost.

export const MIN_CHARS = 8000
const MIN_SAVING = 0.25

const GUTTER = /^ *\d+\t/
const NINJA_PROGRESS = /^\[\d+\/\d+\] (?:Building|Linking|Generating|Running|Copying|Automatic|Re-running|Scanning|Creating|Install|Performing|No install)\b/
const GTEST_RUN = /^\[ RUN      \] (\S+)/
const GTEST_OK = /^\[       OK \] (\S+)/
const GTEST_SEPARATOR = /^\[----------\] /

export type Compressed = { text: string; progress: number; passing: number; separators: number }

export function compressLog(text: string): Compressed | undefined {
  if (text.length < MIN_CHARS) {
    return undefined
  }
  const lines = text.split('\n')
  const bare = lines.map(l => l.replace(/\r$/, '').replace(GUTTER, ''))
  const out: string[] = []
  let progress = 0
  let passing = 0
  let separators = 0

  for (let i = 0; i < lines.length; i += 1) {
    const line = bare[i] ?? ''

    // A run of ninja progress lines: keep the first and the last, count the middle.
    if (NINJA_PROGRESS.test(line)) {
      let j = i
      while (j + 1 < lines.length && NINJA_PROGRESS.test(bare[j + 1] ?? '')) {
        j += 1
      }
      const runLength = j - i + 1
      if (runLength > 2) {
        out.push(lines[i] ?? '', `… ${runLength - 2} ninja progress lines …`, lines[j] ?? '')
        progress += runLength - 2
      } else {
        out.push(...lines.slice(i, j + 1))
      }
      i = j
      continue
    }

    // A test that passed with no output of its own: RUN immediately followed by its OK.
    const run = GTEST_RUN.exec(line)
    if (run !== null && GTEST_OK.exec(bare[i + 1] ?? '')?.[1] === run[1]) {
      passing += 1
      i += 1
      continue
    }

    if (GTEST_SEPARATOR.test(line)) {
      separators += 1
      continue
    }

    out.push(lines[i] ?? '')
  }

  // Say where the collapsed passing tests were, once, not per line.
  const removedLines = progress + passing * 2 + separators
  if (removedLines === 0) {
    return undefined
  }
  const parts = [
    progress > 0 ? `${progress} ninja progress lines` : '',
    passing > 0 ? `${passing} passing tests (their RUN/OK lines)` : '',
    separators > 0 ? `${separators} gtest suite separators` : '',
  ].filter(Boolean)
  out.push(`[olo-context-saver: collapsed ${parts.join(', ')}; every warning, error, failure and summary line above is verbatim]`)
  const result = out.join('\n')
  return result.length <= text.length * (1 - MIN_SAVING) ? { text: result, progress, passing, separators } : undefined
}
