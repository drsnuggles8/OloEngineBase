#!/usr/bin/env python3
"""Regenerate the checked-in engine PSO seed, OloEditor/assets/PipelineManifest.yaml (issue #1650).

The seed lists the complete Vulkan pipeline keys the engine's own scenes draw, so a first session on
a machine (empty OLO_SHADER_CACHE_DIR, no machine manifest) requests them before its first frame.
EnginePipelineSeedTest keeps it current: it fails, naming the missing keys and this script, when a
cold session that starts from the seed draws a pipeline the seed does not request.

What this does:
  1. runs `OloEngine-Tests --olo-rhi=vulkan --gtest_filter=EnginePipelineSeedWriter.WritesTheSeed`
     with an empty temporary OLO_SHADER_CACHE_DIR. That case renders the #1259 acceptance scene
     cold on Forward, Forward+ and Deferred, under CSM and VSM, through the SAME helper the guard
     test renders with, and writes every key the draws used;
  2. checks the case really wrote the file (a skipped case also exits 0);
  3. writes it over the seed with LF line endings, normalized (sorted, no machine sessions), and
     prints how many keys were added and removed, so the diff is easy to review.

`--check` regenerates into a temporary file and exits 1 if it differs from the checked-in seed,
without touching it.

Needs a Vulkan device (any: the keys are VkFormat values and sample counts fixed by the engine's
framebuffer specs, not by the GPU, and EnginePipelineSeedTest proves the seed on lavapipe in CI).
Build the test binary first, e.g.

    pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command \
      'cmake --build build-cached --target OloEngine-Tests --config Debug --parallel 6'
    python scripts/regenerate-pipeline-seed.py
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SEED = REPO / "OloEditor" / "assets" / "PipelineManifest.yaml"
CASE = "EnginePipelineSeedWriter.WritesTheSeed"
EXE_NAME = "OloEngine-Tests.exe" if os.name == "nt" else "OloEngine-Tests"
CANDIDATES = [
    REPO / "build-cached" / "OloEngine" / "tests" / "Debug" / EXE_NAME,
    REPO / "build-cached" / "OloEngine" / "tests" / "Release" / EXE_NAME,
    REPO / "build" / "OloEngine" / "tests" / "Debug" / EXE_NAME,
    REPO / "build" / "OloEngine" / "tests" / "Release" / EXE_NAME,
    REPO / "build-clang" / "OloEngine" / "tests" / "Debug" / EXE_NAME,
    REPO / "build" / "OloEngine" / "tests" / EXE_NAME,
]
ENTRY = re.compile(r"^[ \t]*- Shader: ", re.MULTILINE)


def find_exe(explicit: str | None) -> Path:
    if explicit:
        exe = Path(explicit)
        if not exe.is_file():
            sys.exit(f"error: no test binary at {exe}")
        return exe
    found = [c for c in CANDIDATES if c.is_file()]
    if not found:
        sys.exit("error: no OloEngine-Tests binary found; build it or pass --exe. Looked in:\n  "
                 + "\n  ".join(str(c) for c in CANDIDATES))
    # The newest build is the one that matches the working tree best.
    return max(found, key=lambda p: p.stat().st_mtime)


def entries(text: str) -> list[str]:
    """The seed's entries as text blocks (one per key), for an added/removed count."""
    starts = [m.start() for m in ENTRY.finditer(text)]
    return [text[a:b] for a, b in zip(starts, starts[1:] + [len(text)])]


def regenerate(exe: Path, out: Path, verbose: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="olo-pso-seed-cache-") as cache:
        env = dict(os.environ)
        env["OLO_SHADER_CACHE_DIR"] = cache  # a first session: nothing cached, no machine manifest
        env["OLO_PIPELINE_SEED_OUT"] = str(out)
        cmd = [str(exe), "--olo-rhi=vulkan", "--olo-require-vulkan", f"--gtest_filter={CASE}"]
        print(f"running: {' '.join(cmd)}\n  OLO_SHADER_CACHE_DIR={cache}", flush=True)
        proc = subprocess.run(cmd, cwd=REPO, env=env, capture_output=True, text=True, errors="replace")
    output = proc.stdout + proc.stderr
    if verbose:
        print(output)
    else:
        for line in output.splitlines():
            if line.startswith("[pipeline-seed]") or "[  FAILED" in line or "error" in line.lower()[:40]:
                print("  " + line)
    if proc.returncode != 0:
        sys.exit(f"error: {CASE} failed (exit {proc.returncode}); rerun with --verbose for the full log")
    if "[pipeline-seed] wrote" not in output or not out.is_file():
        sys.exit(f"error: {CASE} did not write the seed (skipped? a build without the case?); "
                 "rerun with --verbose")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--exe", help="the OloEngine-Tests binary (default: the newest build found)")
    parser.add_argument("--check", action="store_true",
                        help="regenerate to a temporary file and fail if it differs from the checked-in seed")
    parser.add_argument("--verbose", action="store_true", help="print the test process's whole output")
    args = parser.parse_args()

    exe = find_exe(args.exe)
    with tempfile.TemporaryDirectory(prefix="olo-pso-seed-") as work:
        fresh = Path(work) / "PipelineManifest.yaml"
        regenerate(exe, fresh, args.verbose)
        # Normalize line endings: the test writes LF; this guards a platform that would not.
        text = fresh.read_bytes().decode("utf-8").replace("\r\n", "\n")

    old = SEED.read_bytes().decode("utf-8").replace("\r\n", "\n") if SEED.is_file() else ""
    old_entries, new_entries = set(entries(old)), set(entries(text))
    added, removed = len(new_entries - old_entries), len(old_entries - new_entries)
    print(f"seed: {len(new_entries)} key(s); {added} added, {removed} removed or changed "
          f"(was {len(old_entries)})")

    if args.check:
        if text != old:
            print(f"error: {SEED.relative_to(REPO)} is out of date; run without --check to rewrite it")
            return 1
        print(f"{SEED.relative_to(REPO)} is current")
        return 0

    if text == old:
        print(f"{SEED.relative_to(REPO)} unchanged")
        return 0
    with open(SEED, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"wrote {SEED.relative_to(REPO)}: review the diff and commit it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
