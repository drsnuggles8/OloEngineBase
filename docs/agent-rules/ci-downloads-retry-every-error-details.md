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
retry. On stdout there is nothing it can truncate. The same transfer written as
`curl --retry 3 --retry-all-errors "$url" > file`, where the shell opened the file, exited 0 and
left 4,492,768 bytes for the 4,194,304-byte body: the two partial bodies, then the whole one. A
pipe or a `$(...)` receives the same stream. So the flag is kept off every transfer without `-o`,
and a download that has to be retried is written with `-o`.

curl uses the last `--retry` of a command: with `--retry 3 --retry 0 --retry-all-errors` it made
one request against the same server and exited 56.

A genuine failure now costs the retries: a 404 under `-f` is retried too, three times with the
delay between them, before the step fails.

## The five downloads

| Step | Before | After |
|---|---|---|
| `setup-llvm-linux` (LLVM, 2 GB) | `--retry 3` | `--retry-all-errors` added |
| `setup-llvm-windows` (LLVM) | no retry | `--retry 3 --retry-delay 5 --retry-all-errors` |
| `video-ffmpeg.yml` (decode fixture) | `--retry 3` | `--retry-all-errors` added |
| `cross-vendor.yml` (Mesa llvmpipe) | `Invoke-WebRequest` | `curl.exe` with the same flags, exit code and SHA-256 checked |
| `vulkan-software.yml` (Mesa lavapipe) | `Invoke-WebRequest` | `curl.exe` with the same flags, exit code and SHA-256 checked |

The Mesa step of `vulkan-software.yml` runs after the build (two to four hours before #1705 gave
it a compiler cache, and still hours on a cold one), so one dropped connection there costs the
whole job. `Invoke-WebRequest -MaximumRetryCount` was not an
alternative: it answers HTTP status codes, not a connection that drops.

`setup-llvm-apt` fetches its signing key with `wget` inside its own four-attempt loop and is left
as it is.

## What is pinned by checksum

A download that is a tool the job then runs is held to a SHA-256 written next to its version, and
the step fails on a mismatch before anything is unpacked:

| Download | Where the checksum is | Checked with |
|---|---|---|
| LLVM for Linux | the `sha256` input of `setup-llvm-linux` | `sha256sum -c` |
| LLVM for Windows | the `sha256` input of `setup-llvm-windows` | `Get-FileHash` |
| Mesa 24.3.4 (`cross-vendor.yml`) | `$sha256` in the download step, beside the URL | `Get-FileHash` |
| Mesa `MESA_VERSION` (`vulkan-software.yml`) | `MESA_SHA256` in the workflow's `env`, beside the version | `Get-FileHash` |

The two Mesa archives were pinned by release tag only until PR #1706. Their checksums are of the
archives as downloaded on 2026-11-17; for 26.2.0 it equals the `digest` GitHub's release API
reports for the asset, and 24.3.4 predates that field. The same lines accept both archives locally
and refuse one checked against the other's checksum.

A version bump changes two values. The asset's digest is in
`gh api repos/pal1000/mesa-dist-win/releases/tags/<version>` under `assets[].digest`.

Not pinned: the decode fixture of `video-ffmpeg.yml`, a sample video from a third-party host. It
is test input, not a tool, and the step checks only that the file is not empty.

## What the hook reads

`scripts/check_ci_download_retries.py` takes every `run:` scalar of every tracked YAML file under
`.github/` from a YAML parser, so it sees what the shell receives:

- A folded scalar (`run: >`, used in `asan.yml`) is one line. Flags on its second source line
  count, and `curl` alone on its first line does not hide the rest.
- A script is split into commands the way a shell splits it: at `;`, `&&`, `||`, `|`, a newline
  and the brackets of a subshell or a PowerShell block, after `\` and backtick continuations are
  joined. Quotes hold a `;` or a `#` in its word, a `# comment` is not an argument, and a
  `$(...)` is a command of its own, inside double quotes too. Each command needs its own flags:
  a compliant download does not vouch for the one after it on the same line.
- A command is a download when its program is `curl` or `curl.exe`, after `sudo`, `if`, a
  `VAR=value` prefix or a PowerShell `$x =`, and with or without a directory in front. After
  `timeout`, `nice`, `ionice`, `nohup`, `stdbuf` or `xargs` the command is judged from the first
  word that names curl. A `curl` inside another command's argument (`echo 'curl ...'`) is text.
- A `curl` command that writes a file (`-o`, `--output`, `-O`, also inside a cluster such as
  `-fsSLo`) needs `--retry-all-errors` and a `--retry` count above zero, with or without `-f`.
  The count judged is the last one of the command.
- A `curl` command without `-o` is refused when it has `-f` (a download through stdout: write
  it to a file first) or `--retry-all-errors` (the retry would repeat bytes). With neither it is
  not judged: `curl -sSL "$page" | grep` is not a download the step acts on.
- curl's arguments are read in order, as curl reads them. The word after an option that takes a
  value is that value (`--header '--retry-all-errors'` sets no flag), also attached to a short
  option (`-oarchive.tar.gz`) or after `=`. Of two opposite flags the later is in force
  (`--retry-all-errors --no-retry-all-errors` is off). Destinations pair with URLs in order:
  each `-o` or `-O` serves one URL, `-o -` is stdout, and a URL beyond them goes to stdout, or
  under `--remote-name-all` to a file unless `--no-remote-name` marked it. `--expand-output` is
  `--output`.
  `--` ends the options. `--next`, or `:` in a cluster of short options (`-s:`), starts a new
  operation with the options reset, judged on its own; as the value of another option
  (`--header --next`) or after `--` it is text, as it is to curl. An unquoted redirection and its
  target are not arguments, bash's `&>log` and PowerShell's `*>log` included; a quoted `>` is
  (`--write-out '>status'`). The list of value-taking
  options is from `curl --help all` of 8.12 and 8.22; an option of a later curl is read as a
  flag, which can add a finding and cannot hide one.
- `Invoke-WebRequest` or `iwr` with `-OutFile` is refused.
- A finding in a literal block (`run: |`) reports the command's own line; in a folded or one-line
  script it reports the `run:` line.

What it does not read, because the text of the step does not hold it:

- the body of a here-document, so a script fed to `bash <<EOF` is not judged, and a script
  passed as one quoted word (`bash -c '...'`, `eval`);
- a variable: a `--retry` count that is one is taken as given, and options that arrive through
  one (`curl $CURL_OPTS ...`) are not seen;
- a curl config file (`-K`, `--config`, `.curlrc`);
- a shell function or alias that wraps curl, and a program outside the six above that runs it.

A download in a here-document, in a quoted script or behind a function is not judged at all. One
whose options arrive through a variable or a config file is judged on the options the step shows.
Either way the download and its retry options are written plainly in the step.

It runs its own cases before it reads the repository: must-fail scripts with the one message
each must produce, must-pass scripts, and YAML documents with the lines to report. It fails if one
of them stops behaving, so a change to the reader cannot switch a rule off in silence. It needs
PyYAML, which `pre-commit` installs, and exits 1 with that message when the import fails.
