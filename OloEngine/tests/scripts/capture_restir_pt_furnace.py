"""Offline prepare by default. --run explicitly connects to the running editor.
Numeric evidence is float target ROI statistics; .hdr is RGBE, not float32 RGBA.
Furnace sphere has only zero-secondary support: this is NOT a multibounce test.

Requires PyYAML and a running Vulkan RT editor with MCP writes authorized for --run.
Example: python OloEngine/tests/scripts/capture_restir_pt_furnace.py --run
  --prefix repro --output repro-smoke --seeds 1211 974
Use --matrix --materials diffuse glossy for 4 modes x 4 masks x 3 stepped poses.
The live-calibrated predecessor disabled all editor debug overlays: axis overlays
otherwise make the center ROI contain non-geometry alpha. Preserve that setup.

records.jsonl contains exact float ROI statistics and actual PT counterFrame data.
GPUPassTimerPool snapshots have their own age/stale provenance; do not attribute
one snapshot to every sampled frame or sum overlapping parent/child scopes.
AS build GPU nanosecond fields currently remain unpopulated: zero is unresolved,
not free. Reference ray counts are upper bounds; PT shader counts are actual.
The separate optional HDR benchmark reloads the scene and captures only a
representative initial/mask1/pose0 image. RGBE discards alpha and quantizes RGB.
Use fresh --output directories; existing log files append. The script leaves the
scratch scene/settings active. Deadlines apply between potentially long RPCs.
"""
import argparse, copy, json, math, statistics, time
from restir_pt_capture_client import Client
from pathlib import Path
import yaml

ROOT = Path(__file__).resolve().parents[3]
HERE = ROOT / 'OloEditor/assets/benchmark/captures/restir-pt-1211'
POSES = [[0, 0, 4], [.35, .2, 3.98], [-.35, -.15, 4.02]]
MODES = {'initial': (False, False), 'temporal': (True, False),
         'spatial': (False, True), 'combined': (True, True)}
ROI = {'x': 72, 'y': 37, 'w': 16, 'h': 16}
TARGET = 'ReSTIRPTRadianceTexture'


def validate_seeds(parser, seeds):
    if len(set(seeds)) != len(seeds):
        parser.error('seeds must be distinct for independent seed blocks')
    if any(seed < 0 or seed > 16777215 for seed in seeds):
        parser.error('PT seeds must be in [0, 16777215]')
    if set(seeds).intersection(seed + 100000 for seed in seeds):
        parser.error('PT seeds must not overlap derived oracle seeds (seed + 100000)')


def prepare(args):
    HERE.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'OloEditor/SandboxProject/Assets/Scenes/Benchmark/MaterialLabFurnace.olo'
    original = yaml.safe_load(source.read_text(encoding='utf-8'))
    scenes = {}
    for material, roughness, metallic in [('diffuse', .8, 0), ('glossy', .12, 1)]:
        scene = copy.deepcopy(original)
        sphere = copy.deepcopy(next(e for e in scene['Entities'] if 'MeshComponent' in e))
        sphere['TransformComponent'].update(Translation=[0, 0, 0], Scale=[1, 1, 1])
        sphere['MaterialComponent'].update(Roughness=roughness, Metallic=metallic, PBRModel=1)
        scene['Entities'] = [e for e in scene['Entities'] if 'EnvironmentMapComponent' in e] + [sphere]
        scene['Scene'] = 'PT1211_' + material
        p = scene['PostProcessSettings']
        p.update(GpuPathTracerEnabled=False, GpuPathTracerMaxBounces=5,
                 GpuPathTracerRussianRouletteStartBounce=0, GpuPathTracerMaxRadianceClamp=0,
                 GpuPathTracerRayEpsilon=.001, GpuPathTracerMaxRayDistance=10000,
                 GpuPathTracerUniformEnvironmentRadiance=[0, 0, 0],
                 GpuPathTracerEnvironmentCubeIntensity=1, GpuPathTracerSampleTextures=False,
                 GpuPathTracerNextEventEstimation=False, GpuPathTracerMaxSamples=args.oracle_spp,
                 GpuPathTracerSamplesPerFrame=8)
        p['ReSTIRPT'] = dict(Enabled=True, TemporalReuse=False, SpatialReuse=False,
            InitialCandidates=8, MappingMask=1, Seed=args.seeds[0], SpatialRadius=4,
            ConfidenceCap=8, RayEpsilon=.001, NormalBias=.001, MaxRayDistance=10000,
            RadianceClamp=0, DebugView=0)
        path = HERE / (args.prefix + '-' + material + '.olo')
        path.write_text(yaml.safe_dump(scene, sort_keys=False), encoding='utf-8', newline='\n')
        scenes[material] = path
        manifest = dict(ManifestVersion=1, Id='pt1211-'+material, Product='diagnostic',
            Scene=str(path), Backends={'Supported': ['vulkan']},
            Camera=dict(Id='sphere', Position=POSES[0], YawDegrees=0, PitchDegrees=0,
                        FovDegrees=45, Near=.05, Far=300),
            Output={'Resolution': [160, 90], 'RenderScale': 1},
            RendererSettings=dict(Path='Deferred', EnableDDGI=False, TAAEnabled=False,
                                  GpuPathTracerEnabled=False),
            Exposure={'Mode': 'Manual', 'Exposure': 1},
            Determinism={'Seed': args.seeds[0], 'StartTimeSeconds': 12, 'FixedDtSeconds': 1/60},
            Warmup={'Frames': 16},
            Attachments=[{'Name': 'PTRadiance', 'Source': TARGET, 'Format': 'hdr'},
                         {'Name': 'Beauty', 'Source': 'UIComposite', 'Format': 'png'}])
        (HERE / (args.prefix + '-' + material + '.yaml')).write_text(yaml.safe_dump(manifest, sort_keys=False), encoding='utf-8', newline='\n')
    return scenes


def unwrap(raw):
    if 'error' in raw:
        raise RuntimeError(raw['error'])
    result = raw.get('result', raw)
    if result.get('isError'):
        raise RuntimeError(result)
    if 'structuredContent' in result:
        return result['structuredContent']
    for block in result.get('content', []):
        if block.get('type') == 'text':
            try:
                return json.loads(block['text'])
            except json.JSONDecodeError:
                pass
    return result


class Harness:
    def __init__(self, args):
        self.args = args
        self.out = HERE / args.output
        self.out.mkdir(parents=True, exist_ok=True)
        self.client = Client(args.port)
        self.start = time.monotonic()
        self.log = (self.out / 'records.jsonl').open('a', encoding='utf-8', newline='\n')
        self.seq = 0

    def call(self, name, payload=None, image=False):
        if time.monotonic() - self.start > self.args.max_seconds:
            raise TimeoutError('Run deadline exceeded between tool calls')
        self.seq += 1
        raw = self.client.tool(name, payload, out=str(self.out / f'{self.seq:05d}-{name}') if image else None)
        value = unwrap(raw)
        self.log.write(json.dumps(dict(sequence=self.seq, tool=name, arguments=payload,
            elapsed=time.monotonic()-self.start, response=value)) + '\n')
        self.log.flush()
        return value

    def setting(self, field, value):
        return self.call('olo_postprocess_settings_set', {'field': field, 'value': value})

    def pose(self, position):
        self.call('olo_camera_set_pose', {'position': position, 'target': [0, 0, 0], 'fov': 45})

    def stats(self, name):
        value = self.call('olo_render_target_stats', {'name': name, 'rect': ROI, 'forceFrame': True})
        assert value['mipWidth'] == 160 and value['mipHeight'] == 90, value
        assert not value['meta'].get('stale', False), value
        channels = value['channels']
        assert len(channels) == 4, value
        for c in channels:
            assert c['finiteCount'] == 256 and not c.get('nanCount') and not c.get('infCount'), value
        return value

    def diagnostics(self):
        return {name: self.call(name) for name in ['olo_restir_pt_stats', 'olo_pathtracer_stats',
            'olo_rt_scene_stats', 'olo_perf_snapshot', 'olo_perf_pass_timings']}

    def oracle(self, position, seed):
        self.setting('ReSTIRPTEnabled', False)
        self.setting('GpuPathTracerSeed', seed)
        self.setting('GpuPathTracerEnabled', True)
        self.pose(position)
        for _ in range(self.args.oracle_spp + 16):
            s = self.stats('PathTracerAccum')
            a = s['channels'][3]
            if a['min'] == a['max'] and a['min'] >= self.args.oracle_spp:
                albedo = self.stats('PathTracerAlbedo')
                assert albedo['channels'][3]['min'] >= self.args.oracle_spp, 'ROI contains misses'
                self.diagnostics()
                return [c['mean']/a['mean'] for c in s['channels'][:3]]
        raise RuntimeError('Oracle did not reach uniform requested sample count')

    def ensemble(self, mode, mask, seed, poses):
        self.setting('GpuPathTracerEnabled', False)
        self.setting('ReSTIRPTEnabled', True)
        for k, v in [('TemporalReuse', MODES[mode][0]), ('SpatialReuse', MODES[mode][1]),
                     ('MappingMask', mask), ('Seed', seed), ('DebugView', 0)]:
            self.setting('ReSTIRPT'+k, v)
        results = []
        for pose_index, position in enumerate(poses):
            self.pose(position)
            samples, frame_ids = [], set()
            # No scene reload / history reset between the three stepped poses.
            for _ in range(self.args.samples):
                s = self.stats(TARGET)
                frame = s['meta']['frameIndex']
                assert frame not in frame_ids, 'Duplicate frame readback'
                frame_ids.add(frame)
                assert s['channels'][3]['min'] == 1 and s['channels'][3]['max'] == 1, 'ROI not fully covered'
                samples.append([c['mean'] for c in s['channels'][:3]])
            diag = self.diagnostics()
            pt = diag['olo_restir_pt_stats']
            assert pt.get('active') and pt.get('countersValid'), pt
            assert diag['olo_rt_scene_stats']['resident']['tlasInstances'] > 0
            results.append(dict(mode=mode, mask=mask, seed=seed, pose=pose_index,
                frames=sorted(frame_ids), samples=samples,
                mean=[statistics.mean(s[c] for s in samples) for c in range(3)], diagnostics=diag))
        return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--matrix', action='store_true', help='4 modes x 4 masks x 3 stepped poses x materials/seeds')
    parser.add_argument('--materials', nargs='+', choices=['diffuse', 'glossy'], default=['diffuse'])
    parser.add_argument('--seeds', nargs='+', type=int, default=[1211, 974])
    parser.add_argument('--samples', type=int, default=8)
    parser.add_argument('--oracle-spp', type=int, default=128)
    parser.add_argument('--max-seconds', type=int, default=1200)
    parser.add_argument('--port', type=int, default=18311)
    parser.add_argument('--output', default='repro-smoke', help='Evidence directory under ignored captures root, or absolute path')
    parser.add_argument('--prefix', default='repro', help='Scratch scene/manifest filename prefix')
    parser.add_argument('--hdr', action='store_true', help='Separate representative benchmark capture after numeric runs; reloads scene')
    args = parser.parse_args()
    validate_seeds(parser, args.seeds)
    if not args.prefix or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.prefix):
        parser.error('--prefix must contain only letters, digits, underscore or hyphen')
    assert args.samples > 0 and args.oracle_spp > 0
    scenes = prepare(args)
    print('Prepared scratch scenes and RGBE manifests:', HERE)
    if not args.run:
        return
    h = Harness(args)
    all_results = []
    for material in args.materials:
        h.call('olo_scene_open', {'path': str(scenes[material])})
        h.call('olo_editor_debug_draw_set', {'category': 'all', 'enabled': False})
        h.call('olo_renderer_settings_set', {'setting': 'renderpath', 'value': 'deferred'})
        h.call('olo_renderer_settings_set', {'setting': 'msaa', 'value': '1'})
        h.call('olo_viewport_set_size', {'width': 160, 'height': 90})
        h.call('olo_render_list_targets')
        poses = POSES if args.matrix else POSES[:1]
        reference = {}
        for seed in args.seeds:
            for i, position in enumerate(poses):
                reference[seed, i] = h.oracle(position, seed + 100000)
        for mode in MODES if args.matrix else ['initial']:
            for mask in [1, 2, 4, 7] if args.matrix else [1]:
                for seed in args.seeds:
                    rows = h.ensemble(mode, mask, seed, poses)
                    for row in rows:
                        row.update(material=material, oracle=reference[seed, row['pose']])
                        row['difference'] = [a-b for a,b in zip(row['mean'], row['oracle'])]
                        all_results.append(row)
                    (h.out / 'results.json').write_text(json.dumps(all_results, indent=2)+'\n', encoding='utf-8', newline='\n')
        for debug in range(7):
            h.setting('ReSTIRPTDebugView', debug)
            h.call('olo_render_capture_target', {'name': TARGET, 'forceFrame': True, 'maxWidth': 160}, image=True)
        h.setting('ReSTIRPTDebugView', 0)
        if args.hdr:
            h.call('olo_benchmark_capture', {'manifest': str(HERE / (args.prefix + '-' + material + '.yaml')),
                   'outDir': str(h.out / ('rgbe-'+material))})
    groups = {}
    for r in all_results:
        groups.setdefault((r['material'], r['mode'], r['mask'], r['pose']), []).append(r)
    summary = []
    for key, rows in groups.items():
        differences = [r['difference'] for r in rows]
        summary.append(dict(material=key[0], mode=key[1], mask=key[2], pose=key[3],
            independent_seed_blocks=len(rows),
            mean_difference=[statistics.mean(d[c] for d in differences) for c in range(3)],
            difference_standard_error=[statistics.stdev(d[c] for d in differences)/math.sqrt(len(rows))
                if len(rows)>1 else None for c in range(3)]))
    (h.out / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8', newline='\n')
    print('Evidence:', h.out, '(no automatic unbiasedness verdict; seed blocks, not frames, define uncertainty)')

if __name__ == '__main__':
    main()
