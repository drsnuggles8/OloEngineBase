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


def enable_ray_traced_sun(call):
    """Opt every directional light into ray-traced shadows, in Edit mode.

    `raytracedshadows: on` only selects the TECHNIQUE. Each light carries its own
    DirectionalLightComponent.RayTracedShadows flag, and the reference fixture
    ships it false, so the pass reports zero ray-traced AND zero fallback lights
    and an RT cell proves nothing. The field write goes through the editor undo
    stack, so it is refused outside Edit mode.
    """
    call('olo_scene_stop')
    enabled = []
    for entity in call('olo_scene_list_entities').get('entities', []):
        fields = call('olo_entity_list_fields', {'entity': entity['id']})
        for component in fields.get('components', []):
            if component.get('component') != 'DirectionalLightComponent':
                continue
            if any(f.get('field') == 'RayTracedShadows' for f in component.get('fields', [])):
                call('olo_entity_set_field', {'entity': entity['id'],
                                              'component': 'DirectionalLightComponent',
                                              'field': 'RayTracedShadows', 'value': True})
                enabled.append(entity['id'])
    if not enabled:
        raise RuntimeError('no directional light exposes RayTracedShadows; an RT cell cannot be proven')
    return enabled


def rt_consumer_active(stats, settings):
    """Why this RT cell is not evidence, or None when it is.

    Checked in the order the failures actually happen: no device, no TLAS, a
    vegetation producer that refused work (which withholds the TLAS by design),
    and finally a shadow tier that reached no light at all.
    """
    if not stats.get('capability', {}).get('supported'):
        return 'ray tracing unsupported on this device'
    status = stats.get('availability', {}).get('status')
    if status != 'ready':
        return f'ray-tracing scene status is {status!r}, not ready'
    vegetation = stats.get('vegetation') or {}
    if not vegetation.get('ready'):
        return ('vegetation not ready: '
                f"{vegetation.get('refused')} refused of {vegetation.get('requested')} requested")
    shadows = next((s for s in settings if s.get('setting') == 'raytracedshadows'), {})
    if shadows.get('rayTracedLights', 0) < 1:
        return (f"no light reached the ray-traced tier (rayTracedLights="
                f"{shadows.get('rayTracedLights')}, fallbackLights={shadows.get('fallbackLights')})")
    return None


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
    rt_planned = any(cell['rt'] for cell in plan)
    if rt_planned:
        enable_ray_traced_sun(call)
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
                if cell['rt']:
                    # Re-read the light counters AFTER the settings settled; the
                    # set call reports the PREVIOUS frame's numbers. Kept under
                    # its own key: record['settings'] is the evidence that this
                    # cell's renderpath/msaa/upscale were actually applied, and
                    # an RT cell is exactly the one whose log must keep it.
                    record['settingsAfterSettle'] = [call('olo_renderer_settings_set',
                                                          {'setting': 'raytracedshadows', 'value': 'on'})]
                    record['rtScene'] = call('olo_rt_scene_stats')
                    rejected = rt_consumer_active(record['rtScene'], record['settingsAfterSettle'])
                    if rejected:
                        # Storing images here would file a raster frame as RT
                        # evidence. Record the reason and move on.
                        record['skipped'] = rejected
                        log.write(json.dumps(record) + chr(10))
                        log.flush()
                        print(ident, 'SKIPPED:', rejected, flush=True)
                        continue
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
