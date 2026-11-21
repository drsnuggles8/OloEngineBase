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


def run_p50s(directory: Path) -> dict:
    """{camera: {"gpu": p50, pass: p50}} for one capture directory."""
    out: dict[str, dict[str, float]] = defaultdict(dict)
    frames: dict[str, list[float]] = defaultdict(list)
    seen: set = set()
    with (directory / "measurement.csv").open(encoding="utf-8") as f:
        for row in csv.DictReader(f):
            key = (row["camera"], row["gpuFrameId"])
            if row["gpuStatus"] != "valid" or key in seen:
                continue
            seen.add(key)
            frames[row["camera"]].append(float(row["gpuMs"]))
    for camera, values in frames.items():
        out[camera]["gpu"] = statistics.median(values)
    passes: dict[tuple, list[float]] = defaultdict(list)
    seen = set()
    for line in (directory / "measurement-passes.jsonl").open(encoding="utf-8"):
        record = json.loads(line)
        for p in record["passes"]:
            key = (record["camera"], record["gpuFrameId"], p["pass"])
            if p["status"] != "valid" or p["isSubPass"] or key in seen:
                continue
            seen.add(key)
            passes[(record["camera"], p["pass"])].append(p["gpuMs"])
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


def delta(a: list[float], b: list[float], judged: bool = True) -> str:
    """`judged=False` for the identical-arms control, whose delta SHOULD sit inside its spread."""
    if not a or not b:
        return "—"
    d = statistics.median(a) - statistics.median(b)
    spread = max(max(a) - min(a), max(b) - min(b))
    verdict = "" if not judged or spread < abs(d) else " (not resolved)"
    return f"{d:+.3f} ± {spread:.3f}{verdict}"


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
                copies = [a + b for a, b in zip(values(on, camera, SKIN_PASSES[1]), values(on, camera, SKIN_PASSES[2]))]
                print(f"| {label} | {camera} | {rng(skin)} | {rng(copies)} | {rng(values(on, camera, 'gpu'))} | "
                      f"{delta(values(on, camera, 'gpu'), values(off, camera, 'gpu'))} | "
                      f"{delta(values(on, camera, 'gpu'), values(rep, camera, 'gpu'), judged=False)} |")
        print()
        print("Skin shading in the passes it rides (full stack − skin-off, ms; per-pass p50):\n")
        print("| path | camera | " + " | ".join(RIDES) + " |")
        print("|---|---|" + "---:|" * len(RIDES))
        for slug, label in PATHS:
            on, bare = cells.get(f"{prefix}-{slug}", []), cells.get(f"{prefix}-{slug}-skin-off", [])
            for camera in ("face-fill", "crowd"):
                print(f"| {label} | {camera} | " + " | ".join(delta(values(on, camera, p), values(bare, camera, p))
                                                         for p in RIDES) + " |")
        up = cells.get(f"{prefix}-deferred-upscale-quality", [])
        if up:
            print("\nDeferred, upscale Quality: SkinDiffusionPass " + ", ".join(
                f"{c} {rng(values(up, c, 'SkinDiffusionPass'))}" for c in ("face-fill", "crowd")) + " ms\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
