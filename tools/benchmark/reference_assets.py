#!/usr/bin/env python3
# =============================================================================
# reference_assets.py — the acquisition and verification path for every asset
# a ManifestVersion 2 benchmark manifest declares (issue #1239).
#
# THE RULE THIS TOOL ENFORCES
# ---------------------------
# A benchmark capture is only as trustworthy as the bytes it rendered. A
# manifest that says "Fox.gltf, CC0, upstream v2.0.1" is a claim; this tool is
# what makes it checkable. Every `Redistribution: committed` asset is hashed
# against the SHA-256 in the manifest, so an asset that silently changed — a
# re-export, a texture swap, a partial LFS checkout — fails loudly instead of
# quietly moving every number taken from that fixture.
#
#   python tools/benchmark/reference_assets.py               # verify (default)
#   python tools/benchmark/reference_assets.py --write-hashes
#   python tools/benchmark/reference_assets.py --fetch
#
# --write-hashes fills the Sha256 fields in from the files on disk. Run it when
# you deliberately change an asset, and review the resulting diff: a hash that
# moved without an intended asset change is the bug this whole mechanism is
# for.
#
# THE THREE REDISTRIBUTION CLASSES, AND WHY THE THIRD EXISTS
# ----------------------------------------------------------
#   committed       the licence permits redistribution; bytes are in-tree, and
#                   this tool hashes them.
#   fetch-required  freely obtainable but NOT redistributed here; --fetch
#                   downloads it to the declared path and verifies the hash.
#   local-only      the rights forbid redistribution entirely. The tool reports
#                   the gap and the fixture says so in its capture rather than
#                   substituting something else and calling the criterion met.
#                   This is the class a licensed AAA head falls into.
#
# Exit code is non-zero when a committed asset is missing or its bytes do not
# match. A local-only gap is REPORTED, never an error — it is a documented
# state of the world, not a broken checkout.
# =============================================================================

import argparse
import hashlib
import pathlib
import re
import sys

import yaml

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST_DIR = REPO_ROOT / "OloEditor" / "assets" / "benchmark" / "manifests"
# Manifest asset paths are relative to OloEditor/ — the working directory the
# editor and the capture tool both resolve assets against.
ASSET_ROOT = REPO_ROOT / "OloEditor"

OK = "ok"
MISSING = "missing"
MISMATCH = "mismatch"
NOT_FETCHED = "not-fetched"
LOCAL_GAP = "local-gap"


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def resolve_in_asset_root(rel):
    """`ASSET_ROOT / rel`, but only if the result stays inside ASSET_ROOT.

    `pathlib` join is not containment: an absolute `Path` on the right replaces
    the left entirely, and `../..` walks out. A manifest is repo-controlled, so
    this is not a live threat today — but every mode of this tool takes a
    manifest-supplied path and then reads, hashes, creates directories under,
    or REPLACES the file at it, and `--fetch` does that with bytes off the
    network. A path that leaves the asset root is a manifest bug in every case,
    so it is rejected once, here, rather than trusted three times.

    Returns None for a path that escapes; callers treat that as a failure.
    """
    candidate = pathlib.Path(rel)
    if candidate.is_absolute():
        return None
    resolved = (ASSET_ROOT / candidate).resolve()
    root = ASSET_ROOT.resolve()
    if resolved == root or root not in resolved.parents:
        return None
    return resolved


# Acquisition URLs are fetched, so the scheme is not a formality: `file:` would
# make --fetch copy a local file into the asset tree and "verify" it, and a
# plain-http or internal-host URL is a request the manifest author controls.
ALLOWED_FETCH_SCHEMES = ("https",)


def fetch_url_error(url):
    """None if `url` is safe to hand to urlretrieve, else the reason it is not."""
    from urllib.parse import urlparse

    try:
        parsed = urlparse(url)
    except ValueError as exc:
        return f"unparseable URL ({exc})"
    if parsed.scheme.lower() not in ALLOWED_FETCH_SCHEMES:
        return (f"scheme {parsed.scheme!r} is not permitted "
                f"(allowed: {', '.join(ALLOWED_FETCH_SCHEMES)})")
    if not parsed.netloc:
        return "no host in URL"
    return None


def load_manifests():
    """Every v2 manifest, as (path, parsed dict). v1 manifests carry no
    provenance contract and are skipped rather than warned about."""
    out = []
    for path in sorted(MANIFEST_DIR.glob("*.yaml")):
        try:
            data = yaml.safe_load(path.read_text(encoding="utf-8"))
        except yaml.YAMLError as exc:
            print(f"ERROR: {path.name}: {exc}", file=sys.stderr)
            out.append((path, None))
            continue
        if not isinstance(data, dict):
            continue
        if data.get("ManifestVersion") != 2:
            continue
        out.append((path, data))
    return out


def check_asset(record):
    """Return (status, detail) for one Assets[] record."""
    rel = record.get("Path", "")
    declared = record.get("Sha256", "")
    redistribution = record.get("Redistribution", "")
    target = resolve_in_asset_root(rel)
    if target is None:
        return MISMATCH, (f"Path escapes the asset root — refusing to read it. "
                          f"Asset paths are relative to OloEditor/ and must stay inside it.")

    if redistribution == "local-only":
        if target.exists():
            actual = sha256_of(target)
            if actual == declared:
                return OK, "local copy present and matching"
            # A local copy whose bytes differ from the recorded hash is exactly
            # what recording the hash is for: the fixture would render from
            # something other than what the manifest documents. Reporting it as
            # ok made the one class of asset nobody else can check the one
            # class that could never fail.
            return MISMATCH, (f"local copy present but does NOT match the recorded hash "
                              f"(declared {declared[:12]}…, actual {actual[:12]}…)")
        return LOCAL_GAP, "not present — rights forbid shipping it; see Acquisition"

    if not target.exists():
        return (NOT_FETCHED if redistribution == "fetch-required" else MISSING), str(target)

    actual = sha256_of(target)
    if actual != declared:
        return MISMATCH, f"declared {declared[:12]}… actual {actual[:12]}…"
    return OK, ""


def collect_records(manifests):
    """De-duplicate by (Path, Sha256): the same asset is declared by several
    fixtures and hashing newport_loft.hdr five times is wasted work. A path
    declared with two DIFFERENT hashes is itself a finding, so the key keeps
    both entries and the report shows the conflict."""
    seen = {}
    for path, data in manifests:
        if data is None:
            continue
        for record in data.get("Assets") or []:
            key = (record.get("Path", ""), record.get("Sha256", ""))
            seen.setdefault(key, (record, []))[1].append(path.name)
    return seen


def cmd_verify(args):
    manifests = load_manifests()
    if any(data is None for _, data in manifests):
        return 2
    if not manifests:
        print("no ManifestVersion 2 manifests found")
        return 0

    records = collect_records(manifests)
    by_path = {}
    for (rel, _), (record, users) in records.items():
        by_path.setdefault(rel, []).append((record, users))

    failures = 0
    gaps = 0
    print(f"verifying {len(records)} asset record(s) from {len(manifests)} v2 manifest(s)\n")
    for (rel, digest), (record, users) in sorted(records.items()):
        status, detail = check_asset(record)
        mark = {OK: "  ok  ", MISSING: " MISS ", MISMATCH: " DIFF ",
                NOT_FETCHED: " FETCH", LOCAL_GAP: " GAP  "}[status]
        print(f"[{mark}] {rel}")
        if detail:
            print(f"          {detail}")
        if status in (MISSING, MISMATCH):
            failures += 1
            print(f"          declared by: {', '.join(sorted(set(users)))}")
        elif status in (NOT_FETCHED, LOCAL_GAP):
            gaps += 1
            print(f"          acquisition: {record.get('Acquisition', '(none recorded)')}")

    # A path declared with two different hashes means two manifests disagree
    # about what they rendered — worth its own line, since each record on its
    # own can still verify.
    conflicts = 0
    for rel, entries in sorted(by_path.items()):
        if len(entries) > 1:
            conflicts += 1
            print(f"\nCONFLICT: {rel} is declared with {len(entries)} different hashes:")
            for record, users in entries:
                print(f"   {record.get('Sha256', '')[:16]}…  by {', '.join(sorted(set(users)))}")

    # Conflicts are counted apart from failures: a conflict is a property of a
    # PATH declared twice, not of a record, so folding it into `failures` made
    # the ok-count wrong — and, with enough conflicts, negative.
    print(f"\n{len(records) - failures - gaps} ok, {failures} failure(s), "
          f"{conflicts} conflict(s), {gaps} declared gap(s)")
    if gaps:
        print("declared gaps are not failures — they are assets this repository "
              "deliberately does not ship. See the Acquisition field.")
    return 1 if (failures or conflicts) else 0


def cmd_write_hashes(args):
    """Rewrite each v2 manifest's Sha256 lines from the files on disk.

    Line-oriented rather than a YAML round-trip on purpose: PyYAML's dumper
    would reflow every comment out of these manifests, and the comments carry
    the measured tolerances and the pose reasoning.
    """
    changed_files = 0
    for path, data in load_manifests():
        if data is None:
            return 2
        # Map Path -> fresh hash for every asset present on disk.
        fresh = {}
        for record in data.get("Assets") or []:
            rel = record.get("Path", "")
            target = resolve_in_asset_root(rel)
            if target is None:
                print(f"ERROR: {path.name}: asset Path {rel!r} escapes the asset root",
                      file=sys.stderr)
                return 2
            if target.exists():
                fresh[rel] = sha256_of(target)

        lines = path.read_text(encoding="utf-8").split("\n")
        current_path = None
        changed = 0
        for i, line in enumerate(lines):
            m = re.match(r"^(\s*)- Path:\s*(\S.*?)\s*$", line)
            if m:
                current_path = m.group(2)
                continue
            m = re.match(r"^(\s*)Sha256:\s*(\S*)\s*$", line)
            if m and current_path and current_path in fresh:
                want = fresh[current_path]
                if m.group(2) != want:
                    lines[i] = f"{m.group(1)}Sha256: {want}"
                    changed += 1
        if changed:
            path.write_text("\n".join(lines), encoding="utf-8", newline="\n")
            changed_files += 1
            print(f"{path.name}: updated {changed} hash(es)")
    if not changed_files:
        print("every Sha256 already matches the files on disk")
    return 0


def cmd_fetch(args):
    """Download every `fetch-required` asset that is not already in place.

    Deliberately a thin wrapper: it downloads to the DECLARED path and then
    verifies the declared hash, so a fetch that succeeds is a fetch that
    produced exactly the bytes the manifest was written against. A hash
    mismatch leaves nothing behind.
    """
    import urllib.request

    manifests = load_manifests()
    # A manifest that failed to parse must stop the fetch, not be skipped:
    # collect_records() ignores it, so --fetch would report success while
    # silently omitting every asset that manifest declares.
    if any(data is None for _, data in manifests):
        print("ERROR: at least one manifest failed to parse — refusing to fetch, because "
              "its assets would be silently omitted", file=sys.stderr)
        return 2

    records = collect_records(manifests)

    # Conflicting hashes must be caught BEFORE any download. `todo` is built up
    # front, so two records for one path with different hashes would both be
    # fetched, the second would overwrite the first, and the run would report
    # success for a file only one of them describes.
    by_path = {}
    for (rel, digest), (record, users) in records.items():
        by_path.setdefault(rel, set()).add(digest)
    conflicted = {rel for rel, digests in by_path.items() if len(digests) > 1}
    if conflicted:
        for rel in sorted(conflicted):
            print(f"ERROR: {rel} is declared with {len(by_path[rel])} different hashes — "
                  f"resolve the conflict before fetching", file=sys.stderr)
        return 1

    def pending(kind):
        out = []
        for (rel, _), (record, _) in sorted(records.items()):
            if record.get("Redistribution") != kind:
                continue
            target = resolve_in_asset_root(rel)
            if target is None:
                continue
            if not target.exists():
                out.append((rel, record, target))
        return out

    todo = pending("fetch-required")
    local_only = pending("local-only")

    if not todo:
        print("nothing to fetch — every fetch-required asset is already in place")
    for rel, record, target in todo:
        url = record.get("Acquisition", "")
        # The URL comes from the manifest and is about to be requested, so the
        # scheme and host are checked first: `file:` would copy a local file in
        # and "verify" it, and a non-HTTPS or internal host is a request the
        # manifest author chose, not this tool.
        if (why := fetch_url_error(url)) is not None:
            print(f"  FAILED: {rel}: refusing to fetch {url!r} — {why}", file=sys.stderr)
            return 1
        print(f"fetching {rel}\n  from {url}")
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp = target.with_suffix(target.suffix + ".part")
        try:
            urllib.request.urlretrieve(url, tmp)
        except Exception as exc:  # network, 404, TLS — all the same to the caller
            tmp.unlink(missing_ok=True)
            print(f"  FAILED: {exc}", file=sys.stderr)
            return 1
        actual = sha256_of(tmp)
        if actual != record.get("Sha256", ""):
            tmp.unlink(missing_ok=True)
            print(f"  FAILED: hash mismatch (got {actual[:16]}…) — nothing written", file=sys.stderr)
            return 1
        tmp.replace(target)
        print("  ok")

    for rel, record, _target in local_only:
        print(f"\nLOCAL-ONLY GAP: {rel}")
        print("  This repository may not redistribute it. To fill the gap yourself:")
        print(f"  {record.get('Acquisition', '(no procedure recorded)')}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[3] if __doc__ else None)
    ap.add_argument("--write-hashes", action="store_true",
                    help="rewrite Sha256 fields from the files on disk")
    ap.add_argument("--fetch", action="store_true",
                    help="download fetch-required assets to their declared paths")
    args = ap.parse_args()
    if args.write_hashes:
        return cmd_write_hashes(args)
    if args.fetch:
        return cmd_fetch(args)
    return cmd_verify(args)


if __name__ == "__main__":
    sys.exit(main())
