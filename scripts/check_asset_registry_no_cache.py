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
# For a file that does not parse: every printable path-like run of bytes, each then judged by
# is_cache_artefact, the engine's rule (AssetRegistry::IsCacheArtefact) and .gitignore's
# `**/cache/**` / `**/[Cc][Aa][Cc][Hh][Ee]/**`: a directory named cache at any depth, any case.
PATH_RUN = re.compile(rb"[A-Za-z0-9_./\\ +()-]{3,400}")


def tracked_registries() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], check=True, capture_output=True).stdout
    paths = out.decode("utf-8", errors="replace").split("\0")
    return [path for path in paths if path.rsplit("/", 1)[-1] == REGISTRY_NAME]


def staged_bytes(path: str) -> bytes | None:
    shown = subprocess.run(["git", "show", f":{path}"], capture_output=True)
    return shown.stdout if shown.returncode == 0 else None


# AssetRegistry::FileVersion. Another version is not parsed: the engine rejects it anyway.
FILE_VERSION = 3


def entries_in_file_order(data: bytes) -> list[tuple[int, str]] | None:
    """The (handle, path) entries of a v3 registry in file order, or None for any other file."""
    try:
        version, count = struct.unpack_from("<II", data, 0)
        if version != FILE_VERSION:
            return None
        offset, entries = 8, []
        for _ in range(count):
            handle, _type, length = struct.unpack_from("<QII", data, offset)
            offset += 16
            entries.append((handle, data[offset : offset + length].decode("utf-8", errors="replace")))
            offset += length
        return entries if offset == len(data) else None
    except struct.error:
        return None


def is_cache_artefact(path: str) -> bool:
    """AssetRegistry::IsCacheArtefact: a directory (not the file name) called cache, any case."""
    return any(part.lower() == "cache" for part in re.split(r"[/\\]", path)[:-1])


def main() -> int:
    count = 0
    for path in tracked_registries():
        data = staged_bytes(path)
        if data is None:
            continue  # staged for deletion
        # AssetRegistry::Serialize writes entries in ascending handle order. A registry put
        # together by hand (a merge resolution) in any other order is rewritten by the first
        # editor session or test that saves it, so the checkout is dirty from then on.
        entries = entries_in_file_order(data)
        if entries is not None:
            handles = [handle for handle, _ in entries]
            if handles != sorted(handles):
                count += 1
                print(f"{path}: entries are not in ascending handle order, so the first save rewrites the file")
            cache_entries = sorted({entry for _, entry in entries if is_cache_artefact(entry)})
        else:
            runs = {match.group(0).decode("ascii") for match in PATH_RUN.finditer(data)}
            cache_entries = sorted(run for run in runs if is_cache_artefact(run))
        for entry in cache_entries:
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
