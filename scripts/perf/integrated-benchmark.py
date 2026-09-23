"""Run fresh-process GL captures and summarize independent integrated runs.

GPU samples retain their statuses and frame IDs in each unmodified raw CSV.
Uncertainty is the range of run-level statistics, not a bootstrap of correlated
consecutive frames. Vulkan runs are produced by olo_benchmark_capture in a live
editor and passed here with --run-dir after capture.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import platform
import subprocess
import sys
import time
from pathlib import Path


def percentile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(q * len(ordered)) - 1)]


def describe(values: list[float], deadline: float) -> dict:
    if not values:
        raise ValueError("no frame measurements")
    return {
        "frames": len(values),
        "p50Ms": percentile(values, 0.50),
        "p95Ms": percentile(values, 0.95),
        "p99Ms": percentile(values, 0.99),
        "maxMs": max(values),
        "deadlineMisses": sum(value > deadline for value in values),
    }


def load_run(path: Path) -> tuple[dict, dict[str, dict[str, list[float]]], dict]:
    result = json.loads((path / "result.json").read_text(encoding="utf-8"))
    measurement = result.get("measurement") or {}
    deadline = measurement.get("deadlineMs")
    if not isinstance(deadline, (int, float)) or deadline <= 0:
        raise ValueError(f"{path}: no valid measurement deadline")
    grouped: dict[str, dict[str, list[float]]] = {}
    gpu_ids: set[str] = set()
    gpu_statuses: dict[str, int] = {}
    with (path / measurement["rawFile"]).open(newline="", encoding="utf-8") as file:
        for row in csv.DictReader(file):
            camera = row["camera"]
            sample = grouped.setdefault(camera, {key: [] for key in
                                                 ("renderCallMs", "cpuMs", "fenceWaitMs", "presentWaitMs", "gpuMs")})
            for key in ("renderCallMs", "cpuMs", "fenceWaitMs", "presentWaitMs"):
                sample[key].append(float(row[key]))
            status = row["gpuStatus"]
            gpu_statuses[status] = gpu_statuses.get(status, 0) + 1
            frame_id = row["gpuFrameId"]
            if status == "valid" and row["gpuMs"] and frame_id and frame_id not in gpu_ids:
                sample["gpuMs"].append(float(row["gpuMs"]))
                gpu_ids.add(frame_id)
    for camera, values in grouped.items():
        if len(values["renderCallMs"]) < 100:
            raise ValueError(f"{path}: {camera} has only {len(values['renderCallMs'])} measured frames; require >=100")
    if result["determinism"].get("warmupTimedOut"):
        raise ValueError(f"{path}: warm-up or measurement timed out")
    return result, grouped, gpu_statuses


def run_gl(args: argparse.Namespace) -> list[Path]:
    if not args.exe or not args.manifest:
        raise ValueError("--exe and --manifest are required with --capture-gl")
    paths = []
    exe = args.exe.resolve()
    manifest = args.manifest.resolve()
    output = args.output.resolve()
    gpu_query = ["nvidia-smi", "--query-gpu=clocks.gr,utilization.gpu,memory.used,temperature.gpu,power.draw",
                 "--format=csv,noheader"]
    for index in range(1, args.repeats + 1):
        destination = output / f"run-{index:02d}"
        if destination.exists() and any(destination.iterdir()):
            raise ValueError(f"run directory is not empty; choose a fresh output: {destination}")
        destination.mkdir(parents=True, exist_ok=True)
        environment = os.environ.copy()
        cache_dir = None
        if args.cache_mode == "cold":
            cache_dir = destination / "shader-cache"
            if cache_dir.exists() and any(cache_dir.iterdir()):
                raise ValueError(f"cold shader cache is not empty: {cache_dir}")
            cache_dir.mkdir(parents=True, exist_ok=True)
        elif args.cache_mode == "warm":
            if args.shader_cache_dir is None:
                raise ValueError("--cache-mode warm requires --shader-cache-dir")
            cache_dir = args.shader_cache_dir.resolve()
            cache_dir.mkdir(parents=True, exist_ok=True)
        elif args.shader_cache_dir is not None:
            cache_dir = args.shader_cache_dir.resolve()
        cache_entries_before = sum(1 for item in cache_dir.rglob("*") if item.is_file()) if cache_dir else None
        if args.cache_mode == "warm" and cache_entries_before == 0:
            raise ValueError(f"warm shader cache is empty; prime it before measurement: {cache_dir}")
        if cache_dir:
            environment["OLO_SHADER_CACHE_DIR"] = str(cache_dir)
        command = [str(exe), f"--olo-capture-manifest={manifest}",
                   f"--olo-capture-out={destination}"]
        gpu_before = command_probe(gpu_query)
        builds_before = system_probe("(Get-Process ninja,clang-cl,MSBuild -ErrorAction SilentlyContinue | Measure-Object).Count")
        start = time.perf_counter()
        completed = subprocess.run(command, cwd=args.repo.resolve(), env=environment,
                                   capture_output=True, text=True)
        elapsed_ms = (time.perf_counter() - start) * 1000
        gpu_after = command_probe(gpu_query)
        builds_after = system_probe("(Get-Process ninja,clang-cl,MSBuild -ErrorAction SilentlyContinue | Measure-Object).Count")
        (destination / "host.json").write_text(json.dumps({
            "command": command, "captureProcessElapsedMs": elapsed_ms,
            "exitCode": completed.returncode, "os": platform.platform(),
            "python": platform.python_version(), "machine": platform.machine(),
            "buildConfig": args.build_config, "compiler": args.compiler,
            "presentation": "offscreen", "vsync": "not-applicable",
            "cpu": system_probe("(Get-CimInstance Win32_Processor | Select-Object -First 1).Name"),
            "physicalMemoryBytes": system_probe("(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory"),
            "gpuDriver": command_probe(["nvidia-smi", "--query-gpu=driver_version,name,memory.total", "--format=csv,noheader"]),
            "gpuTelemetryColumns": "clocks.gr MHz, utilization.gpu %, memory.used MiB, temperature.gpu C, power.draw W",
            "gpuTelemetryBefore": gpu_before, "gpuTelemetryAfter": gpu_after,
            "concurrentBuildProcessesBefore": builds_before,
            "concurrentBuildProcessesAfter": builds_after,
            "shaderCacheMode": args.cache_mode,
            "shaderCacheDir": str(cache_dir) if cache_dir else environment.get("OLO_SHADER_CACHE_DIR", "inherited/default"),
            "shaderCacheEntriesBefore": cache_entries_before,
            "shaderCacheEntriesAfter": (sum(1 for item in cache_dir.rglob("*") if item.is_file())
                                        if cache_dir else None),
        }, indent=2), encoding="utf-8")
        (destination / "stdout.txt").write_text(completed.stdout, encoding="utf-8")
        (destination / "stderr.txt").write_text(completed.stderr, encoding="utf-8")
        if completed.returncode or not (destination / "result.json").exists():
            raise RuntimeError(f"capture {index} failed: exit {completed.returncode}; see {destination}")
        paths.append(destination)
    return paths


def command_probe(command: list[str]) -> str:
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        return result.stdout.strip() if result.returncode == 0 else "unavailable"
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"


def system_probe(expression: str) -> str:
    return command_probe(["powershell", "-NoProfile", "-Command", expression])


def analyze(paths: list[Path], output: Path, environment_json: Path | None = None) -> None:
    if len(paths) < 3:
        raise ValueError("at least three independent result directories are required")
    if len({path.resolve() for path in paths}) != len(paths):
        raise ValueError("run directories must be distinct")
    runs = [load_run(path) for path in paths]
    first = runs[0][0]
    key = (first["manifest"]["sourceHashFnv1a64"], first["provenance"]["backend"],
           first["output"]["actual"])
    for result, _, _ in runs[1:]:
        other = (result["manifest"]["sourceHashFnv1a64"], result["provenance"]["backend"],
                 result["output"]["actual"])
        if other != key:
            raise ValueError("manifest hash, backend or actual resolution differs across runs")
    cameras = set(runs[0][1])
    if any(set(grouped) != cameras for _, grouped, _ in runs):
        raise ValueError("camera sets differ across runs")
    deadline = first["measurement"]["deadlineMs"]
    summary = {"manifest": first["manifest"], "provenance": first["provenance"],
               "actualOutput": first["output"]["actual"], "deadlineMs": deadline,
               "runs": [str(path) for path in paths], "scenarios": {},
               "uncertaintyMethod": "min/max of independent run-level statistics; frames within a run are correlated",
               "gpuStatusCountsByRun": [statuses for _, _, statuses in runs],
               "memoryScope": first["measurement"].get("memoryScope", "unknown")}
    memory_keys = ("peakTrackedRendererBytes", "liveTrackedRendererBytes",
                   "trackedRendererBytesAfterSceneRelease")
    summary["trackedMemoryBytes"] = {}
    for memory_key in memory_keys:
        values = [result["measurement"].get(memory_key) for result, _, _ in runs]
        summary["trackedMemoryBytes"][memory_key] = (
            {"perRun": values, "runLevelRange": [min(values), max(values)]}
            if all(value is not None for value in values) else None
        )
    summary["environment"] = ({"presentation": "offscreen", "vsync": "not-applicable"}
                              if first["provenance"]["host"] == "test-binary"
                              else {"presentation": "unknown", "vsync": "unknown"})
    if environment_json:
        summary["environment"].update(json.loads(environment_json.read_text(encoding="utf-8")))
    for camera in sorted(cameras):
        per_run = []
        for _, grouped, _ in runs:
            samples = grouped[camera]
            row = describe(samples["renderCallMs"], deadline)
            row["cpu"] = describe(samples["cpuMs"], deadline)
            row["fenceWait"] = describe(samples["fenceWaitMs"], deadline)
            row["presentWait"] = describe(samples["presentWaitMs"], deadline)
            row["gpu"] = describe(samples["gpuMs"], deadline) if samples["gpuMs"] else None
            per_run.append(row)
        summary["scenarios"][camera] = {
            "perRun": per_run,
            "runLevelRange": {metric: [min(row[metric] for row in per_run),
                                      max(row[metric] for row in per_run)]
                              for metric in ("p50Ms", "p95Ms", "p99Ms", "maxMs", "deadlineMisses")},
            "cpuP95MsRange": [min(row["cpu"]["p95Ms"] for row in per_run),
                              max(row["cpu"]["p95Ms"] for row in per_run)],
            "validGpuP95MsRange": ([min(row["gpu"]["p95Ms"] for row in per_run),
                                    max(row["gpu"]["p95Ms"] for row in per_run)]
                                   if all(row["gpu"] is not None for row in per_run) else None),
            "fenceWaitP95MsRange": [min(row["fenceWait"]["p95Ms"] for row in per_run),
                                    max(row["fenceWait"]["p95Ms"] for row in per_run)],
            "presentWaitP95MsRange": [min(row["presentWait"]["p95Ms"] for row in per_run),
                                      max(row["presentWait"]["p95Ms"] for row in per_run)],
        }
    output.mkdir(parents=True, exist_ok=True)
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    lines = [f"# Integrated renderer benchmark: {first['manifest']['id']}", "",
             f"Backend: {first['provenance']['backend']}; runs: {len(paths)}; deadline: {deadline} ms.", "",
             "Ranges below span independent runs. Raw frames and GPU validity remain in each run directory.", "",
             "| Scenario | Frames/run | p50 ms | p95 ms | p99 ms | max ms | misses | CPU p95 ms | valid GPU p95 ms |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for camera, item in summary["scenarios"].items():
        ranges = item["runLevelRange"]
        fmt = lambda metric: f"{ranges[metric][0]:.2f}–{ranges[metric][1]:.2f}"
        fmt_pair = lambda pair: "unknown" if pair is None else f"{pair[0]:.2f}–{pair[1]:.2f}"
        lines.append(f"| {camera} | {item['perRun'][0]['frames']} | {fmt('p50Ms')} | {fmt('p95Ms')} | "
                     f"{fmt('p99Ms')} | {fmt('maxMs')} | {fmt('deadlineMisses')} | "
                     f"{fmt_pair(item['cpuP95MsRange'])} | {fmt_pair(item['validGpuP95MsRange'])} |")
    lines += ["", "GPU samples require `gpuStatus=valid` and distinct `gpuFrameId`; missing values are never zero.",
              "Production and consumption of requested techniques require separate runtime evidence.",
              "Tracked memory mixes CPU and GPU allocations; post-scene-release values retain asset and renderer caches. Isolated GPU and pool bytes are unknown."]
    (output / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-gl", action="store_true")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--build-config", default="unknown")
    parser.add_argument("--compiler", default="unknown")
    parser.add_argument("--shader-cache-dir", type=Path)
    parser.add_argument("--cache-mode", choices=("inherited", "cold", "warm"), default="inherited")
    parser.add_argument("--run-dir", type=Path, action="append", default=[])
    parser.add_argument("--environment-json", type=Path,
                        help="Optional live-editor environment record, including presentation and VSync")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.repeats < 3:
        parser.error("--repeats must be at least 3")
    try:
        paths = run_gl(args) if args.capture_gl else args.run_dir
        analyze(paths, args.output, args.environment_json)
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
