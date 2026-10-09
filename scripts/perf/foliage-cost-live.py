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
import math
import os
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from editor_mcp import Client  # noqa: E402

ARMS = ['Shipped', 'CpuCull', 'NoDensityLod', 'NoFoliage']


def foreign_gpu_processes(own_pid=None) -> list:
    """Engine processes on the box other than the measured editor: an editor,
    runtime or test binary from another worktree on the same GPU makes a cell
    invalid, so every cell records what it ran beside."""
    import subprocess
    script = ("Get-Process OloEditor,OloRuntime,OloEngine-Tests,WowB,Wow -ErrorAction SilentlyContinue | "
              "ForEach-Object { '{0}|{1}' -f $_.Id,$_.Path }")
    out = subprocess.run(['pwsh', '-NoProfile', '-Command', script], capture_output=True, text=True).stdout
    rows = [line.split('|', 1) for line in out.splitlines() if '|' in line]
    return [{'pid': int(pid), 'path': path} for pid, path in rows if own_pid is None or int(pid) != own_pid]


class Editor:
    def __init__(self, port: int):
        discovery = Path(os.environ.get('TEMP', '/tmp')) / f'oloengine-mcp-{port}.json'
        self.client = Client(discovery, timeout=120)
        self.foliage_entity = None

    def tool(self, tool_name, **arguments):
        # A render-path switch rebuilds the graph and compiles pipelines on the
        # main thread, which can outlast one MCP marshal; wait it out rather
        # than abandon the pass.
        for attempt in range(12):
            try:
                return self.client.tool(tool_name, **arguments)
            except RuntimeError as error:
                if 'Timed out waiting for the editor main thread' not in str(error) or attempt == 11:
                    raise
                time.sleep(5)

    def open_scene(self, scene: str):
        result = self.tool('olo_scene_open', path=scene)
        if not result.get('ok'):
            raise RuntimeError(f'scene did not open: {result}')
        self.foliage_entity = None
        for row in self.tool('olo_scene_list_entities').get('entities', []):
            yaml_text = self.tool('olo_scene_get_entity', id=str(row['id'])).get('componentsYaml', '')
            if '\nFoliageComponent:' in yaml_text:
                self.foliage_entity = str(row['id'])
        if self.foliage_entity is None:
            raise RuntimeError('no FoliageComponent entity in the scene')

    def setting(self, setting: str, value: str):
        return self.tool('olo_renderer_settings_set', setting=setting, value=value)

    def arm(self, arm: str):
        self.tool('olo_cvar_set', name='OLO_FOLIAGE_CPU_CULL', value='true' if arm == 'CpuCull' else 'false')
        self.tool('olo_cvar_set', name='OLO_FOLIAGE_NO_DENSITY_LOD', value='true' if arm == 'NoDensityLod' else 'false')
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
            key = frame.get('gpuMeasurementFrameId')
            if key is None or timings.get('gpuResultsStale'):
                time.sleep(spacing)
                continue
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


def pass_ms(arm: dict, name: str) -> float:
    return arm['passMedianMs'].get(name) or 0.0


def render_summary(result: dict) -> str:
    """The doc's Vulkan table. A cell measured beside another engine process is
    listed as contended and kept out of the ranges; per-pass GPU times that came
    back null are counted, never read as zero."""
    lines = ['| subject | pose | path | shadows | frame GPU | foliage share of the frame | main-view cull | '
             'shadow-view culls | ShadowPass Δ (casters + culls) | forward draw (+prepass) | G-buffer share (ScenePass Δ) | '
             'GPU culling off: frame Δ | density LOD off: frame Δ | null GPU samples | contended |',
             '|' + '---|' * 15]
    for cell in result['cells']:
        arms = cell['arms']
        shipped, none = arms['Shipped'], arms['NoFoliage']
        contended = bool(cell.get('foreignGpuProcesses'))
        nulls = sum(a['gpuNullSamples'] + a['nullPassEntries'] for a in arms.values())
        values = [shipped['gpuMsMedian'], shipped['gpuMsMedian'] - none['gpuMsMedian'], pass_ms(shipped, 'FoliageCull'),
                  pass_ms(shipped, 'ShadowPass/FoliageCull'), pass_ms(shipped, 'ShadowPass') - pass_ms(none, 'ShadowPass'),
                  pass_ms(shipped, 'FoliagePass') + pass_ms(shipped, 'FoliagePrepassPass'),
                  pass_ms(shipped, 'ScenePass') - pass_ms(none, 'ScenePass'),
                  arms['CpuCull']['gpuMsMedian'] - shipped['gpuMsMedian'],
                  arms['NoDensityLod']['gpuMsMedian'] - shipped['gpuMsMedian']]
        lines.append('| {} | {} | {} | {} | '.format(cell['subject'], cell['pose'], cell['path'], cell['shadows']) +
                     ' | '.join('{:.2f}'.format(v) for v in values) +
                     ' | {} | {} |'.format(nulls, 'yes' if contended else ''))
    if result.get('tails'):
        lines += ['', '| pose | path | frames | frame time p50 | p95 | p99 | max | misses (16.67 ms) | GPU p50 | GPU p99 |',
                  '|' + '---|' * 10]
        for tail in result['tails']:
            frames = tail['history'].get('frames') or tail['history'].get('series') or []
            ft = sorted(f['frameTimeMs'] for f in frames if isinstance(f.get('frameTimeMs'), (int, float)))
            gpu = sorted(f['gpuMs'] for f in frames if isinstance(f.get('gpuMs'), (int, float)))

            def pct(v, q):
                return v[max(0, math.ceil(len(v) * q) - 1)] if v else float('nan')
            lines.append('| {} | {} | {} | {:.2f} | {:.2f} | {:.2f} | {:.2f} | {} | {:.2f} | {:.2f} |'.format(
                tail['pose'], tail['path'], len(ft), pct(ft, .5), pct(ft, .95), pct(ft, .99), ft[-1] if ft else float('nan'),
                sum(1 for x in ft if x > 1000.0 / 60.0), pct(gpu, .5), pct(gpu, .99)))
    return '\n'.join(lines) + '\n'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int)
    parser.add_argument('--summarise', type=Path, help='write the markdown tables for --output to this file and exit')
    parser.add_argument('--poses', type=Path, help='a headless run\'s foliage-cost.json')
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
    parser.add_argument('--pose-filter', default='', help='comma list of pose names to keep (default: all)')
    parser.add_argument('--editor-pid', type=int, help='the measured editor, excluded from the foreign list')
    parser.add_argument('--resume', action='store_true', help='keep the cells already in --output and measure the rest')
    args = parser.parse_args()
    if args.summarise:
        args.summarise.write_text(render_summary(json.loads(args.output.read_text(encoding='utf-8'))), encoding='utf-8')
        return 0
    if args.port is None or args.poses is None or args.editor_pid is None:
        parser.error('--port, --poses and --editor-pid are required to measure')

    source = json.loads(args.poses.read_text(encoding='utf-8'))
    editor = Editor(args.port)
    editor.tool('olo_viewport_set_size', width=1920, height=1080)
    result = {'issue': 1391, 'backend': args.backend_label, 'protocol': vars(args) | {'poses': str(args.poses),
                                                                                       'output': str(args.output)},
              'cells': [], 'tails': [], 'memory': {}}
    result['protocol'].pop('port', None)
    if args.resume and args.output.exists():
        previous = json.loads(args.output.read_text(encoding='utf-8'))
        result['cells'] = previous.get('cells', [])
        result['memory'] = previous.get('memory', {})
    done = {(c['subject'], c['pose'], c['path'], c['shadows']) for c in result['cells']}
    try:
        for subject in args.subjects.split(','):
            scene = source['poses'][subject]['scene']
            poses = source['poses'][subject]['poses']
            if args.pose_filter:
                wanted = set(args.pose_filter.split(','))
                poses = [p for p in poses if p['name'] in wanted]
            if not poses:
                continue
            editor.open_scene(scene)
            editor.tool('olo_viewport_set_size', width=1920, height=1080)
            time.sleep(args.settle * 2)
            if subject not in result['memory']:
                result['memory'][subject] = editor.tool('olo_memory_report')
            for path in args.paths.split(','):
                editor.setting('renderpath', path)
                for shadows in args.shadows.split(','):
                    editor.setting('virtualshadowmaps', 'on' if shadows == 'vsm' else 'off')
                    for pose in poses:
                        if (subject, pose['name'], path, shadows.upper()) in done:
                            continue
                        editor.pose(pose)
                        foreign_before = foreign_gpu_processes(args.editor_pid)
                        started = time.strftime('%Y-%m-%dT%H:%M:%S')
                        arms = {arm: [] for arm in ARMS}
                        for round_index in range(args.rounds):
                            for k in range(len(ARMS)):
                                arm = ARMS[(k + round_index) % len(ARMS)]
                                editor.arm(arm)
                                time.sleep(args.settle)
                                arms[arm] += editor.sample(args.samples, args.spacing)
                        editor.arm('Shipped')
                        foreign_after = foreign_gpu_processes(args.editor_pid)
                        result['cells'].append({'subject': subject, 'pose': pose['name'], 'path': path,
                                                'shadows': shadows.upper(), 'started': started,
                                                'finished': time.strftime('%Y-%m-%dT%H:%M:%S'),
                                                'foreignGpuProcesses': foreign_before + [p for p in foreign_after if p not in foreign_before],
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
