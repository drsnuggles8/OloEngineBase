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

# Instrumentor.h *defines* the macros, so it is not a client of itself.
EXEMPT = {INSTRUMENTOR.as_posix()}

# Configuration flags rather than call sites: a header may legitimately test
# `#if OLO_PROFILE` or paste OLO_FUNC_SIG into an unrelated log line.
NOT_CALL_SITES = {"OLO_PROFILE", "OLO_FUNC_SIG"}

DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(OLO_[A-Z0-9_]+)", re.MULTILINE)
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"OloEngine/Debug/Instrumentor\.h"[ \t]*$', re.MULTILINE)
OPT_OUT_RE = re.compile(r"//[ \t]*OLO_PROFILING_INCLUDE_OK:")


def macro_names() -> set[str]:
    text = INSTRUMENTOR.read_text(encoding="utf-8", errors="replace")
    return {m for m in DEFINE_RE.findall(text) if m not in NOT_CALL_SITES}


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
        # `#define OLO_PROFILE_SCOPE(...)` names the macro, it does not call it, so drop
        # that prefix before looking for call sites. Whatever is left on the line -- a
        # macro body that expands to one -- still counts as a use.
        body = "\n".join(DEFINE_RE.sub("", line, count=1) for line in text.splitlines())
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
