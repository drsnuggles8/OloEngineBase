"""Optional live editor verification for #1240; offline plan by default.

Launch the editor with the requested backend and MCP write consent, then use
--run --backend vulkan|opengl --port <port> --output <new directory>.
Captures are live render-target readbacks. RPC delivery is asynchronous; this
does not claim exact frame pairing. Fixed-time artifact captures use the engine
benchmark harness separately. No credentials or discovery data are logged.
"""
import argparse
import base64
import itertools
import json
import time
from pathlib import Path

from restir_pt_capture_client import Client


def unwrap(raw):
    result = raw.get('result', raw)
    if 'error' in raw or result.get('isError'):
        raise RuntimeError(raw.get('error', result))
    if result.get('structuredContent'):
        return result['structuredContent']
    for block in result.get('content', []):
        if block.get('type') == 'text':
            try:
                return json.loads(block['text'])
            except json.JSONDecodeError:
                pass
    return result


def cases(backend):
    for path in ['forward', 'forwardplus', 'deferred']:
        samples = [1, 2, 4, 8] if path == 'deferred' else [1]
        rt = [False, True] if backend == 'vulkan' and path == 'deferred' else [False]
        for sample, upscale, size, enabled in itertools.product(
                samples, ['off', 'quality', 'balanced', 'performance', 'ultraperformance'],
                [(960, 540), (1280, 720)], rt):
            yield dict(path=path, samples=sample, upscale=upscale, size=size, rt=enabled)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--backend', choices=['opengl', 'vulkan'], required=True)
    parser.add_argument('--port', type=int, default=21540)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--smoke', action='store_true', help='One native single-sample cell per path, plus Vulkan RT')
    args = parser.parse_args()
    plan = list(cases(args.backend))
    if args.smoke:
        plan = [c for c in plan if c['samples'] == 1 and c['upscale'] == 'off' and c['size'] == (960, 540)]
    if not args.run:
        print(json.dumps(plan, indent=2))
        return
    args.output.mkdir(parents=True, exist_ok=False)
    client = Client(args.port)
    call = lambda name, values=None: unwrap(client.tool(name, values))
    call('olo_scene_open', {'path': 'Scenes/FoliageHierarchicalWind.olo'})
    call('olo_scene_simulate')
    call('olo_editor_pause', {'paused': True})
    call('olo_editor_debug_draw_set', {'category': 'all', 'enabled': False})
    call('olo_camera_set_pose', {'position': [128, 14, 196], 'target': [128, 5, 128], 'fov': 45})
    with (args.output / 'cells.jsonl').open('w', encoding='utf-8') as log:
        for cell in plan:
            ident = f"{cell['path']}-msaa{cell['samples']}-{cell['upscale']}-{cell['size'][0]}-rt{int(cell['rt'])}"
            record = dict(cell, id=ident, backend=args.backend, wallTime=time.time())
            try:
                call('olo_viewport_set_size', {'width': cell['size'][0], 'height': cell['size'][1]})
                changes = [('renderpath', cell['path']), ('msaa', str(cell['samples'])),
                           ('upscale', cell['upscale']), ('raytracedshadows', 'on' if cell['rt'] else 'off')]
                record['settings'] = [call('olo_renderer_settings_set', {'setting': key, 'value': value}) for key, value in changes]
                call('olo_postprocess_settings_set', {'field': 'RTReflectionEnabled', 'value': cell['rt']})
                time.sleep(.4)
                record['rtScene'] = call('olo_rt_scene_stats')
                record['performance'] = call('olo_perf_snapshot')
                record['timings'] = call('olo_perf_pass_timings')
                record['targets'] = call('olo_render_list_targets')
                # SceneColor preserves geometry evidence independently of the
                # post chain. Also capture the final UI composition so upscale
                # settings have an observable output, rather than only metadata.
                for target in ['SceneColor', 'UIComposite']:
                    raw = client.tool('olo_render_capture_target', {'name': target, 'forceFrame': True, 'maxWidth': 960})
                    metadata = unwrap(raw)
                    images = [b for b in raw.get('result', {}).get('content', []) if b.get('type') == 'image']
                    if not images:
                        raise RuntimeError(f'{target}: no image: {metadata}')
                    image = args.output / f'{ident}-{target}.png'
                    image.write_bytes(base64.b64decode(images[0]['data']))
                    record[target] = dict(path=image.name, metadata=metadata)
            except Exception as error:
                record['error'] = str(error)
                log.write(json.dumps(record)+'\n')
                log.flush()
                raise
            log.write(json.dumps(record)+'\n')
            log.flush()
            print(ident, 'complete', flush=True)


if __name__ == '__main__':
    main()
