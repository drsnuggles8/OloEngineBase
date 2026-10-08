"""Descriptive streaming capture report using the controlled runner's AB/BA protocol.

This instrument reports each run's empirical tails, never a regression verdict.
It reuses controlled-benchmark.py's capture validity and provenance checks. Missing
streaming telemetry is unavailable, not zero. Use controlled-benchmark.py for a
calibrated confidence gate; frame percentiles here are descriptive observations.
"""
from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import math
import os
import platform
import statistics
import subprocess
import time
from pathlib import Path


_spec = importlib.util.spec_from_file_location(
    'controlled_benchmark', Path(__file__).with_name('controlled-benchmark.py'))
controlled = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(controlled)

COUNTERS = ('residentCpuBytes', 'residentGpuBytes', 'pendingRequests',
            'fallbackDraws', 'evictions', 'reloads', 'pinnedGpuBytes', 'optionalGpuBytes',
            'retiringGpuBytes', 'stagingCpuBytes', 'canonicalCpuBytes', 'pendingCpuBytes',
            'deniedForResident', 'deniedForUpload')
REGION_COUNTERS = ('loaded', 'pending', 'evictions')
TRANSFERS = ('ioMs', 'uploadMs', 'ioBytes', 'uploadBytes')
SHARED_COUNTERS = ('optionalResidentGpuBytes', 'pinnedResidentGpuBytes', 'retiringGpuBytes',
                   'pendingCpuBytes', 'uploadedBytesThisFrame', 'pinnedUploadBytesThisFrame',
                   'maxResidentGpuBytes', 'maxUploadBytesPerFrame', 'maxStagingCpuBytes',
                   'unknownStagingCount', 'pendingLoads', 'completedUnretrievedLoads',
                   'abandonedRunningLoads', 'heldCompletedLoads', 'deniedForResident',
                   'deniedForUpload', 'deniedForStaging', 'preparationMicroseconds', 'readBytes', 'readMicroseconds',
                   'unknownReadCount', 'completedLoads', 'failedLoads',
                   'uploadMicroseconds', 'actualUploadedBytes')


def describe(values: list[float]) -> dict:
    if not values:
        return {'status': 'unavailable', 'reason': 'no valid samples'}
    if any(not math.isfinite(value) or value < 0 for value in values):
        raise ValueError('non-finite or negative descriptive sample')
    ordered = sorted(values)
    return {'status': 'observed', 'samples': len(values),
            **{name: ordered[math.ceil(q * len(ordered)) - 1]
               for name, q in (('p50', .50), ('p95', .95), ('p99', .99))},
            'max': ordered[-1],
            'p99Evidence': ('at least 1000 samples' if len(values) >= 1000 else
                            'fewer than 1000 samples; sparse descriptive tail')}


def telemetry(directory: Path, result: dict, cameras: list[str]) -> dict:
    """Read optional frame-keyed telemetry without inventing missing counters.

    Counters are instantaneous snapshots or cumulative counts. Consequently only
    min/max/last are reported; summing repeated cumulative counts would be wrong.
    Transfer cost fields are per observed interval and receive empirical tails.
    """
    unavailable = {'status': 'unavailable', 'reason': 'capture has no streamingRawFile'}
    filename = result['measurement'].get('streamingRawFile')
    if not filename:
        return unavailable
    path = (directory / filename).resolve()
    if not path.is_relative_to(directory.resolve()):
        raise ValueError('streaming telemetry path escapes capture directory')
    rows = {camera: [] for camera in cameras}
    seen = set()
    with path.open(encoding='utf-8') as stream:
        for line in stream:
            record = json.loads(line)
            camera, frame = record['camera'], record['cpuFrameId']
            declared_host = result.get('provenance', {}).get('host', 'editor-mcp')
            host = record.get('host', declared_host)
            if host != declared_host or camera not in rows or type(frame) is not int:
                raise ValueError('unknown camera or invalid telemetry frame')
            if host == 'test-binary':
                index = record.get('index')
                if frame != 0 or type(index) is not int or index < 0:
                    raise ValueError('test-host telemetry requires its explicit sample index')
                identity = index
            elif host == 'editor-mcp' and frame > 0:
                identity = frame
            else:
                raise ValueError('editor telemetry requires a positive CPU frame ID')
            key = camera, identity
            if key in seen:
                raise ValueError('duplicate streaming telemetry frame')
            seen.add(key)
            rows[camera].append((identity, record))
    expected = result['measurement'].get('sampleCount')
    if expected is not None and len(seen) != expected:
        raise ValueError('truncated streaming telemetry differs from frame sample count')
    if not seen:
        return {'status': 'unavailable', 'reason': 'streaming telemetry contains no samples'}
    summary = {}
    for camera, keyed_records in rows.items():
        records = [row for _, row in sorted(keyed_records, key=lambda entry: entry[0])]
        fields = {}
        # Preparation timing includes build and I/O; it is not pure disk latency.
        # Cumulative transfer figures are kept as snapshots, never summed.
        for name in SHARED_COUNTERS:
            values = [row[name] for row in records if name in row]
            if any(type(value) is not int or value < 0 for value in values):
                raise ValueError(f'invalid shared {name} counter')
            fields[name] = ({'status': 'observed', 'samples': len(values),
                             'min': min(values), 'max': max(values), 'last': values[-1]}
                            if values else {'status': 'unavailable'})
        for family in ('groom', 'vegetation'):
            fields[family] = {}
            for name in COUNTERS:
                values = [row[family][name] for row in records
                          if name in row.get(family, {})]
                if any(type(value) is not int or value < 0 for value in values):
                    raise ValueError(f'invalid {family}.{name} counter')
                fields[family][name] = ({'status': 'observed', 'samples': len(values),
                                         'min': min(values), 'max': max(values), 'last': values[-1]}
                                        if values else {'status': 'unavailable'})
        active_regions = [row['regions'] for row in records if row.get('regions') is not None]
        if any(not isinstance(value, dict) for value in active_regions):
            raise ValueError('invalid region streaming state')
        fields['regions'] = {'activeFrames': len(active_regions),
                             'inactiveOrUnavailableFrames': len(records) - len(active_regions)}
        for name in REGION_COUNTERS:
            values = [state[name] for state in active_regions if name in state]
            if any(type(value) is not int or value < 0 for value in values):
                raise ValueError(f'invalid regions.{name} counter')
            fields['regions'][name] = ({'status': 'observed', 'samples': len(values),
                                        'min': min(values), 'max': max(values), 'last': values[-1]}
                                       if values else {'status': 'unavailable'})
        for name in TRANSFERS:
            values = [row[name] for row in records if name in row]
            if any(type(value) not in (int, float) for value in values):
                raise ValueError(f'invalid {name} sample')
            fields[name] = describe(values)
        summary[camera] = {'observedFrames': len(records), **fields}
    return {'status': 'observed', 'rawFile': filename, 'cameras': summary}


def summarize(directory: Path, workload: dict) -> tuple[dict, dict]:
    channels = workload.get('channels', ['frameTimeMs', 'renderCallMs', 'cpuMs',
                                         'gpuMs', 'fenceWaitMs', 'presentWaitMs'])
    validation = {**workload, 'gates': [{'channel': channel} for channel in channels
                                       if channel.startswith('passGpuMs:')]}
    capture, signature = controlled.read_capture(directory, validation)
    result = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
    summary = {camera: {channel: describe(samples.get(channel, [])) for channel in channels}
               for camera, samples in capture['samples'].items()}
    memory = {camera: {channel: [] for channel in ('gpuResidentBytes', 'cpuTrackedBytes')}
              for camera in workload['cameras']}
    with (directory / result['measurement']['rawFile']).open(newline='', encoding='utf-8') as stream:
        for row in csv.DictReader(stream):
            for channel in ('gpuResidentBytes', 'cpuTrackedBytes'):
                if row.get(channel):
                    value = int(row[channel])
                    if value < 0:
                        raise ValueError(f'negative {channel}')
                    memory[row['camera']][channel].append(value)
    memory_summary = {camera: {channel: describe(values) for channel, values in fields.items()}
                      for camera, fields in memory.items()}
    return {'cameras': summary, 'gpuExcluded': capture['gpuExcluded'],
            'physicalMemory': memory_summary,
            'streaming': telemetry(directory, result, workload['cameras'])}, signature


def execute(plan: dict, output: Path) -> dict:
    if (plan.get('descriptive') is not True or plan['cacheMode'] != 'warm' or
            plan['machine'] != platform.node()):
        raise ValueError('descriptive/warm declaration or machine identity differs')
    if type(plan['pairs']) is not int or plan['pairs'] < 2:
        raise ValueError('at least two pairs required to exercise AB and BA orders')
    if not plan['workloads']:
        raise ValueError('no workloads')
    arms = {name: controlled.provenance(plan['arms'][name]) for name in ('A', 'B')}
    (output / 'provenance.json').write_text(json.dumps(arms, indent=2), encoding='utf-8')
    report = {'status': 'descriptive', 'method': 'fresh-process alternating AB/BA; per-run empirical tails',
              'claim': 'No significance, confidence interval, or regression verdict',
              'machine': platform.node(), 'os': platform.platform(), 'runs': [], 'pairedEffects': []}
    for index, workload in enumerate(plan['workloads']):
        signature = None
        paired = []
        for pair in range(plan['pairs']):
            pair_rows = {}
            for name in ('A', 'B') if pair % 2 == 0 else ('B', 'A'):
                arm = plan['arms'][name]
                if arm.get('injectDelayMs', 0):
                    raise ValueError('descriptive residency campaign does not inject artificial delay')
                destination = output / f'workload-{index}-pair-{pair:02d}-{name}'
                destination.mkdir()
                manifest = Path(arm['repo']) / workload['manifest']
                cache = Path(arm['shaderCache'])
                if not cache.is_dir() or not any(cache.iterdir()):
                    raise ValueError(f'warm shader cache must be primed: {cache}')
                environment = {**os.environ, 'OLO_SHADER_CACHE_DIR': str(cache.resolve()),
                               'OLO_CAPTURE_TEST_DELAY_MS': '0'}
                before = controlled.host_state()
                if before['busyProcesses']:
                    raise ValueError(f'contended host before capture: {before}')
                start = time.perf_counter()
                with (destination / 'stdout.txt').open('w', encoding='utf-8') as out, \
                     (destination / 'stderr.txt').open('w', encoding='utf-8') as err:
                    process = controlled.run_capture(arm, manifest, destination, environment,
                                                     plan['timeoutSeconds'], out, err)
                row = {'pair': pair, 'arm': name, 'workload': workload['manifest'],
                       'directory': str(destination), **process,
                       'processWallSeconds': time.perf_counter() - start,
                       'manifestSha256': controlled.sha256(manifest),
                       'hostBefore': before, 'hostAfter': controlled.host_state()}
                report['runs'].append(row)
                (output / 'runs.json').write_text(json.dumps(report['runs'], indent=2), encoding='utf-8')
                if row['exitCode'] or row['hostAfter']['busyProcesses']:
                    raise ValueError(f'failed or contended capture: {destination}')
                summary, current = summarize(destination, workload)
                if current['host'] != arm.get('host', 'test-binary'):
                    raise ValueError('capture host differs from declared arm')
                if signature is not None and signature != current:
                    raise ValueError('workload, quality, GPU, resolution or interval differs')
                signature = current
                row.update(summary)
                pair_rows[name] = summary
            paired.append(pair_rows)
        for camera in workload['cameras']:
            for channel in workload.get('channels', ['frameTimeMs', 'renderCallMs', 'cpuMs', 'gpuMs']):
                for tail in ('p50', 'p95', 'p99'):
                    effects = [rows['B']['cameras'][camera][channel][tail] -
                               rows['A']['cameras'][camera][channel][tail] for rows in paired
                               if all(rows[name]['cameras'][camera][channel]['status'] == 'observed'
                                      for name in ('A', 'B'))]
                    report['pairedEffects'].append({'workload': workload['manifest'], 'camera': camera,
                        'channel': channel, 'statistic': tail, 'pairs': len(effects),
                        'BminusA': effects, 'medianBminusA': statistics.median(effects) if effects else None})
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--summarize-capture', type=Path,
                        help='Report one existing capture with the first plan workload; no launch')
    args = parser.parse_args()
    try:
        args.output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(json.dumps({'status': 'unavailable', 'reason': str(error)}, indent=2))
        return 2
    try:
        plan = json.loads(args.plan.read_text(encoding='utf-8'))
        (args.output / 'plan.json').write_text(json.dumps(plan, indent=2), encoding='utf-8')
        if args.summarize_capture:
            summary, signature = summarize(args.summarize_capture, plan['workloads'][0])
            report = {'status': 'descriptive', 'signature': signature, **summary}
        else:
            report = execute(plan, args.output)
    except (ValueError, KeyError, TypeError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        report = {'status': 'unavailable', 'reason': str(error)}
    if args.output.is_dir():
        (args.output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    return 0 if report['status'] == 'descriptive' else 2


if __name__ == '__main__':
    raise SystemExit(main())
