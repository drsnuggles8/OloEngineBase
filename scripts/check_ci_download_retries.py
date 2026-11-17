#!/usr/bin/env python3
"""Fail if a workflow or action downloads a file without retrying every error.

curl's --retry repeats a transfer only after a timeout or an HTTP 408, 429 or 5xx.
A connection reset in the middle of the body (curl exit 56) is not on that list, and
it is how a large download from a release CDN fails. A step that says `--retry 3`
therefore fails on the first reset with its three retries unused. --retry-all-errors
makes --retry cover it, and does nothing without a --retry count above zero.

Invoke-WebRequest gives up on the first dropped connection, and its
-MaximumRetryCount answers HTTP status codes only. A file download in PowerShell
uses curl.exe.

Two rules, for every `run:` script in a tracked YAML file under .github/:

  * a curl command that fails on an HTTP error (-f / --fail) carries
    `--retry <n>` with n above zero and `--retry-all-errors` (the last
    `--retry` of the command is the one curl uses);
  * no `Invoke-WebRequest ... -OutFile`.

A curl command without -f is not a download whose failure the step acts on, and is
left alone (--retry-all-errors must not be used on a transfer piped to stdout: a
retry would repeat the bytes already written).

The scripts are read through a YAML parser, so a folded scalar (`run: >`) is judged
as the one line the shell receives. Each command of a script is judged on its own
arguments: a compliant download does not vouch for the one after it on the same line.

Opt out on the command's own line with:

    # OLO_DOWNLOAD_NO_RETRY_OK: <reason>
"""

from __future__ import annotations

import re
import subprocess
import sys

try:
    import yaml
except ImportError:  # no silent pass: a hook that cannot read YAML checks nothing
    print("check_ci_download_retries.py needs PyYAML (pip install pyyaml); pre-commit installs it.")
    sys.exit(1)

OPT_OUT = "OLO_DOWNLOAD_NO_RETRY_OK:"
CURL_RE = re.compile(r"(?<![\w./-])curl(?:\.exe)?\s")
FAIL_RE = re.compile(r"\s(?:--fail(?:-with-body)?\b|-[A-Za-z]*f[A-Za-z]*\b)")
RETRY_COUNT_RE = re.compile(r"\s--retry(?:\s+|=)(\d+)\b")
IWR_RE = re.compile(r"\b(?:Invoke-WebRequest|iwr)\b.*\s-OutFile\b", re.IGNORECASE)
SEPARATOR_RE = re.compile(r";|&&|\|\||\|")
CONTINUES = ("\\", "`")

NO_ALL_ERRORS = "curl download without --retry-all-errors; --retry alone does not repeat a connection reset"
NO_COUNT = "curl download with --retry-all-errors whose last --retry count is missing or zero; the flag alone enables no retry"
IWR = "Invoke-WebRequest -OutFile does not retry a dropped connection; use curl.exe with --retry <n> --retry-all-errors"


def logical_lines(script: str) -> list[tuple[int, str]]:
    """Each non-comment line of a script, joined with its continuation lines.

    Returns (index of the first source line, text).
    """
    lines = script.split("\n")
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
        joined.append((start, text))
        index += 1
    return joined


def command_findings(command: str) -> list[str]:
    """What is wrong with one command, judged on its own arguments."""
    found = []
    curl = CURL_RE.search(command)
    if curl and FAIL_RE.search(command[curl.start():]):
        if "--retry-all-errors" not in command:
            found.append(NO_ALL_ERRORS)
        else:
            # curl keeps the last --retry it is given: `--retry 3 --retry 0`
            # retries nothing.
            counts = RETRY_COUNT_RE.findall(command)
            if not counts or int(counts[-1]) == 0:
                found.append(NO_COUNT)
    if IWR_RE.search(command):
        found.append(IWR)
    return found


def script_findings(script: str) -> list[tuple[int, str]]:
    """(index of the script line, message) for every non-compliant command."""
    found = []
    for start, line in logical_lines(script):
        if OPT_OUT in line:
            continue
        # A quoted separator splits a command in two; that can only add a
        # finding, never hide one, and the opt-out above answers it.
        for command in SEPARATOR_RE.split(line):
            for message in command_findings(" " + command + " "):
                found.append((start, message))
    return found


def run_scripts(text: str) -> list[tuple[int, bool, str]]:
    """Every `run:` scalar of a YAML document: (1-based line, is literal block, script)."""
    scripts = []

    def walk(node: yaml.Node) -> None:
        if isinstance(node, yaml.MappingNode):
            for key, value in node.value:
                if isinstance(key, yaml.ScalarNode) and key.value == "run" and isinstance(value, yaml.ScalarNode):
                    scripts.append((value.start_mark.line + 1, value.style == "|", value.value))
                else:
                    walk(value)
        elif isinstance(node, yaml.SequenceNode):
            for item in node.value:
                walk(item)

    root = yaml.compose(text, Loader=yaml.SafeLoader)
    if root is not None:
        walk(root)
    return scripts


def file_findings(text: str) -> list[tuple[int, str]]:
    """(1-based line in the file, message). A folded or one-line script reports its `run:` line."""
    found = []
    for line, literal, script in run_scripts(text):
        for index, message in script_findings(script):
            # A literal block keeps its lines, the first one after the `|`.
            found.append((line + 1 + index if literal else line, message))
    return found


# What the rules must catch and must leave alone. Checked on every run, so a change
# to the patterns that stops them matching fails here, not in silence.
SCRIPTS_MUST_FAIL = [
    'curl -fL --retry 3 --retry-delay 5 -o "${work}/${tarball}" "$url"',
    "curl.exe --fail --silent --show-error --location --output $archive $url",
    "curl -fSL --retry 3 --max-time 120 \\\n  -o sample.mp4 https://example.invalid/sample.mp4",
    "Invoke-WebRequest -Uri $url -OutFile mesa3d.7z",
    # the flag enables nothing without a count, or with a count of zero
    'curl -fL --retry-all-errors -o "$file" "$url"',
    'curl -fL --retry 0 --retry-all-errors -o "$file" "$url"',
    # curl keeps the last count it is given
    'curl -fL --retry 3 --retry 0 --retry-all-errors -o "$file" "$url"',
    # the first command's flags do not vouch for the second
    'curl -fL --retry 3 --retry-all-errors -o a "$a"; curl -fL --retry 3 -o b "$b"',
    'curl -fL --retry 3 --retry-all-errors -o a "$a" && curl -fL -o b "$b"',
]
SCRIPTS_MUST_PASS = [
    'curl -fL --retry 3 --retry-delay 5 --retry-all-errors -o "${work}/${tarball}" "$url"',
    "curl -fSL --retry 3 --retry-all-errors --max-time 120 \\\n  -o sample.mp4 https://example.invalid/sample.mp4",
    "curl.exe --fail --retry 3 --retry-all-errors --output mesa3d.7z $url",
    "curl.exe --fail --retry=5 --retry-all-errors --output mesa3d.7z $url",
    'curl -fL --retry 0 --retry 3 --retry-all-errors -o "$file" "$url"',
    'curl -fL --retry 3 --retry-all-errors -o a "$a"; curl -fL --retry 3 --retry-all-errors -o b "$b"',
    "# curl -fL -o file https://example.invalid/file",
    "if curl -sSL --max-time 30 https://example.invalid/ | grep -qi reflection; then",
    "for tool in autoconf automake pkg-config curl zip unzip tar; do",
    "sudo dnf install -y autoconf libtool pkgconf-pkg-config curl zip unzip tar",
    "curl -fL -o file https://example.invalid/file # OLO_DOWNLOAD_NO_RETRY_OK: fixture",
]
# (document, the 1-based lines that must be reported)
FILES = [
    # a literal block reports the command's own line
    ("steps:\n  - run: |\n      echo start\n      curl -fL -o f https://example.invalid/f\n", [4]),
    # a folded scalar is one line to the shell: the flags on its second line count
    ('steps:\n  - run: >-\n      curl -fL --retry 3\n      --retry-all-errors -o "$file" "$url"\n', []),
    # and `curl` alone on the first line does not hide what follows it
    ('steps:\n  - run: >\n      curl\n      -fL --retry 3 -o "$file" "$url"\n', [2]),
    ("steps:\n  - run: curl -fL -o f https://example.invalid/f\n", [2]),
    # a commented-out step is not a step, and `run` is found at any depth
    ("steps:\n  # - run: curl -fL -o f https://example.invalid/f\n  - name: none\n", []),
    ("jobs:\n  a:\n    steps:\n      - name: x\n        run: |\n          Invoke-WebRequest -Uri $u -OutFile m.7z\n", [6]),
]


def self_check() -> list[str]:
    problems = []
    for script in SCRIPTS_MUST_FAIL:
        if not script_findings(script):
            problems.append(f"self-check: not caught: {script!r}")
    for script in SCRIPTS_MUST_PASS:
        if script_findings(script):
            problems.append(f"self-check: wrongly caught: {script!r}")
    for document, expected in FILES:
        got = [line for line, _ in file_findings(document)]
        if got != expected:
            problems.append(f"self-check: lines {got}, expected {expected}: {document!r}")
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
                text = handle.read()
        except OSError:
            continue  # deleted in the working tree, still in the index
        try:
            found = file_findings(text)
        except yaml.YAMLError as error:
            found = [(getattr(getattr(error, "problem_mark", None), "line", 0) + 1, f"not readable as YAML: {error}")]
        for number, message in found:
            count += 1
            print(f"{path}:{number}: {message}")
    if count:
        print(
            f"\n{count} finding(s) in {len(files)} files: a download that does not retry every error. "
            "See docs/agent-rules/ci-downloads-retry-every-error.md."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
