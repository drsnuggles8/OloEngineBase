"""Live Vulkan (or GL) acceptance cells through a running editor's MCP server (#1259).

The editor host's benchmark capture reopens the scene, and loading the acceptance
scene's 44 bound coats outlasts the MCP main-thread wait (a reload briefly holds
~13 GB of private memory), so every capture attempt failed. The editor cells are
therefore measured live, on the scene opened once: for each render path (and hybrid ray-traced shadows), with
virtual shadow maps on, at each manifest camera, after a settle, the last
`--frames` completed editor frames from olo_perf_frame_history (frame, CPU, GPU,
fence and present split) plus one olo_perf_pass_timings snapshot. The editor runs
on its own clock (animations play; animal paths do not advance in edit mode), so
these are live observations beside the GL test host's controlled runs, never
pooled with them.

    python scripts/perf/acceptance-live.py --port 7391 \\
        --manifest OloEditor/assets/benchmark/manifests/acceptance-deferred.yaml \\
        --output live-vk.json --repeats 3

Attribution pass on the same editor (no second open): --skip-open with
--scope-samples N (olo_perf_cpu_scopes averaged over N frames per camera),
--groom-stats (olo_groom_budget_stats) and --keep-series (the raw frames, to
tell a CPU hitch from a GPU one). The hybrid cell always records
olo_rt_scene_stats, so a cell that traced nothing is visible.

Launch the editor first (Release, --rhi=vulkan, MCP writes on, ThrottleEditMode
false); the backend is recorded from the editor log by the caller.
"""
from __future__ import annotations

import argparse
import json
import tempfile
import time
from pathlib import Path

import yaml

from editor_mcp import Client

CELLS = [("forward", "forward", False), ("forward-plus", "forwardplus", False),
         ("deferred", "deferred", False), ("hybrid", "deferred", True)]


def percentile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = max(0, min(len(ordered) - 1, int(round(q * (len(ordered) - 1)))))
    return ordered[rank]


def stats(values: list[float], deadline: float) -> dict:
    return {"count": len(values), "p50Ms": percentile(values, 0.5), "p95Ms": percentile(values, 0.95),
            "p99Ms": percentile(values, 0.99), "maxMs": max(values) if values else None,
            "deadlineMisses": sum(1 for v in values if v > deadline)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--port", type=int, default=7391)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=120, help="completed frames kept per camera")
    parser.add_argument("--settle", type=float, default=6.0, help="seconds after a pose or path change")
    parser.add_argument("--repeats", type=int, default=1, help="passes over every cell and camera")
    parser.add_argument("--gate-ms", type=float, default=16.667)
    parser.add_argument("--cells", default=",".join(c[0] for c in CELLS))
    parser.add_argument("--scope-samples", type=int, default=0,
                        help="olo_perf_cpu_scopes snapshots per camera, averaged (0 = none)")
    parser.add_argument("--groom-stats", action="store_true", help="keep one olo_groom_budget_stats per camera")
    parser.add_argument("--keep-series", action="store_true", help="keep the per-frame series, not only its stats")
    parser.add_argument("--skip-open", action="store_true", help="the scene is already open in the editor")
    args = parser.parse_args()

    manifest = yaml.safe_load(args.manifest.read_text(encoding="utf-8"))
    client = Client(Path(tempfile.gettempdir()) / f"oloengine-mcp-{args.port}.json", timeout=900)
    client.wait_ready()
    wanted = set(args.cells.split(","))
    result = {"manifest": str(args.manifest), "scene": manifest["Scene"], "gateMs": args.gate_ms,
              "framesPerCamera": args.frames, "settleSeconds": args.settle, "runs": []}

    # Opening the 96-entity scene with 44 bound coats takes longer than the
    # MCP main-thread wait on Vulkan; the open carries on, so wait for it.
    # A follow-up pass on the same editor skips the open: a reload is another
    # ~13 GB transient and another long main-thread wait.
    try:
        if not args.skip_open:
            client.tool("olo_scene_open", path=manifest["Scene"])
    except RuntimeError as error:
        if "Timed out" not in str(error):
            raise
    scene_name = Path(manifest["Scene"]).name
    deadline = time.monotonic() + 1800
    while True:
        try:
            summary = client.tool("olo_scene_summary")
            if scene_name in json.dumps(summary):
                break
        except RuntimeError:
            pass
        if time.monotonic() > deadline:
            raise RuntimeError(f"{scene_name} did not finish opening")
        time.sleep(10)
    time.sleep(args.settle)
    client.tool("olo_editor_debug_draw_set", category="all", enabled=False)
    client.tool("olo_viewport_set_size", width=1920, height=1080)
    client.tool("olo_renderer_settings_set", setting="virtualshadowmaps", value="on")
    try:
        for repeat in range(args.repeats):
            for cell_id, path, rt in CELLS:
                if cell_id not in wanted:
                    continue
                client.tool("olo_renderer_settings_set", setting="renderpath", value=path)
                client.tool("olo_renderer_settings_set", setting="raytracedshadows", value="on" if rt else "off")
                for cam in manifest["Cameras"]:
                    client.tool("olo_camera_set_pose", position=cam["Position"], yaw=cam["YawDegrees"],
                                pitch=cam["PitchDegrees"], fov=cam["FovDegrees"])
                    time.sleep(args.settle)
                    # Wait until the ring holds `frames` frames of THIS pose.
                    probe = client.tool("olo_perf_frame_history", points=1)
                    frame_ms = (probe.get("stats") or {}).get("meanMs") or 200.0
                    # CPU scopes report the previous frame only, so sample them
                    # across the window and keep the per-scope mean.
                    scope_sums: dict[str, list[float]] = {}
                    for _ in range(args.scope_samples):
                        time.sleep(1.3 * args.frames * frame_ms / 1000.0 / max(1, args.scope_samples))
                        if args.scope_samples:
                            for scope in client.tool("olo_perf_cpu_scopes", limit=40).get("scopes", []):
                                scope_sums.setdefault(scope["name"], []).append(scope["timeMs"])
                    if not args.scope_samples:
                        time.sleep(1.3 * args.frames * frame_ms / 1000.0)
                    history = client.tool("olo_perf_frame_history", raw=True).get("series", [])[-args.frames:]
                    timings = client.tool("olo_perf_pass_timings")
                    passes = sorted(((p.get("gpuMs") or 0.0, p.get("pass"),
                                      {s.get("name") or s.get("pass"): s.get("gpuMs") for s in p.get("subPasses") or []})
                                     for p in timings.get("passes", [])), key=lambda e: e[0], reverse=True)[:6]
                    run = {"repeat": repeat, "cell": cell_id, "camera": cam["Id"], "rayTracedShadows": rt}
                    for key in ("frameTimeMs", "cpuMs", "gpuMs", "fenceWaitMs", "presentWaitMs"):
                        values = [f[key] for f in history if isinstance(f.get(key), (int, float))]
                        run[key] = stats(values, args.gate_ms)
                    run["topPasses"] = [{"pass": n, "gpuMs": v, "subPasses": s} for v, n, s in passes]
                    run["topCpuPasses"] = sorted(({"pass": p.get("pass"), "cpuMs": p.get("cpuMs")}
                                                  for p in timings.get("passes", []) if p.get("cpuMs")),
                                                 key=lambda e: e["cpuMs"], reverse=True)[:6]
                    if args.keep_series:
                        run["series"] = [{k: f.get(k) for k in ("frameTimeMs", "cpuMs", "gpuMs")} for f in history]
                    if args.groom_stats:
                        groom = client.tool("olo_groom_budget_stats")
                        run["groomStats"] = {k: groom.get(k) for k in ("groom", "coatShadow", "sceneShadow", "animalBudget")}
                    if rt:
                        # Whether the hybrid cell traced at all: the TLAS can be withheld (#1578).
                        run["rtScene"] = client.tool("olo_rt_scene_stats")
                    if scope_sums:
                        means = sorted(((sum(v) / args.scope_samples, k) for k, v in scope_sums.items()), reverse=True)
                        run["cpuScopes"] = [{"scope": k, "meanMs": round(m, 3)} for m, k in means[:15]]
                    result["runs"].append(run)
                    args.output.write_text(json.dumps(result, indent=1), encoding="utf-8")
                    print(f"[live] r{repeat} {cell_id} {cam['Id']}: frame p50 {run['frameTimeMs']['p50Ms']} "
                          f"p95 {run['frameTimeMs']['p95Ms']} gpu p50 {run['gpuMs']['p50Ms']}", flush=True)
        result["memory"] = client.tool("olo_memory_report", limit=12)
        args.output.write_text(json.dumps(result, indent=1), encoding="utf-8")
    finally:
        client.tool("olo_viewport_set_size", reset=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
