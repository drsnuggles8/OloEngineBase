# A CI download retries every error: details

The rules are in [ci-downloads-retry-every-error.md](ci-downloads-retry-every-error.md). This file
holds the failures and the measurement behind them (PR #1706).

## What happened

`setup-llvm-linux` downloads a 2 GB LLVM release with `curl -fL --retry 3 --retry-delay 5`. On
PR #1698 the download was reset twice in one day, 80 MB and 124 MB in: `ASan + LSan (Linux)
tests 4/4`, then the `UBSan (Linux/Clang)` build, which also skipped its four test shards. Both
logs show one transfer and exit code 56 inside two seconds, with no second attempt. Both jobs
passed on a rerun, so each read as an infrastructure failure and the unused retries went unnoticed
the first time.

## What curl retries

`--retry <n>` repeats a transfer after a timeout, an FTP 4xx or an HTTP 408, 429, 500, 502, 503
or 504. Any other error ends the command. `--retry-all-errors` (curl 7.71 and later) extends the
retry to every error, and is a modifier of `--retry`: without a count above zero nothing is
retried.

Measured against a local HTTP server that sends 1 MiB of a 4 MiB body and then resets the
connection, on its first two requests:

| curl | flags | requests | exit | file |
|---|---|---|---|---|
| Git for Windows 8.12 | `--retry 3 --retry-delay 1` | 1 | 56 | partial |
| Git for Windows 8.12 | `--retry 3 --retry-delay 1 --retry-all-errors` | 3 | 0 | 4,194,304 bytes |
| Windows `curl.exe` 8.22 | `--retry 3 --retry-delay 1 --retry-all-errors` | 3 | 0 | 4,194,304 bytes |

The complete file on the third request also shows that curl truncates the `-o` file before a
retry. On a pipe there is nothing to truncate, which is why the flag is kept off transfers written
to stdout.

A genuine failure now costs the retries: a 404 under `-f` is retried too, three times with the
delay between them, before the step fails.

## The five downloads

| Step | Before | After |
|---|---|---|
| `setup-llvm-linux` (LLVM, 2 GB) | `--retry 3` | `--retry-all-errors` added |
| `setup-llvm-windows` (LLVM) | no retry | `--retry 3 --retry-delay 5 --retry-all-errors` |
| `video-ffmpeg.yml` (decode fixture) | `--retry 3` | `--retry-all-errors` added |
| `cross-vendor.yml` (Mesa llvmpipe) | `Invoke-WebRequest` | `curl.exe` with the same flags, exit code checked |
| `vulkan-software.yml` (Mesa lavapipe) | `Invoke-WebRequest` | `curl.exe` with the same flags, exit code checked |

The Mesa step of `vulkan-software.yml` runs after a build of two to four hours, so one dropped
connection there costs the whole job. `Invoke-WebRequest -MaximumRetryCount` was not an
alternative: it answers HTTP status codes, not a connection that drops.

`setup-llvm-apt` fetches its signing key with `wget` inside its own four-attempt loop and is left
as it is.

## What the hook reads

`scripts/check_ci_download_retries.py` takes every `run:` scalar of every tracked YAML file under
`.github/` from a YAML parser, so it sees what the shell receives:

- A folded scalar (`run: >`, used in `asan.yml`) is one line. Flags on its second source line
  count, and `curl` alone on its first line does not hide the rest.
- A script is split into commands at `;`, `&&`, `||` and `|`, after `\` and backtick continuations
  are joined. Each command needs its own flags: a compliant download does not vouch for the one
  after it on the same line.
- A `curl` command with `-f` / `--fail` needs `--retry-all-errors` and a `--retry` count above
  zero. One without `-f` is not judged.
- A finding in a literal block (`run: |`) reports the command's own line; in a folded or one-line
  script it reports the `run:` line.

It runs its own must-fail and must-pass cases before it reads the repository, and fails if one of
them stops behaving, so a change to its patterns cannot switch it off in silence. It needs PyYAML,
which `pre-commit` installs, and exits 1 with that message when the import fails.
