#!/usr/bin/env python3
"""Fail if a header uses a profiling macro without including its defining header.

Headers are self-contained here (CLAUDE.md -> Conventions). A header that expands
OLO_PROFILE_FUNCTION() / OLO_PROFILE_SCOPE() but does not include
OloEngine/Debug/Instrumentor.h compiles today only because every current includer
happens to have the macro already in scope -- from OloEnginePCH.h, or from a sibling
header pulled in first. The omission is invisible on Windows and a hard compile error
on the Linux jobs the moment somebody adds the *first* include of that header from a
translation unit that lacks the macro. The person who trips it is never the person who
wrote it; the blame lands on an unrelated one-line include. That is issue #1071, and
it cost a CI round on PR #1062 before anyone noticed the class.

The macro set is read out of Instrumentor.h itself, so it cannot drift.

Position counts, not just presence: an include that sits BELOW the first expansion is
not in scope at that expansion, so the header still does not compile on its own. A use
inside a `#define` body is the exception -- it expands wherever the macro is called, so
it obliges the header to own the include but places no constraint on where.

Opt out on a line of its own with:

    // OLO_PROFILING_INCLUDE_OK: <reason>

This is a cheap textual ratchet, not a self-containment check: it knows about one
macro family only. Compiling every public header as its own translation unit would
catch the whole class and is a bigger piece of work.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SRC_ROOT = Path("OloEngine/src")
INSTRUMENTOR = SRC_ROOT / "OloEngine/Debug/Instrumentor.h"
SUFFIXES = {".h", ".hpp", ".inl"}

# Instrumentor.h *defines* the macros, so it is not a client of itself. Profiler.h
# is a thin wrapper that includes it and re-exports the macros under
# OLO_ASSET_PROFILE_*, so it is exempt as a file and satisfies the check as an
# include.
EXEMPT = {INSTRUMENTOR.as_posix()}

DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(OLO_[A-Z0-9_]+)", re.MULTILINE)
# A trailing comment on the include line is normal style here, so do not anchor at
# end-of-line. Either header brings the macros in.
INCLUDE_RE = re.compile(
    r'^[ \t]*#[ \t]*include[ \t]+"OloEngine/Debug/(?:Instrumentor|Profiler)\.h"', re.MULTILINE
)
OPT_OUT_RE = re.compile(r"//[ \t]*OLO_PROFILING_INCLUDE_OK:")
# `OLO_PROFILE` and `OLO_FUNC_SIG` stay in the checked set on purpose: they are the
# SILENT half of the bug. An undefined name in `#if` is 0 with no diagnostic, so a
# header testing `#if OLO_PROFILE` without the include changes shape between TUs
# instead of failing to compile -- which is worse, not better. Both were real
# offenders when the set was widened (Task/InheritedContext.h, where the flag gates
# FInheritedContextScope's members, and Core/PerformanceProfiler.h).
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


def macro_names() -> set[str]:
    text = INSTRUMENTOR.read_text(encoding="utf-8", errors="replace")
    return set(DEFINE_RE.findall(text))


def _blank(match: re.Match[str]) -> str:
    """Replace a match with spaces, keeping newlines so offsets and line numbers hold."""
    return re.sub(r"[^\n]", " ", match.group(0))


def mask_comments(text: str) -> str:
    """Blank out comments in place. Same length as `text`, so offsets still map.

    Block comments first, then line comments: a `//` inside a block comment must not
    eat the block's terminator. The reverse case (`/*` inside a line comment) is not
    handled, and neither are the sequences inside string literals -- this is a
    textual ratchet, and neither shape occurs in these headers.
    """
    return LINE_COMMENT_RE.sub(_blank, BLOCK_COMMENT_RE.sub(_blank, text))


def logical_lines(text: str) -> list[tuple[int, int]]:
    """(start, end) offsets of logical lines, with backslash continuations joined."""
    spans: list[tuple[int, int]] = []
    start = pos = 0
    while pos < len(text):
        newline = text.find("\n", pos)
        if newline == -1:
            spans.append((start, len(text)))
            return spans
        if text[pos:newline].rstrip().endswith("\\"):
            pos = newline + 1  # continuation: same logical line
            continue
        spans.append((start, newline))
        pos = start = newline + 1
    if start < len(text):
        spans.append((start, len(text)))
    return spans


def main() -> int:
    if not INSTRUMENTOR.is_file():
        print(f"{INSTRUMENTOR} not found -- run from the repository root", file=sys.stderr)
        return 2

    names = macro_names()
    if not names:
        print(f"no OLO_* macros found in {INSTRUMENTOR} -- the check would pass vacuously", file=sys.stderr)
        return 2
    use_re = re.compile(r"\b(?:%s)\b" % "|".join(sorted(names)))

    missing: list[str] = []
    late: list[tuple[str, int, int]] = []
    for path in sorted(SRC_ROOT.rglob("*")):
        if path.suffix not in SUFFIXES or path.as_posix() in EXEMPT:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        # Comments name these macros constantly ("guarded by OLO_PROFILE_SCOPE"), so
        # blank them before looking for uses -- in place, to keep offsets usable below.
        masked = mask_comments(text)

        # Two kinds of use, and they answer different questions.
        #
        #   * An EFFECTIVE use expands here, so the include has to be in scope BEFORE it.
        #   * A use inside a `#define` body expands at the caller, wherever that is, so
        #     it obliges this header to own the include but says nothing about where.
        #
        # The defined name itself (the token right after `#define`) is neither.
        first_effective: int | None = None
        uses_at_all = False
        for start, end in logical_lines(masked):
            line = masked[start:end]
            define = DEFINE_RE.match(line)
            # Skip past the defined name so `#define OLO_PROFILE_SCOPE(x)` is not read
            # as a call to itself; a body after it still counts.
            hit = use_re.search(line, define.end(1) if define else 0)
            if not hit:
                continue
            uses_at_all = True
            if define:
                continue  # expands at the caller: obliges the include, not its position
            if first_effective is None:
                first_effective = start + hit.start()

        if not uses_at_all or OPT_OUT_RE.search(text):
            continue

        include = INCLUDE_RE.search(masked)
        if include is None:
            missing.append(path.as_posix())
        elif first_effective is not None and include.start() > first_effective:
            late.append(
                (
                    path.as_posix(),
                    masked.count("\n", 0, first_effective) + 1,
                    masked.count("\n", 0, include.start()) + 1,
                )
            )

    if not missing and not late:
        return 0

    if missing:
        print("Headers use a profiling macro without including the header that defines it:")
        for f in missing:
            print(f"  {f}")
        print()
    if late:
        print("Headers include the defining header only AFTER their first use of a macro:")
        for f, use_line, include_line in late:
            print(f"  {f}:{use_line} uses it; the include is at line {include_line}")
        print()
        print("Move the include above the first use -- an include below it is not in scope")
        print("there, so the file still does not compile on its own.")
        print()
    print('Add  #include "OloEngine/Debug/Instrumentor.h"  in the project-header block.')
    print("It is a leaf header (Core/Log.h + tracy), so there is no cycle risk.")
    print("A header that genuinely must not include it opts out with a line reading")
    print("  // OLO_PROFILING_INCLUDE_OK: <reason>")
    print("See issue #1071 and docs/agent-rules/pch-masked-missing-includes.md.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
