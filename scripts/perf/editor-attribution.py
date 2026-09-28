"""Collect repeated live CPU/GPU pass snapshots from an attached editor (#1488).

Launch with driver.ps1 -Action attach -AllowWrites -Config Debug|Release -Rhi
opengl|vulkan. Pass its discovery path; credentials never enter the output.
These are attribution samples, not independent full-frame regression runs.
"""
import argparse
import json
from pathlib import Path
import time
from editor_mcp import Client



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--discovery', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scene', required=True)
    parser.add_argument('--path', choices=['forward', 'forward+', 'deferred'], required=True)
    parser.add_argument('--config', choices=['Debug', 'Release'], required=True)
    parser.add_argument('--settle-seconds', type=float, default=30)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--samples', type=int, default=10)
    parser.add_argument('--camera', nargs=3, type=float)
    parser.add_argument('--yaw', type=float, default=0)
    parser.add_argument('--pitch', type=float, default=6)
    args = parser.parse_args()
    if args.repeats < 3 or args.samples < 2 or args.settle_seconds < 0:
        parser.error('require >=3 repeats, >=2 samples, nonnegative settling')
    if args.output.exists():
        parser.error('output exists; preserve previous measurements')
    path_token = {'forward': 'forward', 'forward+': 'forwardplus', 'deferred': 'deferred'}[args.path]
    client = Client(args.discovery)
    scene = client.tool('olo_scene_open', path=args.scene)
    client.wait_ready()
    result = {'scene': scene, 'config': args.config,
              'pathChange': client.tool('olo_renderer_settings_set', setting='renderpath', value=path_token),
              'measurementKind': 'live attribution snapshots, correlated within blocks',
              'readInterruptions': [], 'blocks': []}
    if result['pathChange']['value'] != path_token:
        raise RuntimeError(f"Requested {path_token}, renderer applied {result['pathChange']['value']}")

    def read(name, **arguments):
        deadline = time.monotonic() + 120
        while True:
            try:
                return client.tool(name, **arguments)
            except RuntimeError as error:
                if 'Timed out waiting for the editor main thread' not in str(error):
                    raise
                result['readInterruptions'].append({'tool': name, 'error': str(error),
                                                     'monotonicSeconds': time.perf_counter()})
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
                if time.monotonic() >= deadline:
                    raise
                time.sleep(1)
    client.wait_ready()
    result['viewport'] = client.tool('olo_viewport_set_size', width=1920, height=1080)
    if args.camera:
        client.tool('olo_camera_set_pose', position=args.camera, yaw=args.yaw, pitch=args.pitch, fov=50)
    time.sleep(args.settle_seconds)

    def verify_path():
        settings = read('olo_renderer_settings_set')['settings']
        actual = next(item['currentValue'] for item in settings if item['setting'] == 'renderpath')
        if actual != path_token:
            raise RuntimeError(f'Render path changed: requested {path_token}, active {actual}')
        return actual

    result['verifiedRenderPath'] = verify_path()
    result['sceneSummary'] = read('olo_scene_summary')
    result['camera'] = read('olo_camera_get')
    for repeat in range(args.repeats):
        samples = []
        block = {'repeat': repeat, 'samples': samples, 'complete': False}
        result['blocks'].append(block)
        for _ in range(args.samples):
            start = time.perf_counter()
            value = read('olo_perf_pass_timings')
            samples.append({'sample': value, 'rpcSeconds': time.perf_counter() - start,
                            'monotonicSeconds': time.perf_counter()})
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
            time.sleep(1)
        block['verifiedRenderPath'] = verify_path()
        block['snapshot'] = read('olo_perf_snapshot')
        block['history'] = read('olo_perf_frame_history', points=120)
        block['complete'] = True
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f'Saved {args.repeats} attribution blocks: {args.output}')


if __name__ == '__main__':
    main()
