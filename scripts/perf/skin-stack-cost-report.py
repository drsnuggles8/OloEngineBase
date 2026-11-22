"""Turn a #1396 skin-stack cost campaign into its budget tables.

Reads the campaign directory `acceptance-benchmark.py --plan
scripts/perf/skin-stack-cost-plan.json` wrote, keeps only the runs it COUNTED
(quiet host, valid capture, pinned settings applied), and prints Markdown:

* per backend, path and camera, the per-pass GPU time of the skin stack:
  SkinDiffusionPass, its two snapshot copies, and the passes the skin shading
  rides (ScenePrepassPass, ScenePass, DeferredLightingPass), as the range over
  counted runs of each run's p50;
* the arm deltas, each against its own control: full stack minus diffusion-off
  (what the diffusion costs the frame), full stack minus skin-off per pass (what
  the skin shading costs the G-Buffer and lighting), and full stack minus its
  identical repeat (the noise floor any delta has to clear).

A delta is the difference of the arms' medians over runs; its spread is the
larger of the two arms' run ranges. A delta whose spread is not smaller than the
delta itself is printed as "not resolved", never as a number to budget from.

    python scripts/perf/skin-stack-cost-report.py <campaign-dir>
"""

from __future__ import annotations

import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

SKIN_PASSES = ("SkinDiffusionPass", "DiffusionHandoffSnapshotPass", "DiffusionDepthSnapshotPass")
RIDES = ("ScenePrepassPass", "ScenePass", "DeferredLightingPass")
PATHS = (("forward", "Forward"), ("forward-plus", "Forward+"), ("deferred", "Deferred"))
BACKENDS = (("gl", "OpenGL"), ("vk", "Vulkan"))


def finite_ms(value) -> float | None:
    try:
        ms = float(value)
    except (TypeError, ValueError):
        return None
    return ms if ms == ms and ms not in (float("inf"), float("-inf")) and ms >= 0.0 else None


def frame_id(value) -> int | None:
    try:
        fid = int(value)
    except (TypeError, ValueError):
        return None
    return fid if fid > 0 else None


def run_p50s(directory: Path) -> dict:
    """{camera: {"gpu": p50, pass: p50}} for one capture directory, with the campaign
    driver's exclusions: valid status, a real GPU frame id, a finite value, each frame
    (and each (frame, pass)) once, sub-passes never counted as passes."""
    out: dict[str, dict[str, float]] = defaultdict(dict)
    frames: dict[str, list[float]] = defaultdict(list)
    seen: set = set()
    with (directory / "measurement.csv").open(encoding="utf-8") as f:
        for row in csv.DictReader(f):
            fid, ms = frame_id(row.get("gpuFrameId")), finite_ms(row.get("gpuMs"))
            if row.get("gpuStatus") != "valid" or fid is None or ms is None or fid in seen:
                continue
            seen.add(fid)
            frames[row.get("camera", "")].append(ms)
    for camera, values in frames.items():
        out[camera]["gpu"] = statistics.median(values)
    passes: dict[tuple, list[float]] = defaultdict(list)
    seen = set()
    for line in (directory / "measurement-passes.jsonl").open(encoding="utf-8"):
        if not line.strip():
            continue
        record = json.loads(line)
        fid = frame_id(record.get("gpuFrameId"))
        for p in record.get("passes") or []:
            ms = finite_ms(p.get("gpuMs"))
            key = (fid, p.get("pass"))
            if p.get("status") != "valid" or p.get("isSubPass", True) or fid is None or ms is None or key in seen:
                continue
            seen.add(key)
            passes[(record.get("camera", ""), p["pass"])].append(ms)
    for (camera, name), values in passes.items():
        out[camera][name] = statistics.median(values)
    return out


def load(campaign: Path) -> dict:
    """{cell: [per-run p50s]} over counted runs only."""
    runs = json.loads((campaign / "runs.json").read_text(encoding="utf-8"))
    cells: dict[str, list] = defaultdict(list)
    for record in runs:
        if record.get("status") == "counted":
            cells[record["cell"]].append(run_p50s(campaign / record["directory"]))  # relative to the campaign
    return cells


def values(cell_runs: list, camera: str, name: str) -> list[float]:
    return [r[camera][name] for r in cell_runs if name in r.get(camera, {})]


def rng(v: list[float]) -> str:
    return "—" if not v else (f"{min(v):.3f}" if min(v) == max(v) else f"{min(v):.3f}–{max(v):.3f}")


def delta(a: list[float], b: list[float], control: list[float] | None = None, judged: bool = True) -> str:
    """A − B, judged against its noise floor: the largest of the two arms' run ranges and,
    when `control` (the identical repeat of arm A) is given, its own run range and |A − control|,
    the difference the same workload shows between two cells. `judged=False` prints the
    identical-arms control itself, whose delta SHOULD sit inside its spread."""
    if not a or not b:
        return "—"
    if min(len(a), len(b)) < 2:
        # One run has no spread to judge against; never print it as a budget number.
        return f"insufficient runs ({len(a)}/{len(b)})"
    d = statistics.median(a) - statistics.median(b)
    floor = max(max(a) - min(a), max(b) - min(b))
    if control:
        floor = max(floor, abs(statistics.median(a) - statistics.median(control)), max(control) - min(control))
    verdict = "" if not judged or floor < abs(d) else " (not resolved)"
    return f"{d:+.3f} ± {floor:.3f}{verdict}"


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")  # the tables use − and –; a Windows console is cp1252
    campaign = Path(sys.argv[1])
    cells = load(campaign)
    print(f"Counted runs per cell: " + ", ".join(f"{c} {len(r)}" for c, r in sorted(cells.items())) + "\n")
    for prefix, backend in BACKENDS:
        print(f"### {backend}\n")
        print("| path | camera | SkinDiffusionPass | snapshot copies | frame GPU | diffusion: on − off | "
              "identical arms: on − repeat |")
        print("|---|---|---:|---:|---:|---:|---:|")
        for slug, label in PATHS:
            on, rep, off = (cells.get(f"{prefix}-{slug}{s}", []) for s in ("", "-repeat", "-diffusion-off"))
            for camera in ("face-fill", "crowd"):
                skin = values(on, camera, "SkinDiffusionPass")
                # Summed within each run; a run missing either copy contributes nothing.
                copies = [r[camera][SKIN_PASSES[1]] + r[camera][SKIN_PASSES[2]] for r in on
                          if SKIN_PASSES[1] in r.get(camera, {}) and SKIN_PASSES[2] in r.get(camera, {})]
                print(f"| {label} | {camera} | {rng(skin)} | {rng(copies)} | {rng(values(on, camera, 'gpu'))} | "
                      f"{delta(values(on, camera, 'gpu'), values(off, camera, 'gpu'), values(rep, camera, 'gpu'))} | "
                      f"{delta(values(on, camera, 'gpu'), values(rep, camera, 'gpu'), judged=False)} |")
        print()
        print("Skin shading in the passes it rides (full stack − skin-off, ms; per-pass p50):\n")
        print("| path | camera | " + " | ".join(RIDES) + " |")
        print("|---|---|" + "---:|" * len(RIDES))
        for slug, label in PATHS:
            on, bare = cells.get(f"{prefix}-{slug}", []), cells.get(f"{prefix}-{slug}-skin-off", [])
            rep = cells.get(f"{prefix}-{slug}-repeat", [])
            for camera in ("face-fill", "crowd"):
                print(f"| {label} | {camera} | " + " | ".join(
                    delta(values(on, camera, p), values(bare, camera, p), values(rep, camera, p)) for p in RIDES) + " |")
        up = cells.get(f"{prefix}-deferred-upscale-quality", [])
        if up:
            print("\nDeferred, upscale Quality: SkinDiffusionPass " + ", ".join(
                f"{c} {rng(values(up, c, 'SkinDiffusionPass'))}" for c in ("face-fill", "crowd")) + " ms\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
