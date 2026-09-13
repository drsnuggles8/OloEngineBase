"""Live open-corner ReSTIR PT reproduction; offline preparation unless --run.

Reuses #974 cube/sphere material and constant-furnace environment assets. There
are NO emitters or analytic lights. GPU MaxBounces5, RR/NEE off integrates escape
after up to four surface scatters, matching PT n=0..3 BSDF terminal support.
GPU MaxBounces2 supplies the one-scatter escape baseline: their difference
measures higher-order transport, not an invented closed-form reference.

GPU primary rays jitter within pixels while PT shades raster samples. Flat ROI
geometry removes normal interpolation differences but not all pixel quadrature
error. Use a smooth fully covered ROI; a discrepancy needs resolution checks.
This script provides stepped poses, not continuous motion. The isolated-sphere
motion script's rotationally invariant oracle does NOT apply to this corner.

Requires PyYAML and a Vulkan RT editor with authorized MCP writes for --run.
Example: python OloEngine/tests/scripts/capture_restir_pt_corner.py --run
  --prefix corner --output corner-smoke
Add --matrix --materials diffuse glossy --poses 3 --seeds 1211 974 5423 9821
for all modes/masks and independent seed blocks. Start with the default smoke.
Use a fresh output directory; existing records.jsonl appends. No editor launch.
Timings/counters retain independent frame provenance, RGBE is not float32 RGBA,
and no per-frame independence or automatic unbiasedness verdict is claimed.
"""
import argparse
import copy
import json
import statistics
import math
import yaml
import capture_restir_pt_furnace as furnace

POSES = [[0, 1.5, 3], [.15, 1.5, 3], [-.15, 1.5, 3]]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8', newline='\n')


def prepare(args):
    furnace.HERE.mkdir(parents=True, exist_ok=True)
    assets = furnace.ROOT / 'OloEditor/SandboxProject/Assets/Scenes/Benchmark'
    white = yaml.safe_load((assets/'MaterialLabFurnace.olo').read_text(encoding='utf-8'))
    lab = yaml.safe_load((assets/'MaterialLab.olo').read_text(encoding='utf-8'))
    patch = next(e for e in lab['Entities'] if e.get('TagComponent', {}).get('Tag') == 'PatchGray50')
    paths = {}
    for material in args.materials:
        scene = copy.deepcopy(white)
        scene['Scene'] = 'ReSTIRPTCorner-'+material
        scene['Entities'] = [e for e in scene['Entities'] if 'EnvironmentMapComponent' in e]
        # Unit cube extents +/-0.5, as used by #974's reference patches.
        for index, (tag, position, scale) in enumerate([
            ('Floor', [0, -.1, 0], [6, .2, 6]),
            ('RightWall', [1.35, 1, 0], [.1, 2, 4]),
            ('BackWall', [0, 1, -1.35], [4, 2, .1]),
        ]):
            entity = copy.deepcopy(patch)
            entity['Entity'] = 1211000100+index
            entity['TagComponent']['Tag'] = 'PTCorner'+tag
            entity['TransformComponent'].update(Translation=position, Rotation=[0, 0, 0], Scale=scale)
            m = entity['MaterialComponent']
            m.update(AlbedoColor=[.6, .6, .6], Metallic=0, Roughness=.8, PBRModel=1)
            if index == 0 and material == 'glossy':
                m.update(Metallic=.6, Roughness=.12)
            scene['Entities'].append(entity)
        settings = scene['PostProcessSettings']
        settings.update(GpuPathTracerEnabled=False, GpuPathTracerMaxBounces=5,
            GpuPathTracerRussianRouletteStartBounce=0, GpuPathTracerMaxRadianceClamp=0,
            GpuPathTracerRayEpsilon=.001, GpuPathTracerMaxRayDistance=10000,
            GpuPathTracerUniformEnvironmentRadiance=[0, 0, 0],
            GpuPathTracerEnvironmentCubeIntensity=1, GpuPathTracerSampleTextures=False,
            GpuPathTracerNextEventEstimation=False, GpuPathTracerMaxSamples=args.oracle_spp,
            GpuPathTracerSamplesPerFrame=8)
        settings['ReSTIRPT'] = dict(Enabled=True, TemporalReuse=False, SpatialReuse=False,
            InitialCandidates=8, MappingMask=1, Seed=args.seeds[0], SpatialRadius=4,
            ConfidenceCap=8, RayEpsilon=.001, NormalBias=.001, MaxRayDistance=10000,
            RadianceClamp=0, DebugView=0)
        path = furnace.HERE / (args.prefix+'-'+material+'.olo')
        path.write_text(yaml.safe_dump(scene, sort_keys=False), encoding='utf-8', newline='\n')
        paths[material] = path
    return paths


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', action='store_true')
    p.add_argument('--matrix', action='store_true', help='All4 modes x masks1/2/4/7; default initial+combined mask7')
    p.add_argument('--materials', nargs='+', choices=['diffuse', 'glossy'], default=['diffuse'])
    p.add_argument('--poses', type=int, choices=[1, 3], default=1)
    p.add_argument('--seeds', nargs='+', type=int, default=[1211, 974])
    p.add_argument('--samples', type=int, default=8)
    p.add_argument('--oracle-spp', type=int, default=128)
    p.add_argument('--prefix', default='corner')
    p.add_argument('--output', default='corner-smoke')
    p.add_argument('--port', type=int, default=18311)
    p.add_argument('--max-seconds', type=int, default=1800)
    args = p.parse_args()
    furnace.validate_seeds(p, args.seeds)
    if not args.prefix or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.prefix):
        p.error('--prefix must contain only letters, digits, underscore or hyphen')
    if args.samples <= 0 or args.oracle_spp <= 0:
        p.error('samples and oracle-spp must be positive')
    scenes = prepare(args)
    print('Prepared:', ', '.join(str(s) for s in scenes.values()))
    if not args.run:
        return
    h = furnace.Harness(args)
    rows = []
    references = []
    positions = POSES[:args.poses]
    for material, scene in scenes.items():
        h.call('olo_scene_open', {'path': str(scene)})
        h.call('olo_editor_debug_draw_set', {'category': 'all', 'enabled': False})
        h.call('olo_renderer_settings_set', {'setting': 'renderpath', 'value': 'deferred'})
        h.call('olo_renderer_settings_set', {'setting': 'msaa', 'value': '1'})
        h.call('olo_viewport_set_size', {'width': 160, 'height': 90})
        reference = {}
        for seed in args.seeds:
            for pose, position in enumerate(positions):
                # Same seed gives paired initial randoms; the transport depth
                # changes later draws. Capture exact full and truncated means.
                h.setting('GpuPathTracerMaxBounces', 5)
                full = h.oracle(position, seed+100000)
                h.setting('GpuPathTracerMaxBounces', 2)
                one_scatter = h.oracle(position, seed+100000)
                record = dict(material=material, seed=seed, pose=pose, full=full,
                    one_scatter=one_scatter, higher_order=[a-b for a,b in zip(full, one_scatter)])
                references.append(record)
                reference[seed, pose] = record
                write_json(h.out/'corner-oracles.json', references)
        h.setting('GpuPathTracerMaxBounces', 5)
        for mode in furnace.MODES if args.matrix else ['initial', 'combined']:
            for mask in [1, 2, 4, 7] if args.matrix else [7]:
                for seed in args.seeds:
                    for row in h.ensemble(mode, mask, seed, positions):
                        r = reference[seed, row['pose']]
                        row.update(material=material, oracle=r['full'], one_scatter=r['one_scatter'])
                        row['difference'] = [a-b for a,b in zip(row['mean'], r['full'])]
                        rows.append(row)
                    write_json(h.out/'corner-results.json', rows)
        # Images are separate representative diagnostic frames; no debug color
        # enters the numerical ensembles. Setting DebugView can reset history.
        for debug in range(7):
            h.setting('ReSTIRPTDebugView', debug)
            h.call('olo_render_capture_target', {'name': furnace.TARGET,
                'forceFrame': True, 'maxWidth': 160}, image=True)
        h.setting('ReSTIRPTDebugView', 0)
    groups = {}
    for row in rows:
        groups.setdefault((row['material'], row['mode'], row['mask'], row['pose']), []).append(row)
    summary = []
    for key, group in groups.items():
        def summarize(values):
            return dict(mean=[statistics.mean(v[c] for v in values) for c in range(3)],
                standard_error=[statistics.stdev(v[c] for v in values)/math.sqrt(len(values))
                    if len(values)>1 else None for c in range(3)])
        summary.append(dict(material=key[0], mode=key[1], mask=key[2], pose=key[3],
            independent_seed_blocks=len(group), difference=summarize([r['difference'] for r in group]),
            oracle_higher_order=summarize([[a-b for a,b in zip(r['oracle'],r['one_scatter'])] for r in group])))
    write_json(h.out/'corner-summary.json', summary)
    print('Evidence:', h.out, '(inspect higher-order signal and seed-block uncertainty; no closed-form verdict)')


if __name__ == '__main__':
    main()
