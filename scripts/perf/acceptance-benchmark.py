"""Integrated AAA acceptance gate (#1259): interleaved, repeated, fresh-process
Release captures on the GL test host and the Vulkan editor host, screened for
GPU contention, summarised per cell as run-level ranges with a p95 frame-time
verdict against one gate.

Protocol
--------
* Release only, with a declared compiler. The plan's buildConfig must be
  "Release" and no binary may sit in a Debug/RelWithDebInfo/MinSizeRel output
  directory. The binary, shader tree and asset tree are fingerprinted at the
  start and re-checked before every run; a change aborts the campaign, so no
  cell ever mixes builds.
* Every round runs each cell once, in fresh processes, rotating the start cell
  by one per round. A run that is not counted (contended, failed, invalid) is
  retried at the end of its round, up to maxRetries extra attempts per cell.
* Before each launch the host must be quiet: no competing process (another
  OloEditor / OloEngine-Tests / OloRuntime / OloServer not launched by this
  driver, a build tool, a named game), no GPU process from nvidia-smi that was
  not there when the campaign started, and GPU utilisation at or below
  idleGpuUtilMax. The driver waits up to contentionWaitSeconds for that, else
  records the attempt as skipped. During the run the same probe is sampled
  every 30 s (and once after exit). A run that overlaps contention keeps its
  files on disk but is excluded from the summary, naming the process.
* Statistics are per run and per scenario (camera); across runs each one is
  reported as the range over counted runs. Frames inside one run are not
  independent, so there is no frame bootstrapping. Frame time is the
  `renderCallMs` column, which is a different quantity per host (GL test host:
  the Scene::OnUpdateEditor call; editor: the completed-frame interval), so a
  cell is one host and hosts are never pooled.
* Total GPU time counts only `valid` samples with distinct frame IDs. Per-pass
  GPU time counts only `valid` entries with a distinct (frame ID, pass);
  sub-passes are reported separately and never added to their parent. Every
  excluded sample is counted by reason.
* Verdict per cell: FAIL if any counted run has a scenario whose p95 frame time
  exceeds the gate; otherwise PASS when `repeats` runs counted, else
  INCONCLUSIVE. Exit code: 0 all cells pass, 1 any cell fails, 2 otherwise
  (inconclusive, refused or aborted).

Plan (JSON; relative paths are relative to "repo")
--------------------------------------------------
    {
      "exe": "build-cached/OloEngine/tests/Release/OloEngine-Tests.exe",
      "editorExe": "build-cached/OloEditor/Release/OloEditor.exe",
      "repo": "C:/repos/OloEngine",
      "buildConfig": "Release",
      "compiler": "clang-cl",
      "gateMs": 16.667,
      "repeats": 3,
      "maxRetries": 2,
      "timeoutSeconds": 1800,
      "mcpPort": 7391,
      "shaderCacheDir": null,
      "shaderRoot": "OloEditor/assets/shaders",
      "assetRoot": "OloEditor/SandboxProject/Assets",
      "extraCompetitors": [],
      "ignoreGpuProcesses": [],
      "idleGpuUtilMax": 30,
      "contentionWaitSeconds": 600,
      "cells": [
        {"id": "gl-deferred-vsm", "host": "gl",
         "manifest": "OloEditor/assets/benchmark/manifests/acceptance-deferred.yaml"},
        {"id": "vk-deferred-vsm", "host": "vulkan",
         "manifest": "OloEditor/assets/benchmark/manifests/acceptance-deferred.yaml"}
      ]
    }

A cell may add "requirePasses", e.g. ["VirtualShadowMapMarkPass"] (names as
they appear in measurement-passes.jsonl; a sub-pass as "Parent/Sub"): a run in
which a listed pass has no valid GPU sample in some scenario is not counted. Selected settings (configuration.selectedSettings)
say what the renderer was set to, not that the technique did any work.

Only buildConfig, compiler and cells are required; "exe" is required when a
cell uses host "gl" (the test binary with --olo-capture-manifest) and
"editorExe" when a cell uses host "vulkan" (the editor with --rhi=vulkan, driven
over MCP by capture_process.run_capture; its EditorPreferences.yaml must turn
the throttles off, see docs/guides/controlled-performance.md). Cell ids name
directories, so they are limited to letters, digits, '.', '_' and '-'.

Usage
-----
    python scripts/perf/acceptance-benchmark.py --plan plan.json --output C:/perf/acceptance-001
    python scripts/perf/acceptance-benchmark.py --plan plan.json --dry-run
    python scripts/perf/acceptance-benchmark.py --summarise-only --run-dir C:/perf/acceptance-001

--gate-ms, --repeats and --max-retries override the plan. --summarise-only
re-analyses every run from its files: --run-dir takes a campaign directory
(runs.json), a single driver run directory (driver-run.json) or a bare capture
directory (result.json; it has no contention record, so it is excluded unless
--include-unsampled is given). Run it under build-lock.ps1 so no cooperating
build starts mid-campaign.

Output: <output>/plan.json, provenance.json, runs.json (every attempt, counted
or not, and why), summary.json, summary.md, and one directory per attempt
<output>/<cell>/round-NN-attempt-K holding the capture, stdout/stderr and
driver-run.json.
"""
from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import importlib.util
import json
import math
import os
import platform
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))


def _load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


controlled = _load('controlled_benchmark', 'controlled-benchmark.py')
foliage = _load('foliage_cost_baseline', 'foliage-cost-baseline.py')

DEFAULT_GATE_MS = 16.667
MIN_REPEATS = 3
MIN_FRAMES = 100
ASSET_HASH_BUDGET_SECONDS = 60.0
SAMPLE_SECONDS = foliage.SAMPLE_SECONDS
COMPETITORS = list(foliage.COMPETITORS)
OTHER_CONFIGS = {'debug', 'relwithdebinfo', 'minsizerel'}
CELL_ID = re.compile(r'^[A-Za-z0-9._-]+$')

HOSTS = {
    'gl': {'captureHost': 'test-binary', 'backend': 'opengl', 'exeKey': 'exe',
           'frameMetric': 'Scene::OnUpdateEditor wall-clock call on the offscreen GL test host '
                          '(excludes present, editor UI and attachment readback)'},
    'vulkan': {'captureHost': 'editor-mcp', 'backend': 'vulkan', 'exeKey': 'editorExe',
               'frameMetric': 'completed editor-frame interval on the Vulkan editor host '
                              '(MCP sampling marshals may perturb it)'},
}

# (summary name, measurement.csv column). Frame time first: the gate reads it.
CHANNELS = (('frameTime', 'renderCallMs'), ('cpu', 'cpuMs'), ('fenceWait', 'fenceWaitMs'),
            ('presentWait', 'presentWaitMs'), ('recordingWall', 'recordingWallMs'),
            ('recordingJoinWait', 'recordingJoinWaitMs'))
REQUIRED_COLUMNS = ('camera', 'index', 'renderCallMs', 'cpuMs', 'fenceWaitMs', 'presentWaitMs',
                    'gpuFrameId', 'gpuMs', 'gpuStatus')
SUMMARY_CHANNELS = ('frameTime', 'cpu', 'gpu', 'fenceWait', 'presentWait', 'recordingWall', 'recordingJoinWait')

# Manifest RendererSettings key -> (configuration.requested key, configuration.selectedSettings key).
# Only settings the manifest pins are compared: an unpinned one is echoed as a
# default in `requested` and says nothing about what the run was asked to do.
SETTING_PINS = (('Path', 'path', 'path'), ('MSAASampleCount', 'msaaSamples', 'msaaSamples'),
                ('Upscale', 'upscaleMode', 'upscaleMode'),
                ('UpscaleTechnique', 'upscaleTechnique', 'upscaleTechnique'),
                ('RayTracedShadowsEnabled', 'rayTracedShadows', 'rayTracedShadowsRequested'),
                ('VirtualShadowMaps', 'virtualShadowMaps', 'virtualShadowMaps'),
                ('LightingTap', 'lightingTap', 'lightingTap'))


class BuildChanged(RuntimeError):
    """A binary, shader or asset fingerprint changed mid-campaign."""


def now_iso() -> str:
    return datetime.datetime.now().isoformat(timespec='seconds')


def read_json(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=1), encoding='utf-8')


# --------------------------------------------------------------------------- plan

def load_plan(raw: dict, *, default_repo: Path, gate_ms=None, repeats=None, max_retries=None,
              check_files: bool = True) -> dict:
    if raw.get('buildConfig') != 'Release':
        raise ValueError('acceptance captures require buildConfig "Release"; the plan declares '
                         + repr(raw.get('buildConfig')))
    compiler = raw.get('compiler')
    if not isinstance(compiler, str) or not compiler.strip():
        raise ValueError('the plan must declare the compiler that built the binaries')
    repo = Path(raw.get('repo') or default_repo).resolve()

    def resolve(value) -> Path:
        path = Path(value)
        return (path if path.is_absolute() else repo / path).resolve()

    gate = raw.get('gateMs', DEFAULT_GATE_MS) if gate_ms is None else gate_ms
    if isinstance(gate, bool) or not isinstance(gate, (int, float)) or not math.isfinite(gate) or gate <= 0:
        raise ValueError(f'gateMs must be a positive number, got {gate!r}')
    count = raw.get('repeats', MIN_REPEATS) if repeats is None else repeats
    if isinstance(count, bool) or not isinstance(count, int) or count < MIN_REPEATS:
        raise ValueError(f'repeats must be an integer >= {MIN_REPEATS}, got {count!r}')
    retries = raw.get('maxRetries', 2) if max_retries is None else max_retries
    if isinstance(retries, bool) or not isinstance(retries, int) or retries < 0:
        raise ValueError(f'maxRetries must be a non-negative integer, got {retries!r}')
    cells = raw.get('cells')
    if not isinstance(cells, list) or not cells:
        raise ValueError('the plan needs a non-empty "cells" list')
    seen = set()
    normalised_cells = []
    for cell in cells:
        cell_id = cell.get('id') if isinstance(cell, dict) else None
        if not isinstance(cell_id, str) or not CELL_ID.match(cell_id):
            raise ValueError(f'cell id must match {CELL_ID.pattern}: {cell!r}')
        if cell_id in seen:
            raise ValueError(f'duplicate cell id {cell_id}')
        seen.add(cell_id)
        if cell.get('host') not in HOSTS:
            raise ValueError(f'cell {cell_id}: host must be one of {sorted(HOSTS)}, got {cell.get("host")!r}')
        if not cell.get('manifest'):
            raise ValueError(f'cell {cell_id}: no manifest')
        manifest = resolve(cell['manifest'])
        if check_files and not manifest.is_file():
            raise ValueError(f'cell {cell_id}: manifest not found: {manifest}')
        required = cell.get('requirePasses', [])
        if not isinstance(required, list) or not all(isinstance(name, str) and name for name in required):
            raise ValueError(f'cell {cell_id}: requirePasses must be a list of pass names')
        normalised_cells.append({'id': cell_id, 'host': cell['host'], 'manifest': str(manifest),
                                 'requirePasses': required})
    plan = {'repo': str(repo), 'buildConfig': 'Release', 'compiler': compiler.strip(), 'gateMs': float(gate),
            'repeats': count, 'maxRetries': retries,
            'timeoutSeconds': float(raw.get('timeoutSeconds', 1800)), 'mcpPort': int(raw.get('mcpPort', 7391)),
            'shaderCacheDir': str(resolve(raw['shaderCacheDir'])) if raw.get('shaderCacheDir') else None,
            'shaderRoot': str(resolve(raw.get('shaderRoot', 'OloEditor/assets/shaders'))),
            'assetRoot': str(resolve(raw.get('assetRoot', 'OloEditor/SandboxProject/Assets'))),
            'competitors': COMPETITORS + [str(n) for n in raw.get('extraCompetitors', [])],
            'ignoreGpuProcesses': [str(n) for n in raw.get('ignoreGpuProcesses', [])],
            'idleGpuUtilMax': float(raw.get('idleGpuUtilMax', 30)),
            'contentionWaitSeconds': float(raw.get('contentionWaitSeconds', 600)),
            'cells': normalised_cells}
    for key in sorted({HOSTS[c['host']]['exeKey'] for c in normalised_cells}):
        if not raw.get(key):
            raise ValueError(f'a cell needs "{key}" but the plan does not name it')
        binary = resolve(raw[key])
        check_binary_config(binary)
        if check_files and not binary.is_file():
            raise ValueError(f'{key} not found: {binary}')
        plan[key] = str(binary)
    return plan


def check_binary_config(path: Path) -> None:
    other = {part.lower() for part in Path(path).parts[:-1]} & OTHER_CONFIGS
    if other:
        raise ValueError(f'{path} sits in a {sorted(other)[0]} output directory; acceptance captures are Release only')


def schedule(cell_ids: list, repeats: int) -> list:
    """Round r runs every cell once, starting at cell r mod n."""
    count = len(cell_ids)
    return [[cell_ids[(r + k) % count] for k in range(count)] for r in range(repeats)]


# ------------------------------------------------------------------- statistics

def nearest_rank(values: list, q: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered), max(1, math.ceil(q * len(ordered)))) - 1]


def describe(values: list, gate_ms: float | None = None) -> dict | None:
    if not values:
        return None
    row = {'count': len(values), 'p50Ms': nearest_rank(values, 0.50), 'p95Ms': nearest_rank(values, 0.95),
           'p99Ms': nearest_rank(values, 0.99), 'maxMs': max(values)}
    if gate_ms is not None:
        row['deadlineMisses'] = sum(1 for value in values if value > gate_ms)
    return row


def bump(counter: dict, key: str) -> None:
    counter[key] = counter.get(key, 0) + 1


# ---------------------------------------------------------------- run analysis

def actual_sizes(result: dict) -> dict:
    actual = (result.get('output') or {}).get('actual')
    if not isinstance(actual, dict):
        raise ValueError('output.actual is absent: the capture did not measure its internal/display size; '
                         'refusing the run')
    sizes = {}
    for key in ('renderWidth', 'renderHeight', 'displayWidth', 'displayHeight'):
        value = actual.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            raise ValueError(f'output.actual.{key} is absent or not positive ({value!r}); refusing the run')
        sizes[key] = int(value)
    upscaler = actual.get('upscaler')
    return {'render': [sizes['renderWidth'], sizes['renderHeight']],
            'display': [sizes['displayWidth'], sizes['displayHeight']],
            'renderScale': actual.get('renderScale'), 'internalSource': actual.get('internalSource'),
            'upscaler': ({'resolved': upscaler.get('resolved'), 'fallback': upscaler.get('fallback')}
                         if isinstance(upscaler, dict) else None),
            'upscalerLatched': isinstance(upscaler, dict)}


def manifest_pins(manifest_text: str) -> dict:
    """Top-level keys under RendererSettings in a (block-style) manifest."""
    pins = {}
    inside = False
    for line in manifest_text.splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        if not line[0].isspace():
            inside = line.rstrip() == 'RendererSettings:'
            continue
        match = re.match(r'^  ([A-Za-z]+):\s*(.*?)\s*$', line) if inside else None
        if match:
            pins[match.group(1)] = match.group(2).strip('\'"')
    return pins


def technique_flags(result: dict, manifest_text: str | None) -> tuple:
    configuration = result.get('configuration') or {}
    requested = configuration.get('requested') or {}
    selected = configuration.get('selectedSettings') or {}
    problems = []
    if manifest_text is None:
        problems.append('manifest.yaml echo absent; pinned renderer settings cannot be checked')
        pins = {}
    else:
        pins = manifest_pins(manifest_text)
    for manifest_key, requested_key, selected_key in SETTING_PINS:
        if manifest_key not in pins:
            continue
        if manifest_key == 'UpscaleTechnique' and requested.get('upscaleMode') in (0, None):
            continue  # the technique of a disabled upscaler is not a setting the run used
        if selected_key not in selected:
            problems.append(f'manifest pins {manifest_key} but selectedSettings.{selected_key} is not recorded')
        elif requested.get(requested_key) != selected[selected_key]:
            problems.append(f'selected {selected_key}={selected[selected_key]!r} differs from requested '
                            f'{requested_key}={requested.get(requested_key)!r}')
    flags = {'requested': requested, 'selected': selected,
             'pinned': sorted(k for k, _, _ in SETTING_PINS if k in pins),
             'virtualShadowMapsSelected': selected.get('virtualShadowMaps')}
    return flags, problems


def read_frames(path: Path, gate_ms: float) -> tuple:
    """Per-camera channel samples and total-GPU samples from measurement.csv."""
    cameras = {}
    seen_gpu = set()
    rows = 0
    with path.open(newline='', encoding='utf-8') as stream:
        reader = csv.DictReader(stream)
        header = set(reader.fieldnames or [])
        missing = [column for column in REQUIRED_COLUMNS if column not in header]
        if missing:
            raise ValueError(f'{path.name} lacks columns {missing}')
        for row in reader:
            rows += 1
            entry = cameras.setdefault(row['camera'], {
                'values': {name: ([] if column in header else None) for name, column in CHANNELS},
                'gpu': [], 'gpuExcluded': {}, 'indices': [], 'cpuFrameIds': []})
            for name, column in CHANNELS:
                if entry['values'][name] is None:
                    continue
                value = float(row[column])
                if not math.isfinite(value) or value < 0:
                    raise ValueError(f'{path.name} row {rows}: invalid {column} {row[column]!r}')
                entry['values'][name].append(value)
            entry['indices'].append(int(row['index']))
            if row.get('cpuFrameId'):
                entry['cpuFrameIds'].append(int(row['cpuFrameId']))
            status = row['gpuStatus'] or 'missing-status'
            frame = row['gpuFrameId']
            reason = None
            if status != 'valid':
                reason = status
            elif not frame.isdigit() or int(frame) <= 0:
                reason = 'missing-frame-id'
            elif frame in seen_gpu:
                reason = 'duplicate-frame-id'
            elif not row['gpuMs']:
                reason = 'missing-value'
            else:
                value = float(row['gpuMs'])
                if not math.isfinite(value) or value < 0:
                    reason = 'invalid-value'
                else:
                    entry['gpu'].append(value)
                    seen_gpu.add(frame)
            if reason:
                bump(entry['gpuExcluded'], reason)
    return cameras, rows


def pass_key(entry: dict) -> str:
    name = str(entry.get('pass', ''))
    parent = str(entry.get('parent') or '')
    if entry.get('isSubPass') and parent and not name.startswith(parent + '/'):
        return parent + '/' + name
    return name


def aggregate_passes(records, frame_counts: dict) -> tuple:
    """Valid per-pass GPU samples per camera.

    An entry counts once per distinct (gpuFrameId, pass) and only with status
    `valid` and a finite non-negative value; everything else is counted under
    its reason. Sub-passes go to `subPasses` with their parent and are never
    added to the parent's samples. Returns (per camera, excluded by reason,
    excluded by pass and reason, rows read)."""
    out = {camera: {'passes': {}, 'subPasses': {}, 'parents': {}} for camera in frame_counts}
    excluded = {}
    by_pass = {}
    seen_rows = set()
    seen = set()
    for record in records:
        camera, index = record.get('camera'), record.get('index')
        if (camera not in out or not isinstance(index, int) or not 0 <= index < frame_counts[camera]
                or (camera, index) in seen_rows):
            raise ValueError(f'unexpected or duplicate per-pass row {camera!r}/{index!r}')
        seen_rows.add((camera, index))
        frame = record.get('gpuFrameId')
        for entry in record.get('passes') or []:
            key = pass_key(entry)
            sub = bool(entry.get('isSubPass'))
            value = entry.get('gpuMs')
            status = entry.get('status')
            if status != 'valid':
                reason = 'status:' + str(status)
            elif isinstance(frame, bool) or not isinstance(frame, int) or frame <= 0:
                reason = 'missing-frame-id'
            elif (frame, key) in seen:
                reason = 'duplicate-frame-pass'
            elif (isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value)
                  or value < 0):
                reason = 'invalid-value'
            else:
                reason = None
            if reason:
                bump(excluded, reason)
                bump(by_pass.setdefault(key, {}), reason)
                continue
            seen.add((frame, key))
            out[camera]['subPasses' if sub else 'passes'].setdefault(key, []).append(float(value))
            if sub:
                out[camera]['parents'][key] = str(entry.get('parent') or '')
    return out, excluded, by_pass, len(seen_rows)


def memory_summary(result: dict) -> dict:
    measurement = result.get('measurement') or {}
    out = {key: measurement.get(key) for key in ('peakGpuResidentBytes', 'liveGpuResidentBytes',
                                                  'peakCpuTrackedBytes', 'gpuResidentBytesAfterSceneRelease')}
    report = result.get('memory')
    if not isinstance(report, dict):
        out['report'] = None
        return out
    gpu, cpu = report.get('gpu') or {}, report.get('cpu') or {}
    backend = report.get('backend') or {}
    owners = report.get('owners') or []

    def gpu_bytes(row):
        return (row.get('gpuLiveBytes') or 0) + (row.get('gpuRetiringBytes') or 0)

    out['report'] = {
        'gpuPeakBytes': gpu.get('peakBytes'), 'gpuLiveBytes': gpu.get('liveBytes'),
        'gpuResidentBytes': gpu.get('residentBytes'), 'cpuPeakBytes': cpu.get('peakBytes'),
        'cpuLiveBytes': cpu.get('liveBytes'), 'backend': backend.get('name'),
        'reconciliation': (backend.get('reconciliation') or {}).get('status'),
        'residency': (backend.get('residency') or {}).get('status'),
        'topGpuOwners': [{'owner': row.get('owner'), 'gpuBytes': gpu_bytes(row), 'count': row.get('count')}
                         for row in sorted(owners, key=gpu_bytes, reverse=True)[:10] if gpu_bytes(row)],
        'topCpuOwners': [{'owner': row.get('owner'), 'cpuBytes': row.get('cpuLiveBytes') or 0, 'count': row.get('count')}
                         for row in sorted(owners, key=lambda r: r.get('cpuLiveBytes') or 0, reverse=True)[:5]
                         if row.get('cpuLiveBytes')]}
    return out


def analyse_run(directory: Path, gate_ms: float) -> dict:
    """Per-scenario distributions of one capture directory.

    Raises ValueError when the directory cannot be read as a measurement at
    all (including absent actual sizes). Protocol problems that leave the
    numbers readable but not countable go to `invalidReasons`."""
    directory = Path(directory)
    result = read_json(directory / 'result.json')
    version = result.get('resultSchemaVersion')
    if not isinstance(version, int) or version < 2:
        raise ValueError(f'result schema {version!r} predates per-pass validity (need >= 2)')
    sizes = actual_sizes(result)
    measurement = result.get('measurement')
    if not isinstance(measurement, dict) or not measurement.get('rawFile'):
        raise ValueError('result.json has no measurement block; the manifest measured no frames')
    invalid = []
    calibration = directory / 'calibration.json'
    if calibration.exists() and read_json(calibration).get('injectedWallDelayMs', 0):
        invalid.append('negative-control wall delay was injected (calibration evidence, not a measurement)')
    if (result.get('determinism') or {}).get('warmupTimedOut'):
        invalid.append('warm-up or measurement timed out')
    manifest_file = directory / 'manifest.yaml'
    techniques, problems = technique_flags(
        result, manifest_file.read_text(encoding='utf-8') if manifest_file.exists() else None)
    invalid += problems
    provenance = result.get('provenance') or {}
    host = provenance.get('host')
    cameras, rows = read_frames(directory / measurement['rawFile'], gate_ms)
    if rows != measurement.get('sampleCount'):
        invalid.append(f'truncated capture: {rows} rows, result.json declares {measurement.get("sampleCount")}')
    if host == 'editor-mcp':
        if not measurement.get('continuousEditorFrames') or measurement.get('traceOverflow'):
            invalid.append('continuous completed editor-frame trace unavailable or overflowed')
        for camera, entry in cameras.items():
            ids = entry['cpuFrameIds']
            if (len(ids) != len(entry['indices']) or any(i <= 0 for i in ids)
                    or any(b != a + 1 for a, b in zip(ids, ids[1:]))):
                invalid.append(f'{camera}: editor CPU frame trace has a gap or duplicate')
            if entry['indices'] != list(range(len(entry['indices']))):
                invalid.append(f'{camera}: editor frame indices are not contiguous')
    pass_file = measurement.get('passRawFile')
    frame_counts = {camera: len(entry['indices']) for camera, entry in cameras.items()}
    passes, pass_excluded, pass_excluded_by_pass = {}, {}, {}
    if not pass_file or not (directory / pass_file).exists():
        invalid.append('per-pass GPU samples absent (measurement.passRawFile)')
    else:
        with (directory / pass_file).open(encoding='utf-8') as stream:
            records = (json.loads(line) for line in stream if line.strip())
            passes, pass_excluded, pass_excluded_by_pass, pass_rows = aggregate_passes(records, frame_counts)
        if pass_rows != rows:
            invalid.append(f'truncated per-pass capture: {pass_rows} of {rows} rows')
    scenarios = {}
    for camera, entry in sorted(cameras.items()):
        if len(entry['indices']) < MIN_FRAMES:
            invalid.append(f'{camera}: {len(entry["indices"])} measured frames; at least {MIN_FRAMES} required')
        scenario = {'frames': len(entry['indices'])}
        for name, _ in CHANNELS:
            values = entry['values'][name]
            scenario[name] = None if values is None else describe(values, gate_ms)
        scenario['gpu'] = describe(entry['gpu'], gate_ms)
        scenario['gpuExcluded'] = entry['gpuExcluded']
        camera_passes = passes.get(camera, {'passes': {}, 'subPasses': {}, 'parents': {}})
        scenario['passes'] = {name: describe(values) for name, values in sorted(camera_passes['passes'].items())}
        scenario['subPasses'] = {name: {**describe(values), 'parent': camera_passes['parents'][name]}
                                 for name, values in sorted(camera_passes['subPasses'].items())}
        scenarios[camera] = scenario
    manifest = result.get('manifest') or {}
    return {'host': host, 'backend': provenance.get('backend'), 'gpuRenderer': provenance.get('gpuRenderer'),
            'commitSha': provenance.get('commitSha'), 'frameMetric': measurement.get('metric'),
            'manifestId': manifest.get('id'), 'manifestSourceHash': manifest.get('sourceHashFnv1a64'),
            'sizes': sizes, 'techniques': techniques, 'memory': memory_summary(result),
            'gpuStatusExcluded': {camera: entry['gpuExcluded'] for camera, entry in cameras.items()},
            'passExcluded': pass_excluded, 'passExcludedByPass': pass_excluded_by_pass,
            'scenarios': scenarios, 'invalidReasons': invalid}


# ---------------------------------------------------------------- contention

def _stem(name) -> str:
    base = os.path.basename(str(name or '')).lower()
    return base[:-4] if base.endswith('.exe') else base


def descendants(processes: list, root_pid: int) -> set:
    """root_pid and every process it started, transitively. A child must be no
    older than its parent, so a reused parent PID does not adopt strangers."""
    created = {p['pid']: p.get('created', 0) for p in processes}
    children = {}
    for p in processes:
        children.setdefault(p['ppid'], []).append(p['pid'])
    out = {root_pid}
    stack = [root_pid]
    while stack:
        parent = stack.pop()
        for child in children.get(parent, []):
            if child not in out and child != parent and created.get(child, 0) >= created.get(parent, 0):
                out.add(child)
                stack.append(child)
    return out


def reduce_sample(when: str, processes: list, gpu_apps, gpu_util, *, driver_pid: int, competitors: list,
                  gpu_baseline: set, ignore: list) -> dict:
    """One probe, reduced to what counts as contention: competitor processes
    not descended from this driver, and GPU processes absent from the
    campaign-start baseline (None when nvidia-smi is unavailable)."""
    own = descendants(processes, driver_pid)
    names = {c.lower() for c in competitors}
    ignored = {_stem(i) for i in ignore}
    foreign = [{'pid': p['pid'], 'name': p['name'], 'path': p.get('path') or ''}
               for p in processes if p['pid'] not in own and _stem(p['name']) in names]
    new_gpu = None
    if gpu_apps is not None:
        new_gpu = [{'pid': a['pid'], 'path': a['path']} for a in gpu_apps
                   if a['pid'] not in own and a['path'].lower() not in gpu_baseline
                   and _stem(a['path']) not in ignored and _stem(a['path']) not in names]
    return {'time': when, 'gpuUtilization': gpu_util, 'foreignProcesses': foreign, 'newGpuProcesses': new_gpu}


def contention_reasons(samples: list) -> list:
    """One reason per competing image (a build runs dozens of clang-cl), naming
    it, its first PID and path, the instance count and when it was first seen."""
    reasons = []
    groups = {}
    for sample in samples:
        if sample.get('error'):
            reasons.append(f"probe failed at {sample['time']}: {sample['error']} (contention unverifiable)")
            continue
        found = [('competing process', p['name'], p) for p in sample['foreignProcesses']]
        found += [('new GPU process', os.path.basename(p['path']) or p['path'], p)
                  for p in sample['newGpuProcesses'] or []]
        for kind, label, p in found:
            group = groups.setdefault((kind, label.lower()), {'kind': kind, 'label': label, 'pids': [],
                                                              'path': p.get('path') or 'path unknown',
                                                              'time': sample['time']})
            if p['pid'] not in group['pids']:
                group['pids'].append(p['pid'])
    for group in groups.values():
        count = len(group['pids'])
        instances = '' if count == 1 else f'{count} instances, first '
        reasons.append(f"{group['kind']} {group['label']} ({instances}pid {group['pids'][0]}, {group['path']}) "
                       f"first seen at {group['time']}")
    return reasons


def prelaunch_reasons(sample: dict, idle_util_max: float) -> list:
    reasons = contention_reasons([sample])
    util = sample.get('gpuUtilization')
    if util is not None and util > idle_util_max:
        reasons.append(f'GPU utilisation {util:.0f}% before launch exceeds the idle limit {idle_util_max:.0f}%')
    return reasons


PROCESS_SCRIPT = ("Get-CimInstance Win32_Process | ForEach-Object { '{0}|{1}|{2}|{3}|{4}' -f "
                  "$_.ProcessId,$_.ParentProcessId,"
                  "$(if ($_.CreationDate) { $_.CreationDate.ToFileTimeUtc() } else { 0 }),"
                  "$_.Name,$_.ExecutablePath }")


def list_processes() -> list:
    completed = subprocess.run(['pwsh', '-NoProfile', '-Command', PROCESS_SCRIPT], capture_output=True,
                               text=True, timeout=120)
    if completed.returncode:
        raise RuntimeError('process probe failed: ' + completed.stderr.strip())
    processes = []
    for line in completed.stdout.splitlines():
        parts = line.split('|', 4)
        if len(parts) == 5 and parts[0].isdigit():
            processes.append({'pid': int(parts[0]), 'ppid': int(parts[1] or 0), 'created': int(parts[2] or 0),
                              'name': parts[3], 'path': parts[4]})
    if not processes:
        raise RuntimeError('process probe returned no processes')
    return processes


def list_gpu_apps():
    """GPU processes from nvidia-smi, or None when it is unavailable."""
    try:
        completed = subprocess.run(['nvidia-smi', '--query-compute-apps=pid,process_name', '--format=csv,noheader'],
                                   capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if completed.returncode:
        return None
    apps = []
    for line in completed.stdout.splitlines():
        pid, _, path = line.partition(',')
        if pid.strip().isdigit():
            apps.append({'pid': int(pid), 'path': path.strip()})
    return apps


def gpu_utilisation(readings: int = 1):
    values = []
    for index in range(readings):
        try:
            completed = subprocess.run(['nvidia-smi', '--query-gpu=utilization.gpu', '--format=csv,noheader,nounits'],
                                       capture_output=True, text=True, timeout=30)
            values.append(float(completed.stdout.splitlines()[0]))
        except (OSError, subprocess.TimeoutExpired, ValueError, IndexError):
            return None
        if index + 1 < readings:
            time.sleep(1)
    return sorted(values)[len(values) // 2]


class LiveProbe:
    """Windows probe bound to one campaign's competitor list and GPU baseline."""

    def __init__(self, plan: dict):
        self.competitors = plan['competitors']
        self.ignore = plan['ignoreGpuProcesses']
        apps = list_gpu_apps()
        names = {c.lower() for c in self.competitors}
        self.gpu_baseline = (None if apps is None else
                             sorted({a['path'].lower() for a in apps if _stem(a['path']) not in names}))

    def __call__(self, util_readings: int = 1) -> dict:
        return reduce_sample(now_iso(), list_processes(), list_gpu_apps(), gpu_utilisation(util_readings),
                             driver_pid=os.getpid(), competitors=self.competitors,
                             gpu_baseline=set(self.gpu_baseline or []), ignore=self.ignore)


class Sampler:
    """Probe at entry, every `interval` seconds while the body runs, and at exit."""

    def __init__(self, probe, interval: float = SAMPLE_SECONDS):
        self.samples = []
        self._probe = probe
        self._interval = interval
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)

    def _safe(self) -> dict:
        try:
            return self._probe()
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            return {'time': now_iso(), 'error': str(error)}

    def _loop(self) -> None:
        while not self._stop.wait(self._interval):
            self.samples.append(self._safe())

    def __enter__(self):
        self.samples.append(self._safe())
        self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._stop.set()
        self._thread.join()
        self.samples.append(self._safe())


def wait_for_quiet(probe, idle_util_max: float, wait_seconds: float, poll: float = SAMPLE_SECONDS,
                   sleep=time.sleep, clock=time.monotonic) -> tuple:
    start = clock()
    history = []
    while True:
        try:
            sample = probe(util_readings=3)
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            sample = {'time': now_iso(), 'error': str(error)}
        reasons = prelaunch_reasons(sample, idle_util_max)
        history.append({'sample': sample, 'reasons': reasons})
        if not reasons:
            return True, history
        if clock() - start >= wait_seconds:
            return False, history
        sleep(poll)


# ---------------------------------------------------------------- provenance

def sha256_file(path: Path) -> str:
    return controlled.sha256(Path(path))


def digest_records(records: dict) -> str:
    return hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()


def _locate(name: str, bases: list):
    for base in bases:
        candidate = (base / name).resolve()
        if candidate.is_file():
            return candidate
    return None


PATH_TOKEN = re.compile(r'[A-Za-z0-9_.\-]+(?:/[A-Za-z0-9_.\-]+)+\.[A-Za-z][A-Za-z0-9]{1,7}')


def manifest_references(manifest: Path, repo: Path, asset_root: Path) -> tuple:
    """Files a manifest names: its Scene, its Assets[].Path entries (with the
    declared Sha256), and path strings inside the scene that resolve to files.
    Returns ({path: declared sha or None}, [unresolved names])."""
    text = Path(manifest).read_text(encoding='utf-8')
    bases = [asset_root, asset_root.parent, repo / 'OloEditor', repo]
    references = {}
    unresolved = []
    current = None
    for line in text.splitlines():
        match = re.match(r'^\s*-\s*Path:\s*(\S.*?)\s*$', line)
        if match:
            current = _locate(match.group(1).strip('\'"'), bases)
            if current is None:
                unresolved.append(match.group(1))
            else:
                references.setdefault(current, None)
            continue
        match = re.match(r'^\s+Sha256:\s*([0-9a-fA-F]{64})\s*$', line)
        if match and current is not None:
            references[current] = match.group(1).lower()
    scene_match = re.search(r'^Scene:\s*(\S.*?)\s*$', text, re.M)
    if scene_match:
        scene = _locate(scene_match.group(1).strip('\'"'), bases)
        if scene is None:
            unresolved.append(scene_match.group(1))
        else:
            references.setdefault(scene, None)
            for token in sorted(set(PATH_TOKEN.findall(scene.read_text(encoding='utf-8', errors='replace')))):
                found = _locate(token, bases)
                if found is not None:
                    references.setdefault(found, None)
    return references, unresolved


def _relative(path: Path, repo: Path) -> str:
    try:
        return Path(path).resolve().relative_to(Path(repo).resolve()).as_posix()
    except ValueError:
        return Path(path).resolve().as_posix()


def referenced_asset_hash(repo: Path, references: dict, unresolved: list, reason: str) -> dict:
    files = {}
    mismatches = []
    for path, declared in sorted(references.items()):
        digest = sha256_file(path)
        files[_relative(path, repo)] = digest
        if declared and declared != digest:
            mismatches.append({'path': _relative(path, repo), 'declared': declared, 'actual': digest})
    return {'mode': 'manifest-referenced', 'reason': reason, 'fileCount': len(files), 'files': files,
            'sha256': digest_records(files), 'unresolved': sorted(set(unresolved)),
            'declaredSha256Mismatches': mismatches,
            'limitation': 'only files named by the manifests (Scene, Assets[].Path) and path strings in their '
                          'scenes are hashed; assets reached only through an AssetHandle are not covered'}


def asset_provenance(root: Path, repo: Path, references: dict, unresolved: list,
                     budget_seconds: float = ASSET_HASH_BUDGET_SECONDS, clock=time.monotonic) -> dict:
    """The whole asset tree's hash, or, when that takes longer than the budget,
    the hash of the files the manifests reference (and the output says so)."""
    start = clock()
    files = sorted(p for p in Path(root).rglob('*') if p.is_file())
    if not files:
        raise ValueError(f'empty asset root: {root}')
    records = {}
    for index, path in enumerate(files):
        if clock() - start > budget_seconds:
            return referenced_asset_hash(repo, references, unresolved,
                                         f'full hash of {root} exceeded {budget_seconds:g} s after {index} of '
                                         f'{len(files)} files')
        records[path.relative_to(root).as_posix()] = sha256_file(path)
    return {'mode': 'full-tree', 'root': str(root), 'fileCount': len(records),
            'seconds': round(clock() - start, 1), 'files': records, 'sha256': digest_records(records)}


def fingerprint(plan: dict, assets_mode: dict, references: dict, unresolved: list) -> dict:
    binaries = {key: sha256_file(Path(plan[key])) for key in ('exe', 'editorExe') if plan.get(key)}
    shaders = controlled.tree_hash(Path(plan['shaderRoot']))['sha256']
    if assets_mode['mode'] == 'full-tree':
        assets = asset_provenance(Path(plan['assetRoot']), Path(plan['repo']), references, unresolved,
                                  budget_seconds=float('inf'))['sha256']
    else:
        assets = referenced_asset_hash(Path(plan['repo']), references, unresolved, '')['sha256']
    return {'binaries': binaries, 'shaders': shaders, 'assets': assets}


def safe_probe_host() -> dict:
    try:
        return foliage.probe_host()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        return {'error': str(error)}


def build_provenance(plan: dict, budget_seconds: float) -> tuple:
    repo = Path(plan['repo'])
    references, unresolved = {}, []
    for manifest in sorted({cell['manifest'] for cell in plan['cells']}):
        found, missing = manifest_references(Path(manifest), repo, Path(plan['assetRoot']))
        for path, declared in found.items():
            if references.get(path) is None:
                references[path] = declared
        unresolved += missing
    shaders = controlled.tree_hash(Path(plan['shaderRoot']))
    assets = asset_provenance(Path(plan['assetRoot']), repo, references, unresolved, budget_seconds)
    binaries = {}
    roles = {}
    for key in ('exe', 'editorExe'):
        if not plan.get(key):
            continue
        arm = {'role': key, 'repo': plan['repo'], 'exe': plan[key], 'buildConfig': plan['buildConfig'],
               'compiler': plan['compiler'], 'shaderRoot': plan['shaderRoot'], 'assetRoot': plan['assetRoot']}
        record = controlled.provenance(arm, shaders={'sha256': shaders['sha256'], 'see': 'shaders'},
                                       assets={'sha256': assets['sha256'], 'mode': assets['mode'], 'see': 'assets'})
        binaries[record['binarySha256']] = record
        roles[key] = record['binarySha256']
    manifests = {_relative(Path(c['manifest']), repo): sha256_file(Path(c['manifest'])) for c in plan['cells']}
    provenance = {'binaries': binaries, 'binaryByRole': roles, 'shaders': shaders, 'assets': assets,
                  'manifests': manifests, 'host': safe_probe_host(), 'machine': platform.node(),
                  'os': platform.platform(), 'python': platform.python_version(),
                  'driverSha256': sha256_file(Path(__file__))}
    return provenance, references, unresolved


# ---------------------------------------------------------------- execution

def classify(record: dict, analysis: dict | None, analysis_error: str | None,
             include_unsampled: bool = False) -> tuple:
    """(status, reasons). counted | excluded (contention) | invalid | failed | skipped."""
    if record.get('skipReason'):
        return 'skipped', [record['skipReason']]
    reasons = []
    status = None
    if record.get('processFailure'):
        status = 'failed'
        reasons.append('process failed: ' + record['processFailure'])
    elif analysis_error:
        status = 'invalid'
        reasons.append(analysis_error)
    else:
        problems = list(analysis['invalidReasons'])
        expected = HOSTS.get(record.get('host'))
        if expected and (analysis['host'] != expected['captureHost'] or analysis['backend'] != expected['backend']):
            problems.append(f"capture host/backend {analysis['host']}/{analysis['backend']} is not the cell's "
                            f"{expected['captureHost']}/{expected['backend']}")
        if problems:
            status = 'invalid'
            reasons += problems
    contention = record.get('contentionReasons')
    if contention is None and not include_unsampled:
        contention = ['no contention sampler record: the run was not launched by this driver']
    if contention:
        reasons += contention
        status = status or 'excluded'
    return status or 'counted', reasons


def analyse_safely(record: dict, directory: Path, gate_ms: float, include_unsampled: bool = False) -> dict | None:
    analysis, error = None, None
    if not record.get('skipReason') and not record.get('processFailure'):
        try:
            analysis = analyse_run(directory, gate_ms)
        except (OSError, ValueError, KeyError, TypeError) as caught:
            error = f'{type(caught).__name__}: {caught}'
        else:
            # A selected setting is not evidence the technique ran; a required
            # pass with no valid sample is.
            for scenario, item in analysis['scenarios'].items():
                for name in record.get('requirePasses') or []:
                    if name not in item['passes'] and name not in item['subPasses']:
                        analysis['invalidReasons'].append(f'{scenario}: required pass {name} has no valid GPU sample')
    record['status'], record['reasons'] = classify(record, analysis, error, include_unsampled)
    return analysis


def run_attempt(plan: dict, cell: dict, round_index: int, attempt: int, output: Path, campaign: dict,
                probe) -> dict:
    from capture_process import run_capture  # needs PyYAML only for the editor host

    host = HOSTS[cell['host']]
    destination = output / cell['id'] / f'round-{round_index + 1:02d}-attempt-{attempt}'
    destination.mkdir(parents=True, exist_ok=False)
    record = {'cell': cell['id'], 'host': cell['host'], 'round': round_index + 1, 'attempt': attempt,
              'directory': destination.relative_to(output).as_posix(), 'manifest': cell['manifest'],
              'manifestSha256': campaign['manifestSha256'][cell['manifest']],
              'binarySha256': campaign['fingerprint']['binaries'][host['exeKey']],
              'requirePasses': cell.get('requirePasses', []),
              'frameMetric': host['frameMetric'], 'queuedAt': now_iso()}
    quiet, history = wait_for_quiet(probe, plan['idleGpuUtilMax'], plan['contentionWaitSeconds'])
    record['preLaunch'] = history[-3:]
    record['gpuProcessProbe'] = 'nvidia-smi' if campaign['gpuBaseline'] is not None else 'unavailable'
    if not quiet:
        record['skipReason'] = (f"host still contended after {plan['contentionWaitSeconds']:g} s: "
                                + '; '.join(history[-1]['reasons']))
        record['contentionReasons'] = history[-1]['reasons']
    else:
        current = fingerprint(plan, campaign['assetsMode'], campaign['references'], campaign['unresolved'])
        if current != campaign['fingerprint']:
            raise BuildChanged(f'binary, shader or asset fingerprint changed mid-campaign: '
                               f'{campaign["fingerprint"]} -> {current}')
        arm = {'exe': plan[host['exeKey']], 'repo': plan['repo'], 'host': host['captureHost'],
               'backend': host['backend'], 'mcpPort': plan['mcpPort']}
        environment = {k: v for k, v in os.environ.items() if k != 'OLO_CAPTURE_TEST_DELAY_MS'}
        if plan['shaderCacheDir']:
            environment['OLO_SHADER_CACHE_DIR'] = plan['shaderCacheDir']
        record['shaderCacheDir'] = environment.get('OLO_SHADER_CACHE_DIR', 'inherited/default')
        record['hostBefore'] = safe_probe_host()
        record['startedAt'] = now_iso()
        started = time.perf_counter()
        with Sampler(probe) as sampler, (destination / 'stdout.txt').open('w', encoding='utf-8') as out, \
                (destination / 'stderr.txt').open('w', encoding='utf-8') as err:
            try:
                result = run_capture(arm, Path(cell['manifest']), destination, environment,
                                     plan['timeoutSeconds'], out, err)
                record['capture'] = result
                if result.get('exitCode'):
                    record['processFailure'] = f"exit code {result['exitCode']}"
                elif not (destination / 'result.json').exists():
                    record['processFailure'] = 'process exited without writing result.json'
            except subprocess.TimeoutExpired:
                record['processFailure'] = f"timed out after {plan['timeoutSeconds']:g} s"
            except (ValueError, RuntimeError, OSError, KeyError) as error:
                record['processFailure'] = f'{type(error).__name__}: {error}'
        record['processWallSeconds'] = round(time.perf_counter() - started, 2)
        record['endedAt'] = now_iso()
        record['hostAfter'] = safe_probe_host()
        record['samples'] = sampler.samples
        record['contentionReasons'] = contention_reasons(sampler.samples)
    analyse_safely(record, destination, plan['gateMs'])
    write_json(destination / 'driver-run.json', record)
    return record


def execute(plan: dict, output: Path, budget_seconds: float, probe=None, attempt_runner=run_attempt) -> list:
    if os.name != 'nt' and probe is None:
        raise ValueError('live acceptance captures probe Windows processes; this host is not Windows')
    for root in (plan['shaderRoot'], plan['assetRoot']):
        if Path(root) in output.resolve().parents or output.resolve() == Path(root):
            raise ValueError(f'output {output} lies inside the hashed tree {root}')
    write_json(output / 'plan.json', plan)
    provenance, references, unresolved = build_provenance(plan, budget_seconds)
    probe = probe or LiveProbe(plan)
    provenance['gpuProcessBaseline'] = probe.gpu_baseline
    write_json(output / 'provenance.json', provenance)
    campaign = {'fingerprint': fingerprint(plan, provenance['assets'], references, unresolved),
                'assetsMode': provenance['assets'], 'references': references, 'unresolved': unresolved,
                'gpuBaseline': probe.gpu_baseline,
                'manifestSha256': {c['manifest']: sha256_file(Path(c['manifest'])) for c in plan['cells']}}
    cells = {cell['id']: cell for cell in plan['cells']}
    budget = {cell_id: plan['maxRetries'] for cell_id in cells}
    attempts = {cell_id: 0 for cell_id in cells}
    records = []

    def attempt(cell_id: str, round_index: int) -> bool:
        attempts[cell_id] += 1
        print(f'[acceptance] round {round_index + 1} {cell_id} attempt {attempts[cell_id]} ...', flush=True)
        record = attempt_runner(plan, cells[cell_id], round_index, attempts[cell_id], output, campaign, probe)
        records.append(record)
        write_json(output / 'runs.json', records)
        print(f"[acceptance]   {record['status']}" + (': ' + '; '.join(record['reasons']) if record['reasons'] else ''),
              flush=True)
        return record['status'] == 'counted'

    for round_index, order in enumerate(schedule(list(cells), plan['repeats'])):
        pending = [cell_id for cell_id in order if not attempt(cell_id, round_index)]
        while pending:
            retry = []
            for cell_id in pending:
                if budget[cell_id] <= 0:
                    continue
                budget[cell_id] -= 1
                if not attempt(cell_id, round_index):
                    retry.append(cell_id)
            pending = retry
    return records


# ---------------------------------------------------------------- summary

def _range(values: list):
    present = [v for v in values if v is not None]
    return [min(present), max(present)] if present else None


def stat_ranges(rows: list, keys=('p50Ms', 'p95Ms', 'p99Ms', 'maxMs', 'deadlineMisses', 'count')) -> dict | None:
    if not rows or any(row is None for row in rows):
        return None
    return {key: _range([row.get(key) for row in rows]) for key in keys if any(key in row for row in rows)}


def gate_verdict(per_run_p95: list, repeats: int, gate_ms: float) -> dict:
    """per_run_p95: one {scenario: p95 frame ms} per counted run."""
    failures = [f'run {index + 1} {scenario} p95 '
                + ('unmeasured' if value is None else f'{value:.3f} ms > {gate_ms:g} ms')
                for index, run in enumerate(per_run_p95) for scenario, value in sorted(run.items())
                if value is None or value > gate_ms]
    if failures:
        return {'verdict': 'FAIL', 'reason': '; '.join(failures)}
    if len(per_run_p95) < repeats:
        return {'verdict': 'INCONCLUSIVE', 'reason': f'{len(per_run_p95)} of {repeats} required runs counted'}
    return {'verdict': 'PASS', 'reason': f'every counted run ({len(per_run_p95)}) has p95 frame time <= '
                                         f'{gate_ms:g} ms in every scenario'}


SIGNATURE_KEYS = ('host', 'backend', 'manifestSourceHash')


def summarise_cell(cell_id: str, entries: list, repeats: int, gate_ms: float) -> dict:
    """entries: [(record, analysis or None)] for one cell, in run order."""
    counted = [(record, analysis) for record, analysis in entries if record['status'] == 'counted']
    if counted:
        first_record, first = counted[0]
        for record, analysis in counted[1:]:
            for key in SIGNATURE_KEYS:
                if analysis[key] != first[key]:
                    raise ValueError(f'{cell_id}: {key} differs across counted runs '
                                     f'({first[key]!r} vs {analysis[key]!r})')
            if record.get('binarySha256') != first_record.get('binarySha256'):
                raise ValueError(f'{cell_id}: counted runs come from different binaries; never compare across builds')
            if (analysis['sizes']['render'], analysis['sizes']['display']) != (first['sizes']['render'],
                                                                              first['sizes']['display']):
                raise ValueError(f'{cell_id}: actual render/display sizes differ across counted runs')
            if set(analysis['scenarios']) != set(first['scenarios']):
                raise ValueError(f'{cell_id}: scenario sets differ across counted runs')
    analyses = [analysis for _, analysis in counted]
    scenarios = {}
    for scenario in sorted(analyses[0]['scenarios']) if analyses else []:
        rows = [a['scenarios'][scenario] for a in analyses]
        item = {'frames': _range([r['frames'] for r in rows]),
                'channels': {name: {'perRun': [r[name] for r in rows], 'range': stat_ranges([r[name] for r in rows])}
                             for name in SUMMARY_CHANNELS},
                'gpuExcluded': [r['gpuExcluded'] for r in rows]}
        for group in ('passes', 'subPasses'):
            names = sorted({name for r in rows for name in r[group]})
            item[group] = {}
            for name in names:
                per_run = [r[group].get(name) for r in rows]
                present = [p for p in per_run if p is not None]
                item[group][name] = {'presentInRuns': len(present),
                                     'range': {key: _range([p[key] for p in present])
                                               for key in ('p50Ms', 'p95Ms', 'p99Ms', 'maxMs', 'count')}}
                if group == 'subPasses':
                    item[group][name]['parent'] = present[0]['parent']
        scenarios[scenario] = item
    verdict = gate_verdict([{s: a['scenarios'][s]['frameTime']['p95Ms'] for s in a['scenarios']} for a in analyses],
                           repeats, gate_ms)
    memory_keys = ('peakGpuResidentBytes', 'liveGpuResidentBytes', 'peakCpuTrackedBytes',
                   'gpuResidentBytesAfterSceneRelease')
    report_keys = ('gpuPeakBytes', 'gpuLiveBytes', 'cpuPeakBytes', 'cpuLiveBytes')
    memory = {key: _range([a['memory'].get(key) for a in analyses]) for key in memory_keys}
    reports = [a['memory'].get('report') for a in analyses]
    memory['report'] = ({key: _range([r.get(key) for r in reports]) for key in report_keys}
                        if reports and all(reports) else None)
    if memory['report'] is not None:
        memory['report']['reconciliation'] = sorted({str(r.get('reconciliation')) for r in reports})
        memory['report']['residency'] = sorted({str(r.get('residency')) for r in reports})
        memory['report']['topGpuOwners'] = reports[0]['topGpuOwners']
        memory['report']['topCpuOwners'] = reports[0]['topCpuOwners']
    first = analyses[0] if analyses else None
    return {'cell': cell_id, 'verdict': verdict['verdict'], 'verdictReason': verdict['reason'],
            'countedRuns': len(counted), 'requiredRuns': repeats,
            'host': first['host'] if first else None, 'backend': first['backend'] if first else None,
            'frameMetric': first['frameMetric'] if first else None,
            'manifestId': first['manifestId'] if first else None,
            'binarySha256': counted[0][0].get('binarySha256') if counted else None,
            'sizes': first['sizes'] if first else None,
            'upscalerByRun': [a['sizes']['upscaler'] for a in analyses],
            'techniques': first['techniques'] if first else None,
            'techniquesConsistent': all(a['techniques']['selected'] == first['techniques']['selected']
                                        for a in analyses) if first else None,
            'memory': memory, 'scenarios': scenarios,
            'passExcludedByRun': [a['passExcluded'] for a in analyses],
            'notCounted': [{'directory': record['directory'], 'status': record['status'], 'reasons': record['reasons']}
                           for record, _ in entries if record['status'] != 'counted']}


def header_from(provenance: dict | None, analyses: list) -> dict:
    header = {'gpu': None, 'driver': None, 'cpu': None, 'os': None, 'ramBytes': None, 'build': None}
    if provenance:
        host = provenance.get('host') or {}
        smi = ((host.get('nvidiaSmi') or {}).get('gpu') or '').split(',')
        controllers = host.get('gpu') or []
        if isinstance(controllers, dict):
            controllers = [controllers]
        cpus = host.get('cpu') or []
        if isinstance(cpus, dict):
            cpus = [cpus]
        header.update(gpu=smi[0].strip() if smi[0] else ', '.join(str(c.get('Name')) for c in controllers) or None,
                      driver=(smi[1].strip() if len(smi) > 1 else
                              ', '.join(str(c.get('DriverVersion')) for c in controllers) or None),
                      cpu=cpus[0].get('Name') if cpus else None, os=host.get('os') or provenance.get('os'),
                      ramBytes=host.get('ramBytes'))
        header['build'] = [{'role': record.get('role'), 'exe': record.get('exe'),
                            'buildConfig': record.get('buildConfig'), 'compiler': record.get('compiler'),
                            'binarySha256': sha, 'commit': record.get('commit'),
                            'dirtyDiffSha256': record.get('dirtyDiffSha256'),
                            'untrackedFiles': len(record.get('untracked') or {})}
                           for sha, record in (provenance.get('binaries') or {}).items()]
        header['assetHashMode'] = (provenance.get('assets') or {}).get('mode')
        header['assetHashReason'] = (provenance.get('assets') or {}).get('reason')
    if not header['gpu'] and analyses:
        header['gpu'] = analyses[0].get('gpuRenderer')
    return header


def summarise(records: list, resolve_dir, *, gate_ms: float, repeats: int, output: Path,
              provenance: dict | None = None, include_unsampled: bool = False) -> dict:
    analyses = {}
    for index, record in enumerate(records):
        analyses[index] = analyse_safely(record, resolve_dir(record), gate_ms, include_unsampled)
    cells = {}
    order = []
    for index, record in enumerate(records):
        if record['cell'] not in cells:
            order.append(record['cell'])
        cells.setdefault(record['cell'], []).append((record, analyses[index]))
    summary_cells = {cell_id: summarise_cell(cell_id, cells[cell_id], repeats, gate_ms) for cell_id in order}
    verdicts = [cell['verdict'] for cell in summary_cells.values()]
    overall = ('FAIL' if 'FAIL' in verdicts else 'INCONCLUSIVE' if 'INCONCLUSIVE' in verdicts or not verdicts
               else 'PASS')
    limitations = []
    if any(record.get('gpuProcessProbe') == 'unavailable' for record in records):
        limitations.append('nvidia-smi was unavailable for some runs: GPU processes were not screened, only '
                           'named competitor processes and nothing else')
    if provenance and (provenance.get('assets') or {}).get('mode') == 'manifest-referenced':
        limitations.append('asset provenance covers manifest-referenced files only: '
                           + provenance['assets'].get('reason', ''))
    if provenance is None:
        limitations.append('no provenance.json: binary, source and content identity are unrecorded')
    summary = {'gateMs': gate_ms, 'repeats': repeats, 'overall': overall,
               'header': header_from(provenance, [a for a in analyses.values() if a]),
               'method': {'uncertainty': 'range over counted fresh-process runs; frames within a run are '
                                         'correlated and are not bootstrapped',
                          'verdict': 'FAIL if any counted run has a scenario with p95 frame time > gate; PASS '
                                     'when `repeats` runs counted and none fails; else INCONCLUSIVE',
                          'gpu': 'valid status, distinct gpuFrameId only',
                          'passes': 'valid status, distinct (gpuFrameId, pass) only; sub-passes reported '
                                    'separately, never added to parents',
                          'frameTimeByHost': {HOSTS[k]['captureHost']: HOSTS[k]['frameMetric'] for k in HOSTS}},
               'limitations': limitations, 'cells': summary_cells}
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / 'runs.json', portable(records, resolve_dir, output))
    write_json(output / 'summary.json', summary)
    (output / 'summary.md').write_text(render_markdown(summary), encoding='utf-8')
    return summary


def fmt_range(pair, digits: int = 2) -> str:
    if not pair:
        return 'n/a'
    low, high = pair
    if isinstance(low, int) and isinstance(high, int):
        return str(low) if low == high else f'{low}–{high}'
    if abs(high - low) < 10 ** -digits:
        return f'{low:.{digits}f}'
    return f'{low:.{digits}f}–{high:.{digits}f}'


def fmt_bytes(pair) -> str:
    if not pair:
        return 'n/a'
    return fmt_range([pair[0] / 1048576.0, pair[1] / 1048576.0], 1) + ' MiB'


CHANNEL_LABELS = {'frameTime': 'frame time', 'cpu': 'CPU', 'gpu': 'GPU total (valid)', 'fenceWait': 'fence wait',
                  'presentWait': 'present wait', 'recordingWall': 'recording wall',
                  'recordingJoinWait': 'recording join wait'}


def render_markdown(summary: dict) -> str:
    header = summary['header']
    lines = ['# Integrated AAA acceptance gate (#1259)', '',
             f"Overall: **{summary['overall']}**. Gate: p95 frame time <= {summary['gateMs']:g} ms in every "
             f"scenario of every counted run; {summary['repeats']} counted runs required per cell.", '',
             f"GPU: {header.get('gpu') or 'unknown'}; driver {header.get('driver') or 'unknown'}. "
             f"CPU: {header.get('cpu') or 'unknown'}. OS: {header.get('os') or 'unknown'}.", '']
    for build in header.get('build') or []:
        lines.append(f"Build ({build['role']}): {build['buildConfig']} / {build['compiler']}, commit "
                     f"{build['commit']}, dirty diff {str(build['dirtyDiffSha256'])[:12]}, "
                     f"{build['untrackedFiles']} untracked files, binary {str(build['binarySha256'])[:12]}.")
    if not header.get('build'):
        lines.append('Build: unknown (no provenance.json).')
    if header.get('assetHashMode'):
        lines.append(f"Asset hash: {header['assetHashMode']}"
                     + (f" ({header['assetHashReason']})" if header.get('assetHashReason') else '') + '.')
    lines += ['', 'Ranges are min–max over counted runs. Frame time is a different quantity per host '
                  '(see each cell); hosts are never pooled.', '']
    for note in summary['limitations']:
        lines.append(f'- Limitation: {note}')
    for cell in summary['cells'].values():
        lines += ['', f"## {cell['cell']}: {cell['verdict']}", '',
                  f"{cell['verdictReason']}. Counted runs: {cell['countedRuns']} of {cell['requiredRuns']}.", '']
        if cell['frameMetric']:
            lines.append(f"Host: {cell['host']} / {cell['backend']}. Frame time = {cell['frameMetric']}.")
        sizes = cell['sizes']
        if sizes:
            upscalers = sorted({json.dumps(u, sort_keys=True) for u in cell['upscalerByRun']})
            lines.append(f"Internal {sizes['render'][0]}x{sizes['render'][1]}, display {sizes['display'][0]}x"
                         f"{sizes['display'][1]} ({sizes['internalSource']}); upscaler resolved/fallback: "
                         f"{', '.join(upscalers)}.")
        techniques = cell['techniques']
        if techniques:
            selected = techniques['selected']
            lines.append('Selected: ' + ', '.join(f'{k}={selected[k]}' for k in sorted(selected))
                         + f"; VSM selected: {techniques['virtualShadowMapsSelected']}; pinned by manifest: "
                         + (', '.join(techniques['pinned']) or 'none')
                         + ('' if cell['techniquesConsistent'] else '; SELECTED SETTINGS DIFFER ACROSS RUNS') + '.')
            lines.append('Selected settings are what the renderer was set to, not proof each technique produced '
                         'or consumed work.')
        for scenario, item in cell['scenarios'].items():
            lines += ['', f"### {scenario} (frames/run {fmt_range(item['frames'])})", '',
                      '| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |',
                      '|---|---:|---:|---:|---:|---:|---:|']
            for name in SUMMARY_CHANNELS:
                ranges = item['channels'][name]['range']
                if ranges is None:
                    lines.append(f'| {CHANNEL_LABELS[name]} | n/a | n/a | n/a | n/a | n/a | n/a |')
                    continue
                lines.append(f"| {CHANNEL_LABELS[name]} | {fmt_range(ranges['p50Ms'])} | {fmt_range(ranges['p95Ms'])} | "
                             f"{fmt_range(ranges['p99Ms'])} | {fmt_range(ranges['maxMs'])} | "
                             f"{fmt_range(ranges.get('deadlineMisses'))} | {fmt_range(ranges['count'])} |")
            excluded = {}
            for run in item['gpuExcluded']:
                for reason, count in run.items():
                    excluded[reason] = excluded.get(reason, 0) + count
            if excluded:
                lines.append('')
                lines.append('Excluded total-GPU samples (all counted runs): '
                             + ', '.join(f'{reason} {count}' for reason, count in sorted(excluded.items())) + '.')
            for group, title, limit in (('passes', 'Top passes by p50', 15), ('subPasses', 'Top sub-passes by p50', 10)):
                ranked = sorted(item[group].items(), key=lambda kv: -(kv[1]['range']['p50Ms'] or [0, 0])[1])[:limit]
                if not ranked:
                    continue
                lines += ['', f'{title} (valid samples only):', '',
                          '| pass | p50 ms | p95 ms | p99 ms | samples | runs |' if group == 'passes' else
                          '| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |',
                          '|---|---:|---:|---:|---:|---:|' if group == 'passes' else '|---|---|---:|---:|---:|---:|---:|']
                for name, entry in ranked:
                    r = entry['range']
                    cells_text = (f"{fmt_range(r['p50Ms'], 3)} | {fmt_range(r['p95Ms'], 3)} | "
                                  f"{fmt_range(r['p99Ms'], 3)} | {fmt_range(r['count'])} | {entry['presentInRuns']}")
                    if group == 'passes':
                        lines.append(f'| {name} | {cells_text} |')
                    else:
                        lines.append(f"| {name} | {entry['parent']} | {cells_text} |")
        pass_excluded = {}
        for run in cell['passExcludedByRun']:
            for reason, count in run.items():
                pass_excluded[reason] = pass_excluded.get(reason, 0) + count
        if pass_excluded:
            lines += ['', 'Excluded per-pass samples (all counted runs): '
                      + ', '.join(f'{reason} {count}' for reason, count in sorted(pass_excluded.items())) + '.']
        memory = cell['memory']
        lines += ['', f"Memory: peak GPU resident {fmt_bytes(memory['peakGpuResidentBytes'])}, live GPU "
                      f"{fmt_bytes(memory['liveGpuResidentBytes'])}, peak CPU tracked "
                      f"{fmt_bytes(memory['peakCpuTrackedBytes'])}, GPU after scene release "
                      f"{fmt_bytes(memory['gpuResidentBytesAfterSceneRelease'])}."]
        report = memory['report']
        if report:
            lines.append(f"Report: GPU peak {fmt_bytes(report['gpuPeakBytes'])} / live "
                         f"{fmt_bytes(report['gpuLiveBytes'])}; CPU peak {fmt_bytes(report['cpuPeakBytes'])} / live "
                         f"{fmt_bytes(report['cpuLiveBytes'])}; reconciliation {', '.join(report['reconciliation'])}; "
                         f"residency {', '.join(report['residency'])}.")
            owners = ', '.join(f"{o['owner']} {o['gpuBytes'] / 1048576.0:.1f} MiB" for o in report['topGpuOwners'])
            lines.append('Top GPU owners (first counted run): ' + (owners or 'none') + '.')
        elif cell['countedRuns']:
            lines.append('Memory report (result.json `memory`) absent in at least one counted run.')
        if cell['notCounted']:
            lines += ['', 'Not counted:', '']
            for run in cell['notCounted']:
                lines.append(f"- `{run['directory']}` {run['status']}: {'; '.join(run['reasons'])}")
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------- re-summary

def bare_record(directory: Path) -> dict:
    result = read_json(directory / 'result.json')
    provenance = result.get('provenance') or {}
    host = 'gl' if provenance.get('host') == 'test-binary' else str(provenance.get('backend'))
    return {'cell': f"{host}-{(result.get('manifest') or {}).get('id')}", 'host': host,
            'directory': str(directory.resolve()), 'binarySha256': None, 'contentionReasons': None}


def collect(run_dirs: list) -> tuple:
    records, plan, provenance, roots = [], None, None, []
    for directory in run_dirs:
        directory = Path(directory).resolve()
        if (directory / 'runs.json').exists():
            for record in read_json(directory / 'runs.json'):
                record['_root'] = str(directory)
                records.append(record)
            if (directory / 'plan.json').exists() and plan is None:
                plan = read_json(directory / 'plan.json')
            if (directory / 'provenance.json').exists() and provenance is None:
                provenance = read_json(directory / 'provenance.json')
            roots.append(directory)
        elif (directory / 'driver-run.json').exists():
            record = read_json(directory / 'driver-run.json')
            record['directory'] = str(directory)
            records.append(record)
        elif (directory / 'result.json').exists():
            records.append(bare_record(directory))
        else:
            raise ValueError(f'{directory} holds no runs.json, driver-run.json or result.json')
    return records, plan, provenance, roots


def resolve_record_dir(record: dict) -> Path:
    path = Path(record['directory'])
    return path if path.is_absolute() else Path(record.get('_root', '.')) / path


def portable(records: list, resolve_dir, output: Path) -> list:
    """Records as written to runs.json: run directories relative to `output`
    when they lie inside it, absolute otherwise; no private keys."""
    out = []
    for record in records:
        clean = {k: v for k, v in record.items() if k != '_root'}
        if 'directory' in record:
            directory = Path(resolve_dir(record)).resolve()
            try:
                clean['directory'] = directory.relative_to(Path(output).resolve()).as_posix()
            except ValueError:
                clean['directory'] = directory.as_posix()
        out.append(clean)
    return out


EXIT_CODES = {'PASS': 0, 'FAIL': 1, 'INCONCLUSIVE': 2}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--gate-ms', type=float)
    parser.add_argument('--repeats', type=int)
    parser.add_argument('--max-retries', type=int)
    parser.add_argument('--asset-hash-budget-seconds', type=float, default=ASSET_HASH_BUDGET_SECONDS)
    parser.add_argument('--dry-run', action='store_true', help='print the interleaved schedule and exit')
    parser.add_argument('--summarise-only', action='store_true')
    parser.add_argument('--run-dir', type=Path, action='append', default=[])
    parser.add_argument('--include-unsampled', action='store_true',
                        help='count bare capture directories that have no contention record')
    args = parser.parse_args(argv)
    default_repo = HERE.parents[1]
    try:
        if args.summarise_only:
            if not args.run_dir:
                parser.error('--summarise-only needs at least one --run-dir')
            records, plan, provenance, roots = collect(args.run_dir)
            plan = plan or {}
            gate = args.gate_ms if args.gate_ms is not None else plan.get('gateMs', DEFAULT_GATE_MS)
            repeats = args.repeats if args.repeats is not None else plan.get('repeats', MIN_REPEATS)
            output = args.output or (roots[0] if len(roots) == 1 else None)
            if output is None:
                parser.error('--output is required unless exactly one campaign directory is given')
            summary = summarise(records, resolve_record_dir, gate_ms=gate, repeats=repeats, output=output,
                                provenance=provenance, include_unsampled=args.include_unsampled)
        else:
            if not args.plan:
                parser.error('--plan is required')
            plan = load_plan(read_json(args.plan), default_repo=default_repo, gate_ms=args.gate_ms,
                             repeats=args.repeats, max_retries=args.max_retries, check_files=not args.dry_run)
            if args.dry_run:
                print(f"gate {plan['gateMs']:g} ms, {plan['repeats']} rounds, up to {plan['maxRetries']} retries per "
                      f'cell (a retry runs at the end of its round)')
                for cell in plan['cells']:
                    host = HOSTS[cell['host']]
                    print(f"  {cell['id']}: {host['captureHost']}/{host['backend']} "
                          f"{plan.get(host['exeKey'], '<' + host['exeKey'] + '>')} {cell['manifest']}")
                for round_index, order in enumerate(schedule([c['id'] for c in plan['cells']], plan['repeats'])):
                    print(f'round {round_index + 1}: ' + ', '.join(order))
                return 0
            if not args.output:
                parser.error('--output is required')
            if args.output.exists() and any(args.output.iterdir()):
                raise ValueError(f'output directory is not empty; choose a fresh one: {args.output}')
            args.output.mkdir(parents=True, exist_ok=True)
            output = args.output.resolve()
            aborted = None
            try:
                records = execute(plan, output, args.asset_hash_budget_seconds)
            except BuildChanged as error:
                aborted = error
                records = read_json(output / 'runs.json') if (output / 'runs.json').exists() else []
            for record in records:
                record['_root'] = str(output)
            summary = summarise(records, resolve_record_dir, gate_ms=plan['gateMs'], repeats=plan['repeats'],
                                output=output, provenance=read_json(output / 'provenance.json'))
            if aborted is not None:
                print(f'[acceptance] aborted, never comparing across builds: {aborted}', file=sys.stderr)
                return 2
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f'[acceptance] {type(error).__name__}: {error}', file=sys.stderr)
        return 2
    for cell in summary['cells'].values():
        print(f"[acceptance] {cell['cell']}: {cell['verdict']} ({cell['verdictReason']})")
    print(f"[acceptance] overall {summary['overall']}; see {output / 'summary.md'}")
    return EXIT_CODES[summary['overall']]


if __name__ == '__main__':
    raise SystemExit(main())
