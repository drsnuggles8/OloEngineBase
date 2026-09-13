"""Asynchronous continuous-motion evidence; no editor access without --run.
Requires an existing scratch diffuse.olo/glossy.olo from capture_restir_pt_furnace.py.
Camera requests target 20Hz; actual delivery cadence is logged, not assumed.

Example: python OloEngine/tests/scripts/capture_restir_pt_motion.py --run
  --prefix repro --output repro-motion --reference <furnace-output>/results.json
Use --matrix for modes initial/temporal/spatial/combined mask7 and combined1/2/4.
Requires prior matching scratch preparation by capture_restir_pt_furnace.py.
No exact camera/frame synchronization or every-frame capture is claimed. Tool
serialization may coalesce requests. A tessellated sphere can have orientation
error even though the ideal sphere integral is rotation invariant. Numeric
sampling and moving debug-image phases are separate; debug settings may reset
history. Per-frame samples correlate, so no independent-frame error bar is used.
The default zero-secondary scene cannot prove multibounce mapping correctness.
With --corner, use a scene prepared by capture_restir_pt_corner.py and its prefix.
This performs a lateral multibounce camera sweep, with no per-frame GPU oracle
comparison. --reference is rejected in that mode. Original 100-sphere scenes
need separate framing. Use a fresh --output directory for each run.
"""
import argparse
import json
import math
import statistics
import threading
import time
from pathlib import Path
import capture_restir_pt_furnace as furnace


def camera_worker(client, stop, failed, path, hz, degrees_per_second, radius, corner=False):
    started = time.monotonic()
    deadline = started
    with path.open('w', encoding='utf-8', newline='\n') as log:
        while not stop.is_set():
            before = time.monotonic()
            angle = math.radians(degrees_per_second) * (before-started)
            position = ([.3*math.sin(angle), 1.5, 3] if corner else
                        [radius*math.sin(angle), 0, radius*math.cos(angle)])
            payload = {'position': position, 'target': [0, 0, 0], 'fov': 45}
            row = dict(requestMonotonic=before, requestWallTime=time.time(),
                       elapsed=before-started, arguments=payload)
            try:
                row['response'] = furnace.unwrap(client.tool('olo_camera_set_pose', payload))
            except Exception as error:
                row['error'] = repr(error)
                failed.set()
                stop.set()
            row['responseMonotonic'] = time.monotonic()
            log.write(json.dumps(row)+'\n')
            log.flush()
            # Do not burst to catch up after a slow request. This is a target rate.
            deadline = max(deadline + 1/hz, time.monotonic())
            stop.wait(max(0, deadline-time.monotonic()))


def reference_mean(path, material):
    if path is None:
        return None
    rows = json.loads(Path(path).read_text(encoding='utf-8'))
    # Deduplicate oracle repeated across mode/mask cases; stationary pose0 only.
    by_seed = {r['seed']: r['oracle'] for r in rows
               if r['material'] == material and r['pose'] == 0}
    if not by_seed:
        raise ValueError('No matching stationary pose0 oracle records')
    return dict(independentSeeds=list(by_seed),
                mean=[statistics.mean(v[c] for v in by_seed.values()) for c in range(3)])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', action='store_true')
    p.add_argument('--corner', action='store_true', help='Use a prepared open-corner scene and a lateral camera sweep; no stationary oracle comparison')
    p.add_argument('--matrix', action='store_true', help='modes initial/temporal/spatial/combined mask7, then combined masks1/2/4')
    p.add_argument('--seconds', type=float, default=10)
    p.add_argument('--hz', type=float, default=20)
    p.add_argument('--degrees-per-second', type=float, default=12)
    p.add_argument('--material', choices=['diffuse', 'glossy'], default='diffuse')
    p.add_argument('--seeds', nargs='+', type=int, default=[1211])
    p.add_argument('--reference', help='capture_furnace results.json; matching scene/settings required')
    p.add_argument('--output', default='repro-motion-smoke', help='Evidence directory under ignored captures root, or absolute path')
    p.add_argument('--prefix', default='repro', help='Prefix of furnace script generated scene files')
    p.add_argument('--port', type=int, default=18311)
    p.add_argument('--max-seconds', type=int, default=1200)
    args = p.parse_args()
    furnace.validate_seeds(p, args.seeds)
    if args.corner and args.reference:
        p.error('--corner cannot use the rotationally invariant sphere reference')
    if not args.prefix or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.prefix):
        p.error('--prefix must contain only letters, digits, underscore or hyphen')
    if not 0 < args.seconds <= 120 or not 0 < args.hz <= 60:
        p.error('seconds must be (0,120], hz (0,60]')
    cases = [(m, 7) for m in furnace.MODES] + [('combined', m) for m in [1, 2, 4]] if args.matrix else [('combined', 7)]
    plan = dict(material=args.material, secondsPerCase=args.seconds, requestedCameraHz=args.hz,
        cases=cases, seeds=args.seeds, ROI=furnace.ROI,
        trajectory=({'kind': 'lateral', 'amplitude': .3, 'height': 1.5, 'z': 3}
                    if args.corner else {'kind': 'orbit', 'radius': 4}),
        notes=('Asynchronous requests: no exact camera/frame pairing. No every-frame claim. '
               + ('Open corner: multibounce, no per-frame oracle comparison.' if args.corner else 'Sphere zero-secondary only.')))
    print(json.dumps(plan, indent=2))
    if not args.run:
        return
    scene = furnace.HERE / (args.prefix + '-' + args.material + '.olo')
    if not scene.is_file():
        raise FileNotFoundError('Prepare scratch scenes with capture_restir_pt_furnace.py first')
    reference = reference_mean(args.reference, args.material)
    h = furnace.Harness(args)
    (h.out/'plan.json').write_text(json.dumps(plan, indent=2)+'\n', encoding='utf-8', newline='\n')
    # Separate MCP sessions: worker never shares the main client's sequence state.
    camera_client = type(h.client)(args.port)
    h.call('olo_viewport_set_size', {'width': 160, 'height': 90})
    h.call('olo_scene_open', {'path': str(scene)})
    h.call('olo_editor_debug_draw_set', {'category': 'all', 'enabled': False})
    h.call('olo_renderer_settings_set', {'setting': 'renderpath', 'value': 'deferred'})
    h.call('olo_renderer_settings_set', {'setting': 'msaa', 'value': '1'})
    h.call('olo_viewport_set_size', {'width': 160, 'height': 90})
    h.setting('GpuPathTracerEnabled', False)
    for key, value in [('Enabled', True), ('RadianceClamp', 0), ('InitialCandidates', 8),
                       ('RayEpsilon', .001), ('NormalBias', .001), ('MaxRayDistance', 10000),
                       ('SpatialRadius', 4), ('ConfidenceCap', 8), ('DebugView', 0)]:
        h.setting('ReSTIRPT'+key, value)
    results = []
    for mode, mask in cases:
        for seed in args.seeds:
            case = f'{mode}-mask{mask}-seed{seed}'
            for key, value in [('TemporalReuse', furnace.MODES[mode][0]),
                               ('SpatialReuse', furnace.MODES[mode][1]), ('MappingMask', mask),
                               ('Seed', seed), ('DebugView', 0)]:
                h.setting('ReSTIRPT'+key, value)
            h.pose([0, 1.5, 3] if args.corner else [0, 0, 4])
            # Settle setup before timing the motion experiment, not between samples.
            h.stats(furnace.TARGET)
            initial = h.diagnostics()
            assert initial['olo_rt_scene_stats']['resident']['tlasInstances'] > 0, initial
            assert initial['olo_restir_pt_stats'].get('active'), initial
            stop = threading.Event()
            failed = threading.Event()
            worker = threading.Thread(target=camera_worker, args=(camera_client, stop, failed,
                h.out/(case+'-camera.jsonl'), args.hz, args.degrees_per_second, 4, args.corner), daemon=True)
            samples = []
            frames = set()
            started = time.monotonic()
            worker.start()
            try:
                with (h.out/(case+'-samples.jsonl')).open('w', encoding='utf-8', newline='\n') as log:
                    while time.monotonic()-started < args.seconds:
                        if stop.is_set():
                            raise RuntimeError('Camera worker failed; see camera log')
                        before = time.monotonic()
                        s = h.stats(furnace.TARGET)
                        frame = s['meta']['frameIndex']
                        assert frame not in frames, 'Repeated rendered frame'
                        frames.add(frame)
                        assert s['channels'][3]['min'] == s['channels'][3]['max'] == 1, 'ROI not fully covered'
                        counters = h.call('olo_restir_pt_stats')
                        assert counters.get('active') and counters.get('countersValid'), counters
                        row = dict(requestMonotonic=before, responseMonotonic=time.monotonic(),
                            stats=s, counters=counters, rgb=[c['mean'] for c in s['channels'][:3]])
                        log.write(json.dumps(row)+'\n')
                        log.flush()
                        samples.append(row)
                # Separate diagnostic phase, still moving. Debug color values are
                # never mixed into the numerical radiance ensemble above.
                for debug in [0, 1, 2, 3, 4, 5, 6]:
                    h.setting('ReSTIRPTDebugView', debug)
                    h.call('olo_render_capture_target', {'name': furnace.TARGET,
                        'forceFrame': True, 'maxWidth': 160}, image=True)
                h.setting('ReSTIRPTDebugView', 0)
                final = h.diagnostics()
            finally:
                stop.set()
                worker.join(timeout=1)
            if worker.is_alive():
                raise RuntimeError('Camera RPC remains in flight; no subsequent case started')
            if failed.is_set():
                raise RuntimeError('Camera worker failed; see camera log')
            assert samples, 'No rendered samples during interval'
            mean = [statistics.mean(s['rgb'][c] for s in samples) for c in range(3)]
            result = dict(case=case, sampleCount=len(samples), frames=sorted(frames),
                mean=mean, stationaryOracle=reference, initialDiagnostics=initial,
                finalDiagnostics=final, elapsedIncludingDiagnostics=time.monotonic()-started,
                difference=[a-b for a,b in zip(mean, reference['mean'])] if reference else None)
            results.append(result)
            (h.out/'motion-results.json').write_text(json.dumps(results, indent=2)+'\n', encoding='utf-8', newline='\n')
    print('Motion evidence:', h.out)

if __name__ == '__main__':
    main()
