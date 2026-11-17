# A CI download retries every error

Rules for any workflow or action step that downloads a file. Applies to every YAML file under
`.github/`. PR #1706.

```bash
curl -fL --retry 3 --retry-delay 5 --retry-all-errors -o "$file" "$url"
```

```powershell
curl.exe --fail --silent --show-error --location --retry 3 --retry-delay 5 --retry-all-errors --output $file $url
if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
```

## Rules

1. **Download with `curl` and both `--retry <n>` and `--retry-all-errors`.** `--retry` alone
   repeats a transfer only after a timeout or an HTTP 408, 429 or 5xx. A connection reset in the
   middle of the body (`curl: (56) Recv failure: Connection reset by peer`) is not on that list,
   so curl exits at once with the retries unused. `--retry-all-errors` alone enables no retry
   either: the default count is zero, and curl uses the last `--retry` it is given.
   [Details](ci-downloads-retry-every-error-details.md#what-curl-retries)
2. **Do not add `--retry-all-errors` to a transfer piped to stdout.** A retry restarts the
   transfer; with `-o` curl truncates the file first, on a pipe the bytes already written are
   written again. [Details](ci-downloads-retry-every-error-details.md#what-curl-retries)
3. **In PowerShell use `curl.exe`, not `Invoke-WebRequest -OutFile`, and check `$LASTEXITCODE`.**
   `Invoke-WebRequest` gives up on the first dropped connection, and a failed native command does
   not fail a `pwsh` step by itself.
   [Details](ci-downloads-retry-every-error-details.md#the-five-downloads)
4. **Pin a downloaded tool with a checksum, and bump the checksum with the version.** The retry
   makes the download arrive; it does not make it the right file. A release tag is only a URL.
   [Details](ci-downloads-retry-every-error-details.md#what-is-pinned-by-checksum)
5. **The pre-commit hook `ci-download-retries` holds rules 1 and 3.**
   `scripts/check_ci_download_retries.py` reads every `run:` script under `.github/` through a
   YAML parser and judges each command on its own arguments. A command that must not retry opts
   out on its own line with `# OLO_DOWNLOAD_NO_RETRY_OK: <reason>`.
   [Details](ci-downloads-retry-every-error-details.md#what-the-hook-reads)

---

The measurements and the failures behind these rules:
[ci-downloads-retry-every-error-details.md](ci-downloads-retry-every-error-details.md).
