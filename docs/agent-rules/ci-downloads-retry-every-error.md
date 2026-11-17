# A CI download retries every error

**A workflow or action that downloads a file uses `curl` (`curl.exe` in PowerShell) with
`--retry <n> --retry-all-errors`.** `--retry` alone is not enough, and
`Invoke-WebRequest -OutFile` is not used.

```bash
curl -fL --retry 3 --retry-delay 5 --retry-all-errors -o "$file" "$url"
```

```powershell
curl.exe --fail --silent --show-error --location --retry 3 --retry-delay 5 --retry-all-errors --output $file $url
if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
```

- **What `--retry` covers on its own:** a timeout and an HTTP 408, 429 or 5xx. A connection
  reset in the middle of the body (`curl: (56) Recv failure: Connection reset by peer`) is
  not on that list, so curl exits at once with the retries unused.
- **`--retry-all-errors` restarts the transfer** and truncates the `-o` file first. Do not add
  it to a transfer piped to stdout: the bytes already written are written again.
- **PowerShell needs the exit-code check.** A failed native command does not fail a `pwsh`
  step by itself.
- **Pin what you download with a checksum** where the file is a tool (the LLVM actions do).
  The retry makes the download arrive; it does not make it the right file.

The pre-commit hook `ci-download-retries` (`scripts/check_ci_download_retries.py`) refuses a
`curl -f` without the flag and an `Invoke-WebRequest -OutFile` in any YAML under `.github/`.
A command that must not retry opts out on its first line with
`# OLO_DOWNLOAD_NO_RETRY_OK: <reason>`.

## What this found (PR #1698)

`setup-llvm-linux` downloads a 2 GB LLVM release with `curl -fL --retry 3 --retry-delay 5`.
On one branch in one day the download was reset twice, 80 MB and 124 MB in: `ASan + LSan
(Linux) tests 4/4`, then the `UBSan (Linux/Clang)` build, which also skipped its test shards.
Both logs show one transfer and exit code 56 inside two seconds, with no second attempt.

A local server that resets the connection after 1 MiB on its first two requests gives the
same result on curl 8.12: with `--retry 3` curl makes one request and exits 56; with
`--retry-all-errors` added it makes three and the file is complete.

Four other downloads had the same gap or no retry at all: the Windows LLVM release, the
decode fixture of `video-ffmpeg.yml`, and the Mesa archive of `cross-vendor.yml` and
`vulkan-software.yml`. The last one runs after a build of two to four hours, so one dropped
connection there costs the whole job.
