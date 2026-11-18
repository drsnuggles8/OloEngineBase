#!/usr/bin/env python3
"""Fail if a tracked AssetRegistry.oar registers a file under Assets/cache/.

Assets/cache/ is derived data and git-ignored (`**/cache/**`). The editor registers a
cache artifact as a side-effect of loading the model it was cooked from (an embedded
texture written out as a PNG, for one), and saves that into the project's registry,
which is tracked. Committed, the entry names a file a fresh clone does not have:
AssetContentValidity.SandboxAssetRegistryDeserialisesAndPathsResolve fails there and
passes on the machine that made the commit, because that machine has the cache.

That is how PR #1708 went red on Linux only, on all three sanitizer shards, with a
registry that had passed every local run: twelve cooked-texture entries picked up
while the editor built a game from the project.

The check reads the INDEX, not the working tree. An editor session leaves the working
copy with cache entries as a matter of course; what matters is what gets committed.

To clean a registry: restore it (`git checkout -- <path>`) if the session added
nothing worth keeping, or move Assets/cache/ aside and run
`OloEngine-Tests --gtest_also_run_disabled_tests
--gtest_filter=AssetContentValidity.DISABLED_RebaseAssetRegistry`, which drops the
entries whose files are gone and keeps every other handle.
"""

from __future__ import annotations

import re
import subprocess
import sys

REGISTRY_NAME = "AssetRegistry.oar"
# The registry stores project-relative paths as plain bytes inside a binary file.
CACHE_ENTRY = re.compile(rb"Assets[/\\]cache[/\\][A-Za-z0-9_./\\ +()-]{1,200}")


def tracked_registries() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], check=True, capture_output=True).stdout
    paths = out.decode("utf-8", errors="replace").split("\0")
    return [path for path in paths if path.rsplit("/", 1)[-1] == REGISTRY_NAME]


def staged_bytes(path: str) -> bytes | None:
    shown = subprocess.run(["git", "show", f":{path}"], capture_output=True)
    return shown.stdout if shown.returncode == 0 else None


def main() -> int:
    count = 0
    for path in tracked_registries():
        data = staged_bytes(path)
        if data is None:
            continue  # staged for deletion
        for entry in sorted({match.group(0).decode("ascii") for match in CACHE_ENTRY.finditer(data)}):
            count += 1
            print(f"{path}: registers {entry}, which is git-ignored cache and absent from a fresh clone")
    if count:
        print(
            f"\n{count} cache entr{'y' if count == 1 else 'ies'} in a tracked asset registry. "
            "Restore the registry or rebase it with Assets/cache/ moved aside "
            "(see the top of scripts/check_asset_registry_no_cache.py)."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
