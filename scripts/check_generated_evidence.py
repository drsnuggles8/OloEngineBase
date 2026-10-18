#!/usr/bin/env python3
"""Fail when generated verification output is committed as source (issue #1610).

Evidence tests, benchmark captures and live verification campaigns write PNGs,
HDRs, JSON/XML reports, replay scenes and logs. Each run regenerates them, so a
committed copy goes stale and every later edit adds another full copy to Git
history, where a later deletion does not remove it. The tree keeps test inputs
(goldens and fixtures a test READS) and deliberately curated documentation
images. Everything else belongs in an ignored output directory or a CI artifact.

The single source of truth is scripts/generated-evidence-allowlist.txt: one
glob per line followed by the test or document that reads the file. Two rules
use it.

1. A file under a GUARDED root must match an allow-list entry. Under
   OloEditor/assets/tests/ that is every file: the visual/ directory is ignored
   wholesale, and a golden is added with `git add -f` plus an entry naming its
   reader. Under docs/ it is every file except Markdown and Git attribute
   files: prose is free, and any image, video, report or scene is curated
   deliberately or not at all.
2. A tracked file that the repository's .gitignore files would exclude must
   match an allow-list entry. This catches `git add -f` of a capture anywhere in
   the repository. A developer's global excludes file and .git/info/exclude are
   not consulted, so the result is the same on every machine and in CI.

Two more checks keep the list honest: every entry carries a reason, and every
entry still allows at least one tracked file that needs allowing.

Only the index is checked (tracked and staged files). An untracked capture in a
worktree is not an error; it is what a test run is supposed to leave behind.

The fixtures in SELF_TEST run first, every time, and the run fails if any
expected report is missing or a clean tree reports anything. A guard that cannot
fail proves nothing.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ALLOWLIST = "scripts/generated-evidence-allowlist.txt"

# Every file under these roots must be allow-listed.
GUARDED_ALL = ("OloEditor/assets/tests/",)

# Under these roots, every file except prose must be allow-listed.
GUARDED_DATA = ("docs/",)
PROSE_EXTENSIONS = frozenset({".md"})
PROSE_NAMES = frozenset({".gitignore", ".gitattributes"})


def compile_glob(pattern: str) -> re.Pattern[str]:
    """Translate a repository glob: `*` stays inside one path segment, `**` crosses them."""
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def parse_allowlist(text: str) -> tuple[list[tuple[str, re.Pattern[str], int]], list[str]]:
    entries = []
    errors = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) < 2 or not parts[1].strip():
            errors.append(f"{ALLOWLIST}:{number}: '{parts[0]}' has no reason; name the test or doc that reads it")
            continue
        entries.append((parts[0], compile_glob(parts[0]), number))
    return entries, errors


def is_guarded(path: str) -> bool:
    if path.startswith(GUARDED_ALL):
        return True
    if path.startswith(GUARDED_DATA):
        name = path.rsplit("/", 1)[-1]
        dot = name.rfind(".")
        return name not in PROSE_NAMES and (dot <= 0 or name[dot:].lower() not in PROSE_EXTENSIONS)
    return False


def run_checks(tracked: list[str], ignored: set[str], allowlist_text: str) -> list[str]:
    entries, errors = parse_allowlist(allowlist_text)
    used = set()

    def allowed(path: str) -> bool:
        hit = False
        for glob, regex, number in entries:
            if regex.match(path):
                used.add(number)
                hit = True
        return hit

    for path in tracked:
        guarded = is_guarded(path)
        is_ignored = path in ignored
        # An entry is in use only while it allows a file that needs allowing; one that
        # matches nothing but prose would silently allow the next force-added capture.
        if not (guarded or is_ignored) or allowed(path):
            continue
        if is_ignored:
            errors.append(f"{path}: tracked although .gitignore excludes it (force-added?)")
        else:
            errors.append(f"{path}: generated-output location, not on the allow-list")
    for glob, _, number in entries:
        if number not in used:
            errors.append(f"{ALLOWLIST}:{number}: '{glob}' allows no tracked file that needs it; delete the entry")
    return errors


# One case per behaviour. `expect` lists substrings the report must contain;
# an empty list means the tree must be clean.
SELF_TEST_ALLOW = """
# comment
OloEditor/assets/tests/golden/**            GoldenImageTests
OloEditor/assets/tests/visual/Water_*.png   WaterVisualEvidenceTest
OloEditor/assets/tests/splat/*.ply          GaussianSplatFixtureTest
docs/images/**                              curated documentation images
"""
SELF_TEST_BASE = [
    "OloEditor/assets/tests/golden/fxaa_hard_edge.png",
    "OloEditor/assets/tests/visual/Water_Overview.png",
    "OloEditor/assets/tests/splat/fixture_discs.ply",
    "docs/images/dog/closeup.jpg",
    "docs/analysis/report" + ".md",
    "OloEngine/src/Renderer.cpp",
]
SELF_TEST = {
    "clean: a golden, a fixture, a curated doc image and prose": ([], set(), None, []),
    "capture PNG beside the goldens": (
        ["OloEditor/assets/tests/visual/Water_GL_Forward_Live_Control.webp"],
        set(),
        None,
        ["visual/Water_GL_Forward_Live_Control.webp: generated-output location"],
    ),
    "render-target export report": (
        ["OloEditor/assets/tests/exports/RenderTargetExports_GL_Forward.json"],
        set(),
        None,
        ["exports/RenderTargetExports_GL_Forward.json: generated-output location"],
    ),
    "replay scene force-added past .gitignore": (
        ["OloEngine/tests/crosspath-live/Cell_GL_Deferred.olo"],
        {"OloEngine/tests/crosspath-live/Cell_GL_Deferred.olo"},
        None,
        ["Cell_GL_Deferred.olo: tracked although .gitignore excludes it"],
    ),
    "campaign data under docs": (
        ["docs/testing/evidence/campaign-9999/run-1/frame.hdr", "docs/testing/evidence/campaign-9999/report.json"],
        set(),
        None,
        ["campaign-9999/run-1/frame.hdr: generated-output", "campaign-9999/report.json: generated-output"],
    ),
    "a single star stays in its directory": (
        ["OloEditor/assets/tests/visual/Water_campaign/frame.png"],
        set(),
        None,
        ["visual/Water_campaign/frame.png: generated-output location"],
    ),
    "an entry is anchored at the repository root": (
        ["OloEditor/assets/tests/visual/docs/images/frame.png"],
        set(),
        None,
        ["visual/docs/images/frame.png: generated-output location"],
    ),
    "entry without a reason":([], set(), SELF_TEST_ALLOW + "docs/images/**/*.jpg\n", ["has no reason"]),
    "motion capture under docs": (
        ["docs/testing/evidence/campaign-9999/turntable.mp4"],
        set(),
        None,
        ["campaign-9999/turntable.mp4: generated-output location"],
    ),
    "an entry matching only prose is stale": (
        ["docs/reviews/gallery/notes" + ".md"],
        set(),
        SELF_TEST_ALLOW + "docs/reviews/gallery/**  approval gallery\n",
        ["'docs/reviews/gallery/**' allows no tracked file"],
    ),
    "an entry for an unguarded, unignored file is stale": (
        ["OloEngine/tests/fixtures/x.bin"],
        set(),
        SELF_TEST_ALLOW + "OloEngine/tests/fixtures/x.bin  fixture\n",
        ["'OloEngine/tests/fixtures/x.bin' allows no tracked file"],
    ),
    "stale entry":([], set(), SELF_TEST_ALLOW + "docs/gone/*.png  removed campaign\n", ["'docs/gone/*.png' allows no tracked file"]),
}


def self_test() -> list[str]:
    failures = []
    for name, (extra, ignored, allow, expect) in SELF_TEST.items():
        report = run_checks(SELF_TEST_BASE + extra, ignored, SELF_TEST_ALLOW if allow is None else allow)
        if not expect and report:
            failures.append(f"self-test '{name}': a clean tree reported {report}")
        for want in expect:
            if not any(want in line for line in report):
                failures.append(f"self-test '{name}': expected a report containing '{want}', got {report}")
    for path, want in (
        ("docs/e/README" + ".md", False),
        ("docs/e/.gitignore", False),
        ("docs/a.PNG", True),
        ("docs/e/turntable.mp4", True),
        ("docs/archive", True),
        ("OloEditor/assets/tests/x", True),
    ):
        if is_guarded(path) != want:
            failures.append(f"self-test: is_guarded('{path}') should be {want}")
    return failures


def git_list(root: Path, *args: str) -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z", *args], cwd=root, capture_output=True, text=True, encoding="utf-8", check=True
    ).stdout
    return [p for p in out.split("\0") if p]


def main() -> int:
    failures = self_test()
    if failures:
        print("check_generated_evidence: the guard failed its own negative controls:", file=sys.stderr)
        for line in failures:
            print(f"  {line}", file=sys.stderr)
        return 2
    root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, encoding="utf-8", check=True
        ).stdout.strip()
    )
    tracked = git_list(root, "--cached")
    # --exclude-per-directory reads only the repository's .gitignore files;
    # --exclude-standard would also read core.excludesFile and .git/info/exclude.
    ignored = set(git_list(root, "--cached", "--ignored", "--exclude-per-directory=.gitignore"))
    allow_path = root / ALLOWLIST
    if not allow_path.is_file():
        print(f"check_generated_evidence: {ALLOWLIST} is missing", file=sys.stderr)
        return 2
    errors = run_checks(tracked, ignored, allow_path.read_text(encoding="utf-8"))
    for line in errors:
        print(line)
    if errors:
        print(
            f"\n{len(errors)} generated-evidence error(s). Run output stays local or goes to a CI artifact: "
            "`git rm --cached <path>`. A file a test READS (a golden or fixture) or a curated doc image gets "
            f"a line in {ALLOWLIST} naming its reader. See docs/agent-rules/generated-evidence-stays-out-of-git.md."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
