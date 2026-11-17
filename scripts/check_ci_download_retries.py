#!/usr/bin/env python3
"""Fail if a workflow or action downloads a file without retrying every error.

curl's --retry repeats a transfer only after a timeout or an HTTP 408, 429 or 5xx.
A connection reset in the middle of the body (curl exit 56) is not on that list, and
it is how a large download from a release CDN fails. A step that says `--retry 3`
therefore fails on the first reset with its three retries unused. --retry-all-errors
makes --retry cover it.

Invoke-WebRequest gives up on the first dropped connection, and its
-MaximumRetryCount answers HTTP status codes only. A file download in PowerShell
uses curl.exe.

Two rules, for every tracked YAML file under .github/:

  * a curl command that fails on an HTTP error (-f / --fail) carries
    --retry-all-errors;
  * no `Invoke-WebRequest ... -OutFile`.

A curl command without -f is not a download whose failure the step acts on, and is
left alone (--retry-all-errors must not be used on a transfer piped to stdout: a
retry would repeat the bytes already written).

Opt out on the command's first line with:

    # OLO_DOWNLOAD_NO_RETRY_OK: <reason>
"""

from __future__ import annotations

import re
import subprocess
import sys

OPT_OUT = "OLO_DOWNLOAD_NO_RETRY_OK:"
CURL_RE = re.compile(r"(?<![\w./-])curl(?:\.exe)?\s")
FAIL_RE = re.compile(r"\s(?:--fail(?:-with-body)?\b|-[A-Za-z]*f[A-Za-z]*\b)")
IWR_RE = re.compile(r"\b(?:Invoke-WebRequest|iwr)\b.*\s-OutFile\b", re.IGNORECASE)
CONTINUES = ("\\", "`")


def commands(lines: list[str]) -> list[tuple[int, str]]:
    """Each non-comment line joined with the continuation lines that follow it."""
    joined = []
    index = 0
    while index < len(lines):
        start = index
        text = lines[index].rstrip()
        if text.lstrip().startswith("#"):
            index += 1
            continue
        while text.endswith(CONTINUES) and index + 1 < len(lines):
            index += 1
            text = text[:-1] + " " + lines[index].strip()
        joined.append((start + 1, text))
        index += 1
    return joined


def findings(lines: list[str]) -> list[tuple[int, str]]:
    found = []
    for number, command in commands(lines):
        if OPT_OUT in command:
            continue
        curl = CURL_RE.search(command)
        if curl and FAIL_RE.search(command[curl.start():]) and "--retry-all-errors" not in command:
            found.append(
                (number, "curl download without --retry-all-errors; --retry alone does not repeat a connection reset")
            )
        if IWR_RE.search(command):
            found.append(
                (number, "Invoke-WebRequest -OutFile does not retry a dropped connection; use curl.exe with --retry-all-errors")
            )
    return found


# What the two rules must catch and must leave alone. Checked on every run, so a
# change to the patterns that stops them matching fails here, not in silence.
MUST_FAIL = [
    'curl -fL --retry 3 --retry-delay 5 -o "${work}/${tarball}" "$url"',
    "curl.exe --fail --silent --show-error --location --output $archive $url",
    "curl -fSL --retry 3 --max-time 120 \\\n  -o sample.mp4 https://example.invalid/sample.mp4",
    "Invoke-WebRequest -Uri $url -OutFile mesa3d.7z",
]
MUST_PASS = [
    'curl -fL --retry 3 --retry-delay 5 --retry-all-errors -o "${work}/${tarball}" "$url"',
    "curl -fSL --retry 3 --retry-all-errors --max-time 120 \\\n  -o sample.mp4 https://example.invalid/sample.mp4",
    "curl.exe --fail --retry 3 --retry-all-errors --output mesa3d.7z $url",
    "# curl -fL -o file https://example.invalid/file",
    "if curl -sSL --max-time 30 https://example.invalid/ | grep -qi reflection; then",
    "for tool in autoconf automake pkg-config curl zip unzip tar; do",
    "sudo dnf install -y autoconf libtool pkgconf-pkg-config curl zip unzip tar",
    "curl -fL -o file https://example.invalid/file # OLO_DOWNLOAD_NO_RETRY_OK: fixture",
]


def self_check() -> list[str]:
    problems = []
    for text in MUST_FAIL:
        if not findings(text.split("\n")):
            problems.append(f"self-check: not caught: {text!r}")
    for text in MUST_PASS:
        if findings(text.split("\n")):
            problems.append(f"self-check: wrongly caught: {text!r}")
    return problems


def tracked_ci_files() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z", ".github"], check=True, capture_output=True
    ).stdout.decode("utf-8", errors="replace")
    return [path for path in out.split("\0") if path.endswith((".yml", ".yaml"))]


def main() -> int:
    problems = self_check()
    if problems:
        print("\n".join(problems))
        return 1
    files = tracked_ci_files()
    count = 0
    for path in files:
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                lines = handle.readlines()
        except OSError:
            continue  # deleted in the working tree, still in the index
        for number, message in findings(lines):
            count += 1
            print(f"{path}:{number}: {message}")
    if count:
        print(
            f"\n{count} download(s) in {len(files)} files that do not retry every error. "
            "See docs/agent-rules/ci-downloads-retry-every-error.md."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
