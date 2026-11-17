#!/usr/bin/env python3
"""Fail if a source file includes an stb header by its bare name.

Two copies of stb are on the include path. The pinned one is staged under a
directory, <stb_image/stb_image_write.h>, and its implementation is compiled into
OloEngine. vcpkg installs an older one flat, <stb_image_write.h>, as a dependency of
assimp. A file that includes the bare name compiles against the older declarations
and links against the pinned implementation.

That is silent until a signature changes. The pin of 2026-11-16 made the stride of
stbi_write_png a ptrdiff_t where the older header declares an int, so a caller that
included the bare name passed four bytes of an eight-byte stack argument on Windows
and the writer read whatever lay above them: an access violation in 26 tests, or a
skewed image where the stray bytes happened to be small.

The rule covers every stb header, not only the three that are staged: a bare
<stb_*.h> can only ever be the copy this project does not pin.

Opt out on the include line itself with:

    #include <stb_foo.h> // OLO_STB_BARE_INCLUDE_OK: <reason>
"""

from __future__ import annotations

import re
import subprocess
import sys

SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inl")
BARE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"](stb_[A-Za-z0-9_]+\.h)[>"]')
OPT_OUT = "OLO_STB_BARE_INCLUDE_OK:"


def tracked_sources() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z"], check=True, capture_output=True
    ).stdout.decode("utf-8", errors="replace")
    return [
        path
        for path in out.split("\0")
        if path.endswith(SUFFIXES) and "/vendor/" not in f"/{path}"
    ]


def findings(path: str) -> list[tuple[int, str]]:
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            lines = handle.readlines()
    except OSError:
        return []  # deleted in the working tree, still in the index
    found = []
    for number, line in enumerate(lines, start=1):
        match = BARE_RE.match(line)
        if match and OPT_OUT not in line:
            found.append((number, match.group(1)))
    return found


def main() -> int:
    sources = tracked_sources()
    count = 0
    for path in sources:
        for number, header in findings(path):
            count += 1
            print(
                f"{path}:{number}: includes <{header}> by its bare name, which is vcpkg's "
                f"older copy; include <stb_image/{header}>"
            )
    if count:
        print(
            f"\n{count} bare stb include(s) in {len(sources)} files. "
            "See docs/agent-rules/two-copies-of-a-header-on-the-include-path.md."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
