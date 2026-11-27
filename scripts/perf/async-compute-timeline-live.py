"""Async-compute GPU timeline (#1341): sample a running Release editor over MCP.

OLO_VK_ASYNC_COMPUTE is read when the device is created, so the off and on arms
are separate editor processes; run this once per process, alternating arms
(on/off/off/on ...), and compare the JSON files. Each run opens the scene, turns
on the async candidates (GTAO, virtual shadow maps, volumetric fog), sets the
camera, warms, and samples distinct resolved frames of olo_perf_pass_timings:

- the frame's GPU time and the asyncCompute counters (batches on the compute
  queue, declines and their reason);
- queueOverlap: union busy time per queue, the time both queues had a bracket
  open, and each async batch's interval and summed pass time;
- every top-level pass's queue, batch, begin/end offset and GPU time, so the
  batch's neighbours on the graphics queue are visible.

    python scripts/perf/async-compute-timeline-live.py --port 7341 --arm on --output <file.json>
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from editor_mcp import Client  # noqa: E402


def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--arm', required=True, help='label recorded in the output (the process env sets the lever)')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scene', default='Scenes/Benchmark/Courtyard.olo')
    parser.add_argument('--eye', type=float, nargs=3, default=[-9.0, 1.9, 0.0])
    parser.add_argument('--yaw', type=float, default=90.0)
    parser.add_argument('--pitch', type=float, default=-4.0)
    parser.add_argument('--fov', type=float, default=60.0)
    parser.add_argument('--render-path', default='deferred')
    parser.add_argument('--settings', nargs='*', default=['virtualshadowmaps=true'],
                        help='olo_renderer_settings_set pairs applied after the scene opens')
    parser.add_argument('--passes', nargs='*', default=['gtao', 'fog', 'fogvolumetric'],
                        help='olo_render_toggle_pass names enabled after the scene opens')
    parser.add_argument('--warm-frames', type=int, default=240)
    parser.add_argument('--samples', type=int, default=120)
    args = parser.parse_args()

    discovery = Path(os.environ.get('TEMP', '/tmp')) / f'oloengine-mcp-{args.port}.json'
    client = Client(discovery, timeout=180)
    client.wait_ready()
    client.tool('olo_viewport_set_size', width=1920, height=1080)
    opened = client.tool('olo_scene_open', path=args.scene)
    if not opened.get('ok'):
        raise RuntimeError(f'scene did not open: {opened}')
    applied = {'renderpath': client.tool('olo_renderer_settings_set', setting='renderpath', value=args.render_path)}
    for pair in args.settings:
        name, value = pair.split('=', 1)
        applied[name] = client.tool('olo_renderer_settings_set', setting=name, value=value)
    for name in args.passes:
        applied['pass:' + name] = client.tool('olo_render_toggle_pass', name=name, enabled=True)
    client.tool('olo_camera_set_pose', position=args.eye, yaw=args.yaw, pitch=args.pitch, fov=args.fov)

    def frame_id() -> int:
        return int(client.tool('olo_perf_pass_timings').get('frame', {}).get('currentFrameId', 0))

    start = frame_id()
    deadline = time.monotonic() + 300
    while frame_id() - start < args.warm_frames:
        if time.monotonic() > deadline:
            raise RuntimeError('editor is not advancing frames (iconified?)')
        time.sleep(0.1)

    samples = {}
    deadline = time.monotonic() + max(120.0, args.samples * 2.0)
    while len(samples) < args.samples and time.monotonic() < deadline:
        t = client.tool('olo_perf_pass_timings')
        key = t.get('frame', {}).get('gpuMeasurementFrameId')
        if not key or key in samples or t.get('gpuResultsStale'):
            time.sleep(0.01)
            continue
        passes = [{k: p.get(k) for k in ('pass', 'queue', 'asyncBatch', 'gpuMs', 'beginMs', 'endMs')}
                  for p in t.get('passes', []) if p.get('gpuMs') is not None]
        samples[key] = {'frameGpuMs': t.get('frame', {}).get('gpuMs'),
                        'frameTimeMs': t.get('frame', {}).get('frameTimeMs'),
                        'asyncCompute': t.get('asyncCompute'), 'queueOverlap': t.get('queueOverlap'),
                        'passGpuTotalMs': t.get('passGpuTotalMs'), 'passes': passes}
    client.tool('olo_viewport_set_size', reset=True)
    if len(samples) < args.samples:
        raise RuntimeError(f'only {len(samples)} of {args.samples} distinct resolved frames')

    rows = list(samples.values())
    batch_rows = [b for r in rows for b in (r['queueOverlap'] or {}).get('batches', [])]
    summary = {
        'frameGpuMs': median([r['frameGpuMs'] for r in rows]),
        'frameTimeMs': median([r['frameTimeMs'] for r in rows]),
        'graphicsBusyMs': median([(r['queueOverlap'] or {}).get('graphicsBusyMs') for r in rows]),
        'asyncComputeBusyMs': median([(r['queueOverlap'] or {}).get('asyncComputeBusyMs') for r in rows]),
        'overlapMs': median([(r['queueOverlap'] or {}).get('overlapMs') for r in rows]),
        'overlapMsMax': max(((r['queueOverlap'] or {}).get('overlapMs') or 0.0) for r in rows),
        'batchesOnComputeQueue': median([(r['asyncCompute'] or {}).get('batchesOnComputeQueue') for r in rows]),
        'batchesDeclined': median([(r['asyncCompute'] or {}).get('batchesDeclined') for r in rows]),
        'declineReasons': sorted({(r['asyncCompute'] or {}).get('declineReason') or '' for r in rows} - {''}),
        'ownershipTransfers': median([(r['asyncCompute'] or {}).get('ownershipTransfers') for r in rows]),
        'batchPassGpuMs': median([b.get('passGpuMs') for b in batch_rows]),
        'batchSpanMs': median([(b['endMs'] - b['beginMs']) for b in batch_rows if 'beginMs' in b]),
        'batchPassNames': sorted({p['pass'] for r in rows for p in r['passes'] if p.get('asyncBatch') is not None}),
    }
    out = {'tool': 'async-compute-timeline-live', 'arm': args.arm, 'argv': sys.argv[1:], 'settings': applied,
           'summary': summary, 'samples': rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(out, indent=1), encoding='utf-8')
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
