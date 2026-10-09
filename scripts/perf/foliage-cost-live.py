"""Foliage cost baseline (#1391), live cells: drive a running editor over MCP.

The headless test covers OpenGL; Vulkan needs a real editor (`--rhi=vulkan`), so
this walks the same poses (read from a headless run's foliage-cost.json) through
the editor, with the same four arms interleaved in rotated order:

    Shipped       as authored
    CpuCull       cvar OLO_FOLIAGE_CPU_CULL = true
    NoDensityLod  cvar OLO_FOLIAGE_NO_DENSITY_LOD = true
    NoFoliage     FoliageComponent.Enabled = false

and samples olo_perf_pass_timings (frame time, GPU frame and per-pass GPU time,
deduplicated by the frame the timings describe) and olo_perf_frame_history for
the tails. Vulkan per-pass GPU times often come back null (outOfOrder); those
samples are counted, never read as zero.

    python scripts/perf/foliage-cost-live.py --port 7391 --poses <run>/foliage-cost.json \
        --output docs/analysis/foliage-cost-baseline-1391/vulkan-live.json

Launch the editor first (Release, --rhi=vulkan, MCP autostart with writes),
with the editor preferences' throttles off and FrameRateCap 0.
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

ARMS = ['Shipped', 'CpuCull', 'NoDensityLod', 'NoFoliage']


class Editor:
    def __init__(self, port: int):
        discovery = Path(os.environ.get('TEMP', '/tmp')) / f'oloengine-mcp-{port}.json'
        self.client = Client(discovery, timeout=120)
        self.foliage_entity = None

    def tool(self, name, **arguments):
        return self.client.tool(name, **arguments)

    def open_scene(self, scene: str):
        result = self.tool('olo_scene_open', path=scene)
        if not result.get('ok'):
            raise RuntimeError(f'scene did not open: {result}')
        entities = self.tool('olo_scene_list_entities')
        rows = entities.get('entities', entities)
        for row in rows:
            if 'FoliageComponent' in row.get('components', []):
                self.foliage_entity = str(row['uuid'] if 'uuid' in row else row['id'])
        if self.foliage_entity is None:
            raise RuntimeError('no FoliageComponent entity in the scene')

    def setting(self, setting: str, value: str):
        return self.tool('olo_renderer_settings_set', setting=setting, value=value)

    def arm(self, arm: str):
        self.tool('olo_cvar_set', name='OLO_FOLIAGE_CPU_CULL', value=arm == 'CpuCull')
        self.tool('olo_cvar_set', name='OLO_FOLIAGE_NO_DENSITY_LOD', value=arm == 'NoDensityLod')
        self.tool('olo_entity_set_field', entity=self.foliage_entity, component='FoliageComponent', field='Enabled',
                  value=arm != 'NoFoliage')

    def pose(self, pose: dict):
        self.tool('olo_camera_set_pose', position=pose['eye'], yaw=pose['yawDegrees'], pitch=pose['pitchDegrees'],
                  fov=pose.get('fovDegrees', 60.0))

    def sample(self, count: int, spacing: float) -> list[dict]:
        """`count` distinct resolved frames of pass timings."""
        seen = {}
        deadline = time.monotonic() + count * spacing * 4 + 5
        while len(seen) < count and time.monotonic() < deadline:
            timings = self.tool('olo_perf_pass_timings')
            frame = timings.get('frame', {})
            key = frame.get('gpuFrameNumber') or frame.get('frameNumber') or timings.get('frameNumber')
            if key is None:
                key = len(seen)
            if key not in seen:
                passes = {}
                nulls = 0
                for entry in timings.get('passes', []):
                    if entry.get('gpuMs') is None:
                        nulls += 1
                        continue
                    passes[entry['pass']] = passes.get(entry['pass'], 0.0) + entry['gpuMs']
                    for sub in entry.get('subPasses', []) or []:
                        if sub.get('gpuMs') is not None:
                            name = entry['pass'] + '/' + sub.get('name', sub.get('pass', '?'))
                            passes[name] = passes.get(name, 0.0) + sub['gpuMs']
                seen[key] = {'frameTimeMs': frame.get('frameTimeMs'), 'gpuMs': frame.get('gpuMs'),
                             'gpuStatus': frame.get('gpuStatus'), 'passes': passes, 'nullPasses': nulls}
            time.sleep(spacing)
        return list(seen.values())


def median(values):
    values = [v for v in values if isinstance(v, (int, float))]
    return statistics.median(values) if values else None


def summarise_arm(samples: list[dict]) -> dict:
    names = sorted({n for s in samples for n in s['passes']})
    return {'samples': len(samples),
            'frameTimeMsMedian': median([s['frameTimeMs'] for s in samples]),
            'gpuMsMedian': median([s['gpuMs'] for s in samples]),
            'gpuNullSamples': sum(1 for s in samples if s['gpuMs'] is None),
            'nullPassEntries': sum(s['nullPasses'] for s in samples),
            'passMedianMs': {n: median([s['passes'].get(n) for s in samples]) for n in names},
            'raw': samples}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--poses', type=Path, required=True, help='a headless run\'s foliage-cost.json')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--backend-label', default='vulkan')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--settle', type=float, default=2.0, help='seconds after an arm switch before sampling')
    parser.add_argument('--samples', type=int, default=8)
    parser.add_argument('--spacing', type=float, default=0.25)
    parser.add_argument('--subjects', default='Traversal,Meadow,Woodland')
    parser.add_argument('--paths', default='forward,forwardplus,deferred')
    parser.add_argument('--shadows', default='csm,vsm')
    parser.add_argument('--tail-seconds', type=float, default=12.0)
    args = parser.parse_args()

    source = json.loads(args.poses.read_text(encoding='utf-8'))
    editor = Editor(args.port)
    editor.tool('olo_viewport_set_size', width=1920, height=1080)
    result = {'issue': 1391, 'backend': args.backend_label, 'protocol': vars(args) | {'poses': str(args.poses),
                                                                                       'output': str(args.output)},
              'cells': [], 'tails': [], 'memory': {}}
    result['protocol'].pop('port', None)
    try:
        for subject in args.subjects.split(','):
            scene = source['poses'][subject]['scene']
            poses = source['poses'][subject]['poses']
            editor.open_scene(scene)
            time.sleep(args.settle * 2)
            result['memory'][subject] = editor.tool('olo_memory_report')
            for path in args.paths.split(','):
                editor.setting('renderpath', path)
                for shadows in args.shadows.split(','):
                    editor.setting('virtualshadowmaps', 'on' if shadows == 'vsm' else 'off')
                    for pose in poses:
                        editor.pose(pose)
                        arms = {arm: [] for arm in ARMS}
                        for round_index in range(args.rounds):
                            for k in range(len(ARMS)):
                                arm = ARMS[(k + round_index) % len(ARMS)]
                                editor.arm(arm)
                                time.sleep(args.settle)
                                arms[arm] += editor.sample(args.samples, args.spacing)
                        editor.arm('Shipped')
                        result['cells'].append({'subject': subject, 'pose': pose['name'], 'path': path,
                                                'shadows': shadows.upper(),
                                                'arms': {a: summarise_arm(s) for a, s in arms.items()}})
                        print(f'[foliage-live] {subject} {pose["name"]} {path} {shadows} done', flush=True)
                        args.output.write_text(json.dumps(result, indent=1), encoding='utf-8')
            if subject == 'Traversal':
                for path in args.paths.split(','):
                    editor.setting('renderpath', path)
                    editor.setting('virtualshadowmaps', 'off')
                    for pose in (poses[0], poses[len(poses) // 2], poses[-1]):
                        editor.pose(pose)
                        time.sleep(args.settle)
                        time.sleep(args.tail_seconds)
                        history = editor.tool('olo_perf_frame_history', raw=True)
                        result['tails'].append({'pose': pose['name'], 'path': path, 'shadows': 'CSM',
                                                'history': history})
                        args.output.write_text(json.dumps(result, indent=1), encoding='utf-8')
    finally:
        editor.arm('Shipped')
        editor.tool('olo_viewport_set_size', reset=True)
    args.output.write_text(json.dumps(result, indent=1), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
