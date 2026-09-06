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


def main() -> int:
    if not INSTRUMENTOR.is_file():
        print(f"{INSTRUMENTOR} not found -- run from the repository root", file=sys.stderr)
        return 2

    names = macro_names()
    if not names:
        print(f"no OLO_* macros found in {INSTRUMENTOR} -- the check would pass vacuously", file=sys.stderr)
        return 2
    use_re = re.compile(r"\b(?:%s)\b" % "|".join(sorted(names)))

    offenders: list[str] = []
    for path in sorted(SRC_ROOT.rglob("*")):
        if path.suffix not in SUFFIXES or path.as_posix() in EXEMPT:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        # Comments name these macros constantly ("guarded by OLO_PROFILE_SCOPE"), and a
        # `#define OLO_PROFILE_SCOPE(...)` names the macro rather than calling it. Drop
        # both before looking for call sites; whatever is left on a #define line -- a
        # macro body that expands to one -- still counts as a use.
        body = BLOCK_COMMENT_RE.sub(" ", LINE_COMMENT_RE.sub("", text))
        body = "\n".join(DEFINE_RE.sub("", line, count=1) for line in body.splitlines())
        if not use_re.search(body):
            continue
        if INCLUDE_RE.search(text) or OPT_OUT_RE.search(text):
            continue
        offenders.append(path.as_posix())

    if not offenders:
        return 0

    print("Headers use a profiling macro without including the header that defines it:")
    for f in offenders:
        print(f"  {f}")
    print()
    print('Add  #include "OloEngine/Debug/Instrumentor.h"  to each, in the project-header')
    print("block. It is a leaf header (Core/Log.h + tracy), so there is no cycle risk.")
    print("A header that genuinely must not include it opts out with a line reading")
    print("  // OLO_PROFILING_INCLUDE_OK: <reason>")
    print("See issue #1071 and docs/agent-rules/pch-masked-missing-includes.md.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
