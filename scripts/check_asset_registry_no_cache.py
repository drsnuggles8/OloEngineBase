#!/usr/bin/env python3
"""Fail if a tracked AssetRegistry.oar registers a file under Assets/cache/.

Assets/cache/ is derived data and git-ignored (`**/cache/**`). The editor registers a
cache artifact as a side-effect of loading the model it was cooked from (an embedded
texture written out as a PNG, for one). Committed to the tracked registry, the entry
names a file a fresh clone does not have:
AssetContentValidity.SandboxAssetRegistryDeserialisesAndPathsResolve fails there and
passes on the machine that made the commit, because that machine has the cache.
That is how PR #1708 went red on Linux only, on all three sanitizer shards.

Since #1664 the engine never writes such an entry: AssetRegistry::Serialize leaves
cache artefacts out (their handle is derived from their path), and Deserialize drops
one it finds. This hook is the second line: it still catches a registry written by a
build from before that change, or edited by hand.

The check reads the INDEX, not the working tree; what matters is what gets committed.

To clean a registry: restore it (`git checkout -- <path>`), or open the project in a
current editor, which drops the cache entries on load and leaves them out on save.
"""

from __future__ import annotations

import re
import struct
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


# AssetRegistry::FileVersion. Another version is not parsed: the engine rejects it anyway.
FILE_VERSION = 3


def handles_in_file_order(data: bytes) -> list[int] | None:
    """The entry handles of a v3 registry in file order, or None for any other file."""
    try:
        version, count = struct.unpack_from("<II", data, 0)
        if version != FILE_VERSION:
            return None
        offset, handles = 8, []
        for _ in range(count):
            handle, _type, length = struct.unpack_from("<QII", data, offset)
            offset += 16 + length
            handles.append(handle)
        return handles if offset == len(data) else None
    except struct.error:
        return None


def main() -> int:
    count = 0
    for path in tracked_registries():
        data = staged_bytes(path)
        if data is None:
            continue  # staged for deletion
        # AssetRegistry::Serialize writes entries in ascending handle order. A registry put
        # together by hand (a merge resolution) in any other order is rewritten by the first
        # editor session or test that saves it, so the checkout is dirty from then on.
        handles = handles_in_file_order(data)
        if handles is not None and handles != sorted(handles):
            count += 1
            print(f"{path}: entries are not in ascending handle order, so the first save rewrites the file")
        for entry in sorted({match.group(0).decode("ascii") for match in CACHE_ENTRY.finditer(data)}):
            count += 1
            print(f"{path}: registers {entry}, which is git-ignored cache and absent from a fresh clone")
    if count:
        print(
            f"\n{count} problem{'' if count == 1 else 's'} in a tracked asset registry. "
            "Restore the registry, or open the project in a current editor (or run any test that "
            "saves it) and commit the file it writes; see the top of "
            "scripts/check_asset_registry_no_cache.py."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
