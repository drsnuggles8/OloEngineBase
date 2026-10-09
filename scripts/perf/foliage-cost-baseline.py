"""Foliage cost baseline (#1391): run FoliageCostBaselineTest.MeasureBaseline in
fresh processes, record the host around every run, and summarise run-level ranges.

    python scripts/perf/foliage-cost-baseline.py --exe build-cached/OloEngine/tests/Release/OloEngine-Tests.exe \
        --runs 3 --output docs/analysis/foliage-cost-baseline-1391

Each run writes <output>/run-<k>/foliage-cost.json (raw samples and per-cell
summaries, see the test) and host.json (CPU/GPU/driver, GPU clocks and power, GPU
processes and competing build/editor/test processes, before and after). The
summary is computed per run first and then reported as the range across runs:
adjacent frames of one run are not independent samples (docs/guides/
controlled-performance.md), runs are.

`--summarise-only` rebuilds summary.json / summary.md from existing run folders.

GPU contention is excluded per cell, not per run. While a run is measuring, a
sampler records every 30 s the GPU's clock, power and utilisation and every other
engine or game process (an editor, runtime or test binary from another worktree
shares the GPU), with its CPU time. An interval in which such a process gained CPU
time is a contention window. <output>/contention.json can add windows found any
other way: [{"start": "2026-10-09T15:48:31", "end": "...", "what": "..."}]. Each
cell and tail window is timed from the engine log's [HH:MM:SS] stamps, and a
cell-run that overlaps a window is left out of the summary and counted as such.
A run with no sampler record cannot show it was uncontended, so it is left out
entirely unless --include-unsampled is given.
"""
from __future__ import annotations

import argparse
import datetime
import gzip
import json
import re
import os
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

COMPETITORS = ['cmake', 'ninja', 'MSBuild', 'clang-cl', 'cl', 'lld-link', 'link', 'OloEditor', 'OloEngine-Tests',
               'OloRuntime', 'OloServer', 'devenv', 'WowB', 'Wow']

ARMS = ['Shipped', 'CpuCull', 'NoDensityLod', 'NoFoliage']


def probe_host() -> dict:
    script = (
        "$names = @(" + ",".join("'" + n + "'" for n in COMPETITORS) + ");"
        "$busy = @(Get-Process -Name $names -ErrorAction SilentlyContinue | Select-Object ProcessName,Id,StartTime);"
        "@{ cpu = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);"
        "   gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion);"
        "   ramBytes = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory;"
        "   os = (Get-CimInstance Win32_OperatingSystem).Version;"
        "   busyProcesses = $busy } | ConvertTo-Json -Depth 4 -Compress"
    )
    state = json.loads(subprocess.check_output(['pwsh', '-NoProfile', '-Command', script], text=True))
    try:
        gpu = subprocess.check_output(
            ['nvidia-smi', '--query-gpu=name,driver_version,clocks.gr,clocks.mem,power.draw,utilization.gpu,temperature.gpu',
             '--format=csv,noheader'], text=True).strip()
        apps = subprocess.check_output(['nvidia-smi', '--query-compute-apps=pid,process_name,used_memory',
                                        '--format=csv,noheader'], text=True).strip()
        graphics = subprocess.check_output(['nvidia-smi'], text=True)
        state['nvidiaSmi'] = {'gpu': gpu, 'computeApps': apps.splitlines() if apps else [],
                              'processTable': [line for line in graphics.splitlines() if ' G ' in line or ' C ' in line]}
    except (OSError, subprocess.CalledProcessError) as error:
        state['nvidiaSmi'] = {'error': str(error)}
    state['time'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
    return state


GPU_USERS = ['OloEditor', 'OloRuntime', 'OloEngine-Tests', 'OloServer', 'WowB', 'Wow']
SAMPLE_SECONDS = 30.0


def sample_gpu_users(own_exe: Path) -> dict:
    script = ("Get-Process -Name " + ",".join(GPU_USERS) + " -ErrorAction SilentlyContinue | "
              "ForEach-Object { '{0}|{1}|{2}' -f $_.Id,$_.CPU,$_.Path }")
    out = subprocess.run(['pwsh', '-NoProfile', '-Command', script], capture_output=True, text=True).stdout
    own = str(own_exe).lower()
    foreign = []
    for line in out.splitlines():
        parts = line.split('|', 2)
        if len(parts) == 3 and parts[2].lower() != own:
            foreign.append({'pid': int(parts[0]), 'cpuSeconds': float(parts[1] or 0.0), 'path': parts[2]})
    try:
        gpu = subprocess.check_output(['nvidia-smi', '--query-gpu=clocks.gr,power.draw,utilization.gpu',
                                       '--format=csv,noheader'], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        gpu = None
    return {'time': datetime.datetime.now().isoformat(timespec='seconds'), 'gpu': gpu, 'foreign': foreign}


def contention_from_samples(samples: list) -> list:
    """Windows in which a foreign GPU user gained CPU time, appeared, or exited
    (it ran until it exited, somewhere inside the interval)."""
    windows = []
    for previous, current in zip(samples, samples[1:]):
        before = {p['pid']: p['cpuSeconds'] for p in previous['foreign']}
        now = {p['pid'] for p in current['foreign']}
        active = [p for p in current['foreign'] if p['pid'] not in before or p['cpuSeconds'] > before[p['pid']] + 0.05]
        active += [p for p in previous['foreign'] if p['pid'] not in now]
        if active:
            window = {'start': previous['time'], 'end': current['time'],
                      'what': '; '.join(sorted({p['path'] for p in active}))}
            if windows and windows[-1]['end'] == window['start'] and windows[-1]['what'] == window['what']:
                windows[-1]['end'] = window['end']
            else:
                windows.append(window)
    return windows


def run_once(exe: Path, repo: Path, out: Path, environment: dict) -> dict:
    out.mkdir(parents=True, exist_ok=False)
    before = probe_host()
    env = {**os.environ, **environment, 'OLO_FOLIAGE_COST': '1', 'OLO_FOLIAGE_COST_OUT': str(out.resolve())}
    samples = [sample_gpu_users(exe)]
    stop = threading.Event()

    def sampler():
        while not stop.wait(SAMPLE_SECONDS):
            samples.append(sample_gpu_users(exe))

    thread = threading.Thread(target=sampler, daemon=True)
    thread.start()
    started = time.perf_counter()
    with (out / 'stdout.log').open('w', encoding='utf-8') as stdout:
        result = subprocess.run([str(exe), '--gtest_filter=FoliageCostBaselineTest.MeasureBaseline'],
                                cwd=repo, env=env, stdout=stdout, stderr=subprocess.STDOUT)
    elapsed = time.perf_counter() - started
    stop.set()
    thread.join()
    samples.append(sample_gpu_users(exe))
    after = probe_host()
    host = {'before': before, 'after': after, 'exitCode': result.returncode, 'elapsedSeconds': round(elapsed, 1),
            'environment': {k: v for k, v in environment.items()},
            'contended': bool(before.get('busyProcesses')) or bool(after.get('busyProcesses')),
            'samples': samples, 'contention': contention_from_samples(samples)}
    (out / 'host.json').write_text(json.dumps(host, indent=1), encoding='utf-8')
    if result.returncode != 0 or not (out / 'foliage-cost.json').exists():
        raise SystemExit(f'run failed (exit {result.returncode}); see {out / "stdout.log"}')
    return host


def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def finite(value):
    """A pass median, or None: a bracket whose every sample was invalid has a
    null median, and None must stay None rather than turn into 0 ms."""
    return value if isinstance(value, (int, float)) and value == value else None


def total(values):
    values = list(values)
    return None if any(v is None for v in values) else sum(values)


def foliage_brackets(passes: dict) -> dict:
    """Split one arm's pass medians into foliage's own brackets. An absent
    bracket is a pass that did not run (0 ms); a null one is unmeasured."""
    def ms(name):
        return finite(passes[name]) if name in passes else 0.0
    return {'cullMain': ms('FoliageCull'),
            'cullShadow': total(finite(v) for k, v in passes.items() if k.endswith('/FoliageCull')),
            'shadowCasters': total(finite(v) for k, v in passes.items() if k.endswith('/FoliageCasters')),
            'forwardDraw': total((ms('FoliagePass'), ms('FoliagePrepassPass')))}


def delta(a, b):
    return None if a is None or b is None else a - b


def run_number(path: Path) -> int:
    suffix = path.name[len('run-'):]
    return int(suffix) if suffix.isdigit() else 0


def load_contention(output: Path, hosts: list) -> list:
    windows = []
    path = output / 'contention.json'
    if path.exists():
        windows += json.loads(path.read_text(encoding='utf-8'))
    for host in hosts:
        windows += host.get('contention', [])
    return [(datetime.datetime.fromisoformat(w['start'][:19]), datetime.datetime.fromisoformat(w['end'][:19]))
            for w in windows]


def timed_entries(run: Path, host: dict) -> dict:
    """{'cell:<key>' or 'tail:<key>': (start, end)} from the engine log's [HH:MM:SS] stamps."""
    start_text = host.get('before', {}).get('time')
    if not start_text:
        return {}
    run_start = datetime.datetime.fromisoformat(start_text[:19])
    stamp = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\]')
    cell = re.compile(r'^\[foliage-cost\] (\S+) (\S+) (\S+) (\S+) msaa(\d+) (\d+)x(\d+) done')
    tail = re.compile(r'^\[foliage-cost\] tail (\S+) (\S+) (\S+) rebuild=(\d) done')
    out = {}
    previous = run_start
    last = run_start
    with (run / 'stdout.log').open(encoding='utf-8', errors='replace') as stream:
        for line in stream:
            m = stamp.match(line)
            if m:
                t = run_start.replace(hour=int(m.group(1)), minute=int(m.group(2)), second=int(m.group(3)))
                if t < run_start:
                    t += datetime.timedelta(days=1)
                last = t
                continue
            m = cell.match(line)
            if m:
                out['cell:' + '{}|{}|{}|{}|msaa{}|{}x{}'.format(*m.groups())] = (previous, last)
                previous = last
                continue
            m = tail.match(line)
            if m:
                out['tail:{}|{}|{}|{}'.format(*m.groups())] = (previous, last)
                previous = last
    return out


def load_run(run: Path) -> dict:
    """A run's foliage-cost.json, or the gzipped copy the analysis commits."""
    plain = run / 'foliage-cost.json'
    if plain.exists():
        return json.loads(plain.read_text(encoding='utf-8'))
    with gzip.open(run / 'foliage-cost.json.gz', 'rt', encoding='utf-8') as stream:
        return json.load(stream)


def run_timing(run: Path, host: dict) -> dict:
    """Cell and tail spans from the engine log when it is there (and cached as
    cell-times.json beside it), else from that cache: the logs are ~11 MB a run
    and are not committed."""
    cache = run / 'cell-times.json'
    if (run / 'stdout.log').exists():
        spans = timed_entries(run, host)
        cache.write_text(json.dumps({k: [a.isoformat(), b.isoformat()] for k, (a, b) in spans.items()}, indent=0),
                         encoding='utf-8')
        return spans
    if cache.exists():
        raw = json.loads(cache.read_text(encoding='utf-8'))
        return {k: (datetime.datetime.fromisoformat(a), datetime.datetime.fromisoformat(b)) for k, (a, b) in raw.items()}
    return {}


def overlaps(span, windows) -> bool:
    return span is not None and any(span[0] <= end and start <= span[1] for start, end in windows)


def cell_key(cell: dict) -> str:
    return '{subject}|{pose}|{path}|{shadows}|msaa{msaa}|{width}x{height}'.format(**cell)


def summarise_cell(cell: dict) -> dict:
    arms = cell['arms']
    shipped = arms['Shipped']
    out = {'gpuP50': shipped['gpuMs']['p50'], 'gpuP95': shipped['gpuMs']['p95'],
           'wallP50': shipped['wallMs']['p50'], 'wallP95': shipped['wallMs']['p95']}
    out.update(foliage_brackets(shipped['passMedianMs']))
    if 'NoFoliage' in arms:
        none = arms['NoFoliage']
        out['frameDeltaVsNoFoliage'] = shipped['gpuMs']['p50'] - none['gpuMs']['p50']
        out['scenePassDeltaVsNoFoliage'] = delta(finite(shipped['passMedianMs'].get('ScenePass', 0.0)),
                                                 finite(none['passMedianMs'].get('ScenePass', 0.0)))
        out['shadowPassDeltaVsNoFoliage'] = delta(finite(shipped['passMedianMs'].get('ShadowPass', 0.0)),
                                                  finite(none['passMedianMs'].get('ShadowPass', 0.0)))
    for arm in ('CpuCull', 'NoDensityLod'):
        if arm in arms:
            out['frameDelta' + arm] = arms[arm]['gpuMs']['p50'] - shipped['gpuMs']['p50']
    out['missingGpuSamples'] = sum(a['missingGpuSamples'] for a in arms.values())
    out['overflowFrames'] = sum(a['overflowFrames'] for a in arms.values())
    out['invalidBracketSamples'] = sum(a.get('invalidBracketSamples', 0) for a in arms.values())
    return out


def summarise(output: Path, include_unsampled: bool = False) -> dict:
    runs = sorted((p for p in output.glob('run-*')
                   if (p / 'foliage-cost.json').exists() or (p / 'foliage-cost.json.gz').exists()),
                  key=run_number)
    if not runs:
        raise SystemExit(f'no runs under {output}')
    per_run = []
    hosts = []
    host_records = []
    timing = []
    kept = []
    for run in runs:
        host = json.loads((run / 'host.json').read_text(encoding='utf-8')) if (run / 'host.json').exists() else {}
        sampled = 'samples' in host
        hosts.append({'run': run.name, 'sampled': sampled, 'usedInSummary': sampled or include_unsampled,
                      'buildsOrTestsSeen': host.get('contended'), 'elapsedSeconds': host.get('elapsedSeconds'),
                      'contentionWindows': host.get('contention', [])})
        if not (sampled or include_unsampled):
            continue
        kept.append(run)
        host_records.append(host)
        timing.append(run_timing(run, host))
        per_run.append(load_run(run))
    if not per_run:
        raise SystemExit('no sampled run to summarise (pass --include-unsampled to use unsampled ones)')
    runs = kept
    windows = load_contention(output, host_records)

    cells = {}
    excluded = 0
    for index, data in enumerate(per_run):
        for cell in data['cells']:
            entry = cells.setdefault(cell_key(cell), {'cell': {k: cell[k] for k in
                                                               ('subject', 'pose', 'path', 'shadows', 'msaa', 'width',
                                                                'height')}, 'runs': [], 'excludedRuns': []})
            if overlaps(timing[index].get('cell:' + cell_key(cell)), windows):
                entry['excludedRuns'].append(runs[index].name)
                excluded += 1
                continue
            entry['runs'].append(summarise_cell(cell))

    def spread(values):
        values = [v for v in values if v is not None]
        if not values:
            return None
        return {'min': round(min(values), 4), 'median': round(statistics.median(values), 4),
                'max': round(max(values), 4)}

    summary_cells = []
    for key, entry in cells.items():
        metrics = sorted({m for r in entry['runs'] for m in r})
        summary_cells.append({**entry['cell'], 'runs': len(entry['runs']), 'excludedRuns': entry['excludedRuns'],
                              'metrics': {m: spread([r.get(m) for r in entry['runs']]) for m in metrics}})

    tails = {}
    for index, data in enumerate(per_run):
        for tail in data['tails']:
            key = '{pose}|{path}|{shadows}|rebuild{rebuildEvery}'.format(**tail)
            entry = tails.setdefault(key, {'pose': tail['pose'], 'path': tail['path'], 'shadows': tail['shadows'],
                                           'rebuildEvery': tail['rebuildEvery'], 'runs': [], 'excludedRuns': []})
            logged = 'tail:{}|{}|{}|{}'.format(tail['pose'], tail['path'], tail['shadows'], 1 if tail['rebuildEvery'] else 0)
            if overlaps(timing[index].get(logged), windows):
                entry['excludedRuns'].append(runs[index].name)
                excluded += 1
                continue
            entry['runs'].append({'wall': tail['wallMs'], 'gpu': tail['gpuMs'],
                                  'rebuildFrameWallMs': tail['rebuildFrameWallMs']})
    summary_tails = []
    for entry in tails.values():
        row = {k: entry[k] for k in ('pose', 'path', 'shadows', 'rebuildEvery')}
        row['runs'] = len(entry['runs'])
        row['excludedRuns'] = entry['excludedRuns']
        for channel in ('wall', 'gpu'):
            for stat in ('p50', 'p95', 'p99', 'max', 'deadlineMisses'):
                row[channel + stat.capitalize()] = spread([r[channel][stat] for r in entry['runs']])
        row['rebuildFrameWallMs'] = spread([v for r in entry['runs'] for v in r['rebuildFrameWallMs']])
        summary_tails.append(row)

    summary = {'runs': hosts, 'contentionWindows': [[a.isoformat(), b.isoformat()] for a, b in windows],
               'excludedCellRuns': excluded, 'host': per_run[0]['host'], 'protocol': per_run[0]['protocol'],
               'memory': per_run[0]['memory'], 'cells': summary_cells, 'tails': summary_tails,
               'coldRebuild': [d['coldRebuild']['wallMs'] for d in per_run]}
    (output / 'summary.json').write_text(json.dumps(summary, indent=1), encoding='utf-8')
    (output / 'summary.md').write_text(render_markdown(summary), encoding='utf-8')
    return summary


def fmt(spread_value, digits=3):
    if not spread_value:
        return 'n/a'
    lo, hi = spread_value['min'], spread_value['max']
    if abs(hi - lo) < 10 ** -digits:
        return f'{lo:.{digits}f}'
    return f'{lo:.{digits}f}–{hi:.{digits}f}'


CONDENSED = [('gpuP50', 'frame GPU p50'), ('frameDeltaVsNoFoliage', 'foliage share of the frame'),
             ('cullMain', 'main-view cull'), ('cullShadow', 'shadow-view culls'), ('shadowCasters', 'shadow casters'),
             ('forwardDraw', 'forward draw (+prepass)'), ('scenePassDeltaVsNoFoliage', 'G-buffer share (ScenePass Δ)'),
             ('frameDeltaCpuCull', 'GPU culling off: frame Δ'), ('frameDeltaNoDensityLod', 'density LOD off: frame Δ')]


def condensed_rows(summary: dict) -> list:
    """Per subject x path x shadow technique at the native size: the range over
    poses of each cell's median across its clean runs."""
    groups = {}
    for cell in summary['cells']:
        if cell['msaa'] != 1 or (cell['width'], cell['height']) != (1920, 1080) or cell['runs'] == 0:
            continue
        groups.setdefault((cell['subject'], cell['path'], cell['shadows']), []).append(cell)
    rows = []
    for (subject, path, shadows), cells in groups.items():
        row = {'subject': subject, 'path': path, 'shadows': shadows, 'poses': len(cells)}
        for key, _ in CONDENSED:
            values = [c['metrics'][key]['median'] for c in cells if c['metrics'].get(key)]
            row[key] = (min(values), max(values)) if values else None
        rows.append(row)
    return rows


def render_markdown(summary: dict) -> str:
    lines = ['# Foliage cost baseline — generated tables', '',
             'Ranges are min–max of each run\'s median over interleaved blocks. Milliseconds of GPU time unless named.', '']
    lines.append('## Condensed: range over poses, 1920x1080, no MSAA')
    lines.append('')
    lines.append('| subject | path | shadows | poses | ' + ' | '.join(label for _, label in CONDENSED) + ' |')
    lines.append('|' + '---|' * (4 + len(CONDENSED)))
    for row in condensed_rows(summary):
        values = []
        for key, _ in CONDENSED:
            value = row[key]
            if value is None:
                values.append('n/a')
            elif abs(value[1] - value[0]) < 0.005:
                values.append('{:.2f}'.format(value[0]))
            else:
                values.append('{:.2f}–{:.2f}'.format(*value))
        lines.append('| {} | {} | {} | {} | '.format(row['subject'], row['path'], row['shadows'], row['poses']) +
                     ' | '.join(values) + ' |')
    lines += ['', '## Every cell', '']
    lines.append('| subject | pose | path | shadows | size | clean runs | frame GPU p50 | main-view cull | '
                 'shadow-view culls | shadow casters | forward draw (+prepass) | ScenePass Δ | frame Δ vs no foliage | '
                 'CPU-cull Δ | no-density-LOD Δ |')
    lines.append('|' + '---|' * 15)
    for cell in summary['cells']:
        m = cell['metrics']
        size = '{}x{}'.format(cell['width'], cell['height']) + (' MSAA{}'.format(cell['msaa']) if cell['msaa'] > 1 else '')
        lines.append('| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |'.format(
            cell['subject'], cell['pose'], cell['path'], cell['shadows'], size, cell['runs'], fmt(m.get('gpuP50')),
            fmt(m.get('cullMain')), fmt(m.get('cullShadow')), fmt(m.get('shadowCasters')), fmt(m.get('forwardDraw')),
            fmt(m.get('scenePassDeltaVsNoFoliage')), fmt(m.get('frameDeltaVsNoFoliage')), fmt(m.get('frameDeltaCpuCull')),
            fmt(m.get('frameDeltaNoDensityLod'))))
    lines += ['', '| pose | path | shadows | rebuild every | clean runs | wall p50 | wall p95 | wall p99 | wall max | '
                  'misses | GPU p50 | GPU p95 | GPU p99 | rebuild-frame wall |', '|' + '---|' * 14]
    for tail in summary['tails']:
        lines.append('| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |'.format(
            tail['pose'], tail['path'], tail['shadows'], tail['rebuildEvery'] or '—', tail['runs'], fmt(tail['wallP50'], 2),
            fmt(tail['wallP95'], 2), fmt(tail['wallP99'], 2), fmt(tail['wallMax'], 2),
            fmt(tail['wallDeadlinemisses'], 0), fmt(tail['gpuP50'], 2), fmt(tail['gpuP95'], 2), fmt(tail['gpuP99'], 2),
            fmt(tail['rebuildFrameWallMs'], 1)))
    return '\n'.join(lines) + '\n'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--exe', type=Path)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--env', action='append', default=[], help='KEY=VALUE passed to the test (e.g. OLO_FOLIAGE_COST_ROUNDS=2)')
    parser.add_argument('--summarise-only', action='store_true')
    parser.add_argument('--include-unsampled', action='store_true',
                        help='also summarise runs that have no contention sampler record')
    args = parser.parse_args()

    if not args.summarise_only:
        if not args.exe or not args.exe.exists():
            parser.error('--exe must name the Release OloEngine-Tests binary')
        environment = dict(item.split('=', 1) for item in args.env)
        args.output.mkdir(parents=True, exist_ok=True)
        start = max((run_number(p) for p in args.output.glob('run-*')), default=0)
        for k in range(args.runs):
            run = args.output / f'run-{start + k + 1}'
            print(f'[foliage-cost] {run.name} ...', flush=True)
            host = run_once(args.exe.resolve(), args.repo.resolve(), run, environment)
            print(f'[foliage-cost] {run.name}: {host["elapsedSeconds"]} s, contended={host["contended"]}', flush=True)
    summarise(args.output, args.include_unsampled)
    print(f'[foliage-cost] summary: {args.output / "summary.md"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
