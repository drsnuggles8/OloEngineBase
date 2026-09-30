"""Run alternating AB/BA fresh-process benchmark captures from a declared plan.

Each arm specifies repo, exe, buildConfig, compiler, shaderRoot and assetRoot.
Each workload specifies manifest, expectedFrames (per camera), cameras, and gates.
Gates specify channel, statistic, tolerance and maxIntervalWidth. The plan also
declares pairs, familyAlpha, machine and cacheMode. Only warmed steady-state
captures qualify; startup time is retained separately. No universal time budget.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

from controlled_comparison import compare_pairs, median_interval
from capture_process import run_capture


def host_state() -> dict:
    if os.name != 'nt':
        raise ValueError('controlled host probe currently supports Windows only')
    script = """
    $busy = @(Get-Process cmake,ninja,MSBuild,clang-cl,cl,lld-link,OloEditor,OloEngine-Tests,OloRuntime,OloServer,devenv -ErrorAction SilentlyContinue |
      Select-Object ProcessName,Id)
    @{ cpu = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfLogicalProcessors)
       gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
       busyProcesses = $busy } | ConvertTo-Json -Depth 4 -Compress
    """
    state = json.loads(subprocess.check_output(['pwsh', '-NoProfile', '-Command', script], text=True))
    if not state['cpu'] or not state['gpu']:
        raise ValueError('CPU/GPU driver metadata unavailable')
    return state


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def tree_hash(path: Path) -> dict:
    files = sorted(p for p in path.rglob('*') if p.is_file())
    if not files:
        raise ValueError(f'empty provenance directory: {path}')
    records = {p.relative_to(path).as_posix(): sha256(p) for p in files}
    return {'root': str(path), 'files': records,
            'sha256': hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()}


def git(repo: Path, *args: str) -> bytes:
    return subprocess.check_output(['git', '-C', str(repo), *args])


def provenance(arm: dict) -> dict:
    repo, exe = Path(arm['repo']).resolve(), Path(arm['exe']).resolve()
    if arm['buildConfig'] != 'Release' or not arm['compiler']:
        raise ValueError('controlled captures require Release with declared compiler')
    diff = git(repo, 'diff', 'HEAD', '--binary')
    untracked = git(repo, 'ls-files', '--others', '--exclude-standard', '-z').decode().split('\0')
    return {**arm, 'binarySha256': sha256(exe),
            'commit': git(repo, 'rev-parse', 'HEAD').decode().strip(),
            'dirtyDiffSha256': hashlib.sha256(diff).hexdigest(),
            'dirtyDiff': diff.decode(errors='replace'),
            'untracked': {p: sha256(repo / p) for p in untracked if p},
            'shaders': tree_hash(Path(arm['shaderRoot'])),
            'assets': tree_hash(Path(arm['assetRoot']))}


def read_capture(directory: Path, workload: dict) -> tuple[dict, dict]:
    result = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
    if result['determinism'].get('warmupTimedOut'):
        raise ValueError('capture timed out')
    actual = result['output']['actual']
    if not actual or any(not math.isfinite(actual.get(k, 0)) or actual.get(k, 0) <= 0 for k in
                         ('renderWidth', 'renderHeight', 'displayWidth', 'displayHeight')):
        raise ValueError('actual internal/display resolution unavailable')
    # These captures do not yet carry consumption proof. Refuse an upscaler
    # claim rather than treating requested/selected settings as executed work.
    if result['configuration']['requested']['upscaleMode'] != 0:
        raise ValueError('upscaler consumption evidence unavailable in capture result')
    measurement = result['measurement']
    deadline = measurement['deadlineMs']
    if not math.isfinite(deadline) or deadline <= 0:
        raise ValueError('invalid deadline')
    grouped = {camera: {key: [] for key in
                       ('renderCallMs', 'cpuMs', 'fenceWaitMs', 'presentWaitMs', 'gpuMs')}
               for camera in workload['cameras']}
    editor_host = result['provenance']['host'] == 'editor-mcp'
    if editor_host:
        if not measurement.get('continuousEditorFrames') or measurement.get('traceOverflow'):
            raise ValueError('continuous completed editor-frame trace unavailable or overflowed')
        if measurement.get('completedSteps') != workload['expectedFrames'] * len(workload['cameras']):
            raise ValueError('truncated editor camera/motion steps')
        for samples in grouped.values():
            samples['frameTimeMs'] = samples['renderCallMs']
    recording_channels = ('recordingWallMs', 'recordingJoinWaitMs')
    excluded = {}
    seen_gpu = set()
    last_cpu = {}
    count = 0
    with (directory / measurement['rawFile']).open(newline='', encoding='utf-8') as stream:
        for row in csv.DictReader(stream):
            sample = grouped[row['camera']]
            if editor_host:
                cpu_frame = int(row['cpuFrameId'])
                previous = last_cpu.get(row['camera'])
                if cpu_frame <= 0 or (previous is not None and cpu_frame != previous + 1):
                    raise ValueError('editor CPU frame trace has a gap or duplicate')
                if int(row['index']) != len(sample['renderCallMs']):
                    raise ValueError('editor frame indices are not contiguous')
                last_cpu[row['camera']] = cpu_frame
            count += 1
            for key in ('renderCallMs', 'cpuMs', 'fenceWaitMs', 'presentWaitMs'):
                value = float(row[key])
                if not math.isfinite(value) or value < 0:
                    raise ValueError(f'invalid {key}')
                sample[key].append(value)
            for key in recording_channels:
                if key in row:
                    value = float(row[key])
                    if not math.isfinite(value) or value < 0:
                        raise ValueError(f'invalid {key}')
                    sample.setdefault(key, []).append(value)
            reason = row['gpuStatus']
            frame = row['gpuFrameId']
            if reason == 'valid':
                if not frame or int(frame) <= 0 or frame in seen_gpu:
                    reason = 'missing-or-duplicate-frame-id'
                elif not row['gpuMs']:
                    reason = 'missing-value'
                else:
                    value = float(row['gpuMs'])
                    if not math.isfinite(value) or value < 0:
                        reason = 'invalid-value'
                    else:
                        sample['gpuMs'].append(value)
                        seen_gpu.add(frame)
            if reason != 'valid':
                excluded[reason] = excluded.get(reason, 0) + 1
    if count != measurement['sampleCount']:
        raise ValueError('truncated capture disagrees with declared sample count')
    for samples in grouped.values():
        size = len(samples['renderCallMs'])
        if (editor_host and size < workload['expectedFrames']) or (not editor_host and size != workload['expectedFrames']):
            raise ValueError('truncated capture or unexpected per-camera frame count')
    if editor_host:
        ranges = {row['camera']: row for row in measurement['scenarios']}
        if set(ranges) != set(grouped):
            raise ValueError('editor trace camera ranges differ')
        for camera, samples in grouped.items():
            bounds = ranges[camera]
            size = len(samples['frameTimeMs'])
            if (size != bounds['sampleCount'] or last_cpu[camera] != bounds['lastCpuFrameId'] or
                    size != bounds['lastCpuFrameId'] - bounds['firstCpuFrameId'] + 1):
                raise ValueError('truncated editor trace boundary')
    pass_channels = sorted({gate['channel'] for gate in workload.get('gates', [])
                            if gate['channel'].startswith('passGpuMs:')})
    if pass_channels:
        pass_path = measurement.get('passRawFile')
        if not pass_path:
            raise ValueError('per-frame GPU pass samples unavailable')
        for samples in grouped.values():
            for channel in pass_channels:
                samples[channel] = []
        seen_rows, seen_passes = set(), set()
        with (directory / pass_path).open(encoding='utf-8') as stream:
            for line in stream:
                record = json.loads(line)
                camera, index, frame = record['camera'], record['index'], record['gpuFrameId']
                key = (camera, index)
                if key in seen_rows or camera not in grouped or not 0 <= index < len(grouped[camera]['renderCallMs']):
                    raise ValueError('duplicate or unexpected per-pass sample row')
                seen_rows.add(key)
                entries = {(p['parent'] + '/' + p['pass'] if p['isSubPass'] and not p['pass'].startswith(p['parent'] + '/') else p['pass']): p
                           for p in record['passes']}
                if len(entries) != len(record['passes']):
                    raise ValueError('ambiguous duplicate pass names')
                for channel in pass_channels:
                    name = channel.removeprefix('passGpuMs:')
                    entry = entries.get(name)
                    reason = entry['status'] if entry else 'absent-pass'
                    pass_key = (frame, name)
                    if reason == 'valid':
                        value = entry['gpuMs']
                        if frame <= 0 or pass_key in seen_passes:
                            reason = 'missing-or-duplicate-frame-id'
                        elif value is None or not math.isfinite(value) or value < 0:
                            reason = 'invalid-value'
                        else:
                            grouped[camera][channel].append(value)
                            seen_passes.add(pass_key)
                    if reason != 'valid':
                        label = channel + ':' + reason
                        excluded[label] = excluded.get(label, 0) + 1
        if len(seen_rows) != count:
            raise ValueError('truncated per-pass capture')
    signature = {'manifest': result['manifest']['sourceHashFnv1a64'],
                 'backend': result['provenance']['backend'],
                 'host': result['provenance']['host'], 'metric': measurement['metric'],
                 'gpu': result['provenance']['gpuRenderer'], 'actual': actual,
                 'configuration': result['configuration'], 'deadlineMs': deadline}
    return {'samples': grouped, 'gpuExcluded': excluded, 'deadlineMs': deadline}, signature


def scalar(samples: list[float], statistic: str, deadline: float) -> float:
    if len(samples) < 100:
        raise ValueError('fewer than 100 valid samples; unsupported capture')
    if statistic == 'deadlineMissFraction':
        return sum(v > deadline for v in samples) / len(samples)
    quantiles = {'p50': .50, 'p95': .95, 'p99': .99}
    if statistic not in quantiles:
        raise ValueError(f'unknown statistic: {statistic}')
    if statistic == 'p99' and len(samples) < 1000:
        raise ValueError('p99 gate requires at least 1000 valid frames per run')
    return sorted(samples)[math.ceil(quantiles[statistic] * len(samples)) - 1]


def execute(plan: dict, output: Path) -> dict:
    if plan.get('controlled') is not True or plan['cacheMode'] != 'warm' or plan['machine'] != platform.node():
        raise ValueError('runner identity or warmed-cache declaration does not match')
    tests = sum(len(w['cameras']) * len(w['gates']) for w in plan['workloads'])
    if not tests:
        raise ValueError('no gates')
    alpha = plan['familyAlpha'] / tests  # Bonferroni across all declared gates
    median_interval([0.] * plan['pairs'], alpha)  # fail before expensive captures
    arms = {name: provenance(plan['arms'][name]) for name in ('A', 'B')}
    (output / 'provenance.json').write_text(json.dumps(arms, indent=2), encoding='utf-8')
    report = {'method': 'alternating AB/BA; sign interval over independent run-pair effects',
              'machine': platform.node(), 'os': platform.platform(),
              'familyAlpha': plan['familyAlpha'], 'gates': [], 'runs': []}
    for workload_index, workload in enumerate(plan['workloads']):
        captures = {'A': [], 'B': []}
        signature = None
        for pair in range(plan['pairs']):
            for name in ('A', 'B') if pair % 2 == 0 else ('B', 'A'):
                arm = plan['arms'][name]
                destination = output / f'workload-{workload_index}-pair-{pair:02d}-{name}'
                destination.mkdir()
                manifest = Path(arm['repo']) / workload['manifest']
                cache = Path(arm['shaderCache'])
                if not cache.is_dir() or not any(cache.iterdir()):
                    raise ValueError(f'warm shader cache must be primed: {cache}')
                environment = {**os.environ, 'OLO_SHADER_CACHE_DIR': str(cache.resolve())}
                injected = arm.get('injectDelayMs', 0)
                if not isinstance(injected, int) or not 0 <= injected <= 1000:
                    raise ValueError('injectDelayMs must be an integer in [0, 1000]')
                environment['OLO_CAPTURE_TEST_DELAY_MS'] = str(injected)
                before = host_state()
                if before['busyProcesses']:
                    raise ValueError(f'contended host before capture: {before}')
                start = time.perf_counter()
                with (destination / 'stdout.txt').open('w', encoding='utf-8') as out, \
                     (destination / 'stderr.txt').open('w', encoding='utf-8') as err:
                    process_result = run_capture(arm, manifest, destination, environment,
                                                 plan['timeoutSeconds'], out, err)
                row = {'pair': pair, 'arm': name, 'directory': str(destination),
                       **process_result, 'processWallSeconds': time.perf_counter() - start,
                       'manifestSha256': sha256(manifest),
                       'hostBefore': before, 'hostAfter': host_state()}
                report['runs'].append(row)
                (output / 'runs.json').write_text(json.dumps(report['runs'], indent=2), encoding='utf-8')
                if row['exitCode']:
                    raise ValueError(f'capture failed: {destination}')
                calibration_path = destination / 'calibration.json'
                if arm.get('host', 'test-binary') == 'editor-mcp':
                    calibration = {'injectedWallDelayMs': 0, 'scope': 'editor host; injection unsupported'}
                elif not calibration_path.exists():
                    raise ValueError('capture binary does not report negative-control provenance')
                else:
                    calibration = json.loads(calibration_path.read_text(encoding='utf-8'))
                if calibration['injectedWallDelayMs'] != injected:
                    raise ValueError('requested delay was not consumed by capture binary')
                row['calibration'] = calibration
                if row['hostAfter']['busyProcesses']:
                    raise ValueError(f'contended host after capture: {destination}')
                capture, current = read_capture(destination, workload)
                if current['host'] != arm.get('host', 'test-binary'):
                    raise ValueError('capture host differs from declared arm')
                if signature is not None and current != signature:
                    raise ValueError('workload, quality, resolution, GPU, or measurement interval differs')
                signature = current
                row['gpuExcluded'] = capture['gpuExcluded']
                (output / 'runs.json').write_text(json.dumps(report['runs'], indent=2), encoding='utf-8')
                captures[name].append(capture)
        for camera in workload['cameras']:
            for gate in workload['gates']:
                values = {name: [scalar(c['samples'][camera][gate['channel']], gate['statistic'],
                                       c['deadlineMs']) for c in captures[name]] for name in ('A', 'B')}
                decision = compare_pairs(values['A'], values['B'], tolerance=gate['tolerance'],
                                         max_interval_width=gate['maxIntervalWidth'], alpha=alpha,
                                         relative=gate['statistic'] != 'deadlineMissFraction')
                report['gates'].append({'workload': workload['manifest'], 'camera': camera,
                                        **gate, **decision})
    report['status'] = ('regression' if any(g['status'] == 'regression' for g in report['gates']) else
                        'inconclusive' if any(g['status'] == 'inconclusive' for g in report['gates']) else 'pass')
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        args.output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(json.dumps({'status': 'inconclusive', 'reason': str(error)}, indent=2))
        return 2
    try:
        plan = json.loads(args.plan.read_text(encoding='utf-8'))
        (args.output / 'plan.json').write_text(json.dumps(plan, indent=2), encoding='utf-8')
        report = execute(plan, args.output)
    except (ValueError, KeyError, TypeError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        report = {'status': 'inconclusive', 'reason': str(error)}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    return {'pass': 0, 'regression': 1, 'inconclusive': 2}[report['status']]


if __name__ == '__main__':
    raise SystemExit(main())
