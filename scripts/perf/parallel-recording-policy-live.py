"""Parallel-recording policy A/B (#1340): drive a running Release editor over MCP.

Every arm runs in the same process and binary; only console variables change:

    off     OLO_VK_PARALLEL_RECORDING=0 (every region inline)
    legacy  the pre-#1340 rule: fork any replay that splits into two items of
            at least 32 packets (OLO_VK_PARALLEL_MIN_PACKETS=64,
            OLO_VK_PARALLEL_PACKETS_PER_ITEM=32)
    policy  the shipped per-family defaults (both overrides unset)

Blocks rotate the arm order (ABC, BCA, CAB, ...). Each block warms, then samples
distinct frames of olo_perf_pass_timings: every recording region's decision,
family, work units, item count, wall, summed worker, join and frontend-prepare
time, plus the CPU time of untouched control passes, whose drift across arms
bounds what the host itself moved. With --capture, each arm also captures the
named render targets and compares them pixel by pixel with the off arm.

    python scripts/perf/parallel-recording-policy-live.py --port 7340 --output <file.json>

Launch the editor first: Release, --rhi=vulkan, MCP autostart with writes, the
editor's throttles off and FrameRateCap 0. Leave OLO_VK_RECORDING_COSTS unset.
"""
from __future__ import annotations

import argparse
import base64
import io
import json
import os
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from editor_mcp import Client  # noqa: E402

SCENES = {
    # Dense: 3,600 independent mesh windows, the #1013 benchmark.
    'dense': {'path': 'Scenes/Benchmark/ParallelRecording.olo', 'eye': [0, 9, 14], 'yaw': 0, 'pitch': 35, 'fov': 60},
    # Small: the authored Material Lab camera.
    'small': {'path': 'Scenes/Benchmark/MaterialLab.olo', 'eye': [0, 9, 16], 'yaw': 0, 'pitch': 31.5, 'fov': 45},
    # Mixed: architecture, foliage, decals, many local lights.
    'courtyard': {'path': 'Scenes/Benchmark/Courtyard.olo', 'eye': [-9, 1.9, 0], 'yaw': 90, 'pitch': -4, 'fov': 60},
    # Mixed: animals, terrain/foliage, local shadows and water.
    'integrated': {'path': 'Scenes/Benchmark/IntegratedRenderer.olo', 'eye': [0, 3.4, 16], 'yaw': 0, 'pitch': 6, 'fov': 50},
}

ARMS = {
    'off': {'OLO_VK_PARALLEL_RECORDING': '0', 'OLO_VK_PARALLEL_MIN_PACKETS': 'unset',
            'OLO_VK_PARALLEL_PACKETS_PER_ITEM': 'unset'},
    'legacy': {'OLO_VK_PARALLEL_RECORDING': 'unset', 'OLO_VK_PARALLEL_MIN_PACKETS': '64',
               'OLO_VK_PARALLEL_PACKETS_PER_ITEM': '32'},
    'policy': {'OLO_VK_PARALLEL_RECORDING': 'unset', 'OLO_VK_PARALLEL_MIN_PACKETS': 'unset',
               'OLO_VK_PARALLEL_PACKETS_PER_ITEM': 'unset'},
}

# Passes no recording region touches: their CPU time is the host-drift control.
CONTROL_PASSES = ('BloomPass', 'ToneMapPass', 'FinalPass')


def foreign_processes(own_pid=None) -> list:
    script = ("Get-Process OloEditor,OloRuntime,OloEngine-Tests,cmake,ninja,clang-cl,cl,lld-link -ErrorAction "
              "SilentlyContinue | ForEach-Object { '{0}|{1}|{2}' -f $_.Id,$_.ProcessName,$_.Path }")
    out = subprocess.run(['pwsh', '-NoProfile', '-Command', script], capture_output=True, text=True).stdout
    rows = [line.split('|', 2) for line in out.splitlines() if line.count('|') >= 2]
    return [{'pid': int(pid), 'name': name, 'path': path} for pid, name, path in rows
            if own_pid is None or int(pid) != own_pid]


class ForeignWatch:
    """Records every other engine or compiler process seen during a cell."""

    def __init__(self, own_pid, interval: float = 5.0):
        self.own_pid, self.interval, self.seen = own_pid, interval, {}
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _poll(self):
        for process in foreign_processes(self.own_pid):
            self.seen.setdefault(process['pid'], process)

    def _run(self):
        while not self.stop.wait(self.interval):
            self._poll()

    def __enter__(self):
        self._poll()
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join()
        self._poll()


class Editor:
    def __init__(self, port: int):
        discovery = Path(os.environ.get('TEMP', '/tmp')) / f'oloengine-mcp-{port}.json'
        self.client = Client(discovery, timeout=120)

    def tool(self, name, **arguments):
        for attempt in range(12):
            try:
                return self.client.tool(name, **arguments)
            except RuntimeError as error:
                if 'Timed out waiting for the editor main thread' not in str(error) or attempt == 11:
                    raise
                time.sleep(5)

    def arm(self, name: str):
        for cvar, value in ARMS[name].items():
            self.tool('olo_cvar_set', name=cvar, value=value)

    def frame_id(self) -> int:
        return int(self.tool('olo_perf_pass_timings').get('frame', {}).get('currentFrameId', 0))

    def warm(self, frames: int, timeout: float = 120.0):
        start = self.frame_id()
        deadline = time.monotonic() + timeout
        while self.frame_id() - start < frames:
            if time.monotonic() > deadline:
                raise RuntimeError(f'editor advanced fewer than {frames} frames in {timeout}s (iconified?)')
            time.sleep(0.05)

    def sample(self, count: int) -> list[dict]:
        seen = {}
        deadline = time.monotonic() + max(60.0, count * 2.0)
        while len(seen) < count and time.monotonic() < deadline:
            t = self.tool('olo_perf_pass_timings')
            frame = t.get('frame', {})
            key = frame.get('currentFrameId')
            if key is None or key in seen:
                time.sleep(0.01)
                continue
            pr = t.get('parallelRecording', {})
            regions = [{k: r.get(k) for k in ('pass', 'family', 'decision', 'workUnits', 'minWorkUnitsToFork',
                                              'itemCount', 'parallel', 'regionWallMs', 'workerRecordMs',
                                              'joinWaitMs', 'frontendPrepareMs', 'itemPassNames')}
                       for r in pr.get('regionTimings', [])]
            controls = {p['pass']: p.get('cpuMs') for p in t.get('passes', []) if p.get('pass') in CONTROL_PASSES}
            seen[key] = {'frameTimeMs': frame.get('frameTimeMs'), 'cpuMs': frame.get('cpuMs'),
                         'gpuMs': frame.get('gpuMs'), 'regions': regions, 'controls': controls,
                         'regionsForked': pr.get('regions'), 'inlineRegions': pr.get('inlineRegions'),
                         'secondaries': pr.get('secondariesExecuted'), 'mergeConflicts': pr.get('mergeConflicts')}
        if len(seen) < count:
            raise RuntimeError(f'only {len(seen)} of {count} distinct frames sampled')
        return list(seen.values())

    def capture(self, target: str):
        from PIL import Image
        result = self.client.rpc('tools/call', {'name': 'olo_render_capture_target',
                                                'arguments': {'name': target, 'forceFrame': True, 'maxWidth': 4096}})
        image = next((c for c in result.get('content', []) if c.get('type') == 'image'), None)
        if result.get('isError') or image is None:
            return None
        return Image.open(io.BytesIO(base64.b64decode(image['data']))).convert('RGBA')


def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def region_key(index: int, region: dict) -> str:
    # The occurrence index keeps two regions of one pass apart (depth vs colour).
    return f"{index:02d}:{region.get('pass')}:{region.get('family') or '-'}"


def summarize_block(samples: list[dict]) -> dict:
    per_region: dict[str, dict[str, list]] = {}
    for s in samples:
        for i, r in enumerate(s['regions']):
            slot = per_region.setdefault(region_key(i, r), {})
            for k in ('regionWallMs', 'workerRecordMs', 'joinWaitMs', 'frontendPrepareMs', 'workUnits', 'itemCount'):
                slot.setdefault(k, []).append(r.get(k))
            slot.setdefault('decisions', []).append(r.get('decision'))
    regions = {}
    for key, slot in per_region.items():
        regions[key] = {k: median(v) for k, v in slot.items() if k != 'decisions'}
        regions[key]['decision'] = max(set(slot['decisions']), key=slot['decisions'].count)
    controls = {}
    for name in CONTROL_PASSES:
        controls[name] = median([s['controls'].get(name) for s in samples])
    return {
        'recordingWallMs': median([sum(r.get('regionWallMs') or 0.0 for r in s['regions']) for s in samples]),
        'workerSumMs': median([sum(r.get('workerRecordMs') or 0.0 for r in s['regions']) for s in samples]),
        'frameTimeMs': median([s['frameTimeMs'] for s in samples]),
        'cpuMs': median([s['cpuMs'] for s in samples]),
        'regionsForked': median([s['regionsForked'] for s in samples]),
        'inlineRegions': median([s['inlineRegions'] for s in samples]),
        'secondaries': median([s['secondaries'] for s in samples]),
        'mergeConflictsMax': max((s['mergeConflicts'] or 0) for s in samples),
        'controls': controls,
        'regions': regions,
    }


def compare_images(a, b) -> dict:
    import numpy as np
    if a is None or b is None:
        return {'compared': False}
    if a.size != b.size:
        return {'compared': False, 'reason': f'size {a.size} vs {b.size}'}
    x = np.asarray(a, dtype=np.int16)
    y = np.asarray(b, dtype=np.int16)
    delta = np.abs(x - y).max(axis=2)
    return {'compared': True, 'pixels': int(delta.size), 'differing': int((delta > 0).sum()),
            'maxDelta': int(delta.max())}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scenes', nargs='+', default=list(SCENES), choices=list(SCENES))
    parser.add_argument('--arms', nargs='+', default=list(ARMS), choices=list(ARMS))
    parser.add_argument('--rounds', type=int, default=2, help='each round runs every rotation of the arm order once')
    parser.add_argument('--warm-frames', type=int, default=60)
    parser.add_argument('--samples', type=int, default=40)
    parser.add_argument('--render-path', default='deferred')
    parser.add_argument('--capture', nargs='*', default=[], help='render targets to compare across arms')
    parser.add_argument('--editor-pid', type=int)
    args = parser.parse_args()

    editor = Editor(args.port)
    editor.tool('olo_viewport_set_size', width=1920, height=1080)
    result = {'tool': 'parallel-recording-policy-live', 'argv': sys.argv[1:], 'arms': {a: ARMS[a] for a in args.arms},
              'renderPath': args.render_path, 'scenes': {}}
    try:
        for scene_name in args.scenes:
            scene = SCENES[scene_name]
            opened = editor.tool('olo_scene_open', path=scene['path'])
            if not opened.get('ok'):
                raise RuntimeError(f'scene did not open: {opened}')
            editor.tool('olo_renderer_settings_set', setting='renderpath', value=args.render_path)
            editor.tool('olo_camera_set_pose', position=scene['eye'], yaw=scene['yaw'], pitch=scene['pitch'],
                        fov=scene['fov'])
            editor.warm(args.warm_frames * 2)
            cell = {'scene': scene, 'blocks': [], 'captures': {}}
            orders = [args.arms[i:] + args.arms[:i] for i in range(len(args.arms))]
            with ForeignWatch(args.editor_pid) as watch:
                for _ in range(args.rounds):
                    for order in orders:
                        for arm in order:
                            editor.arm(arm)
                            editor.warm(args.warm_frames)
                            samples = editor.sample(args.samples)
                            cell['blocks'].append({'arm': arm, 'summary': summarize_block(samples)})
                            print(f"{scene_name:<10} {arm:<7} wall {cell['blocks'][-1]['summary']['recordingWallMs']:.3f} ms",
                                  flush=True)
                if args.capture:
                    reference = {}
                    for arm in args.arms:
                        editor.arm(arm)
                        editor.warm(args.warm_frames)
                        for target in args.capture:
                            image = editor.capture(target)
                            if arm == args.arms[0]:
                                reference[target] = image
                            cell['captures'].setdefault(target, {})[arm] = compare_images(reference.get(target), image)
                            # Same-arm repeat: the scene's own frame-to-frame noise floor.
                            if arm == args.arms[0]:
                                cell['captures'][target]['repeat:' + arm] = compare_images(image, editor.capture(target))
            cell['foreignProcesses'] = list(watch.seen.values())
            arms_summary = {}
            for arm in args.arms:
                blocks = [b['summary'] for b in cell['blocks'] if b['arm'] == arm]
                arms_summary[arm] = {
                    'recordingWallMs': median([b['recordingWallMs'] for b in blocks]),
                    'recordingWallBlocks': [b['recordingWallMs'] for b in blocks],
                    'workerSumMs': median([b['workerSumMs'] for b in blocks]),
                    'frameTimeMs': median([b['frameTimeMs'] for b in blocks]),
                    'controls': {n: median([b['controls'][n] for b in blocks]) for n in CONTROL_PASSES},
                }
            cell['arms'] = arms_summary
            result['scenes'][scene_name] = cell
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, indent=1), encoding='utf-8')
    finally:
        editor.tool('olo_viewport_set_size', reset=True)
        for cvar in ARMS['policy']:
            editor.tool('olo_cvar_set', name=cvar, value='unset')
    args.output.write_text(json.dumps(result, indent=1), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
