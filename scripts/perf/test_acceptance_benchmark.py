"""The acceptance driver's aggregation, verdict, contention and refusal rules,
on synthetic capture directories: no GPU, no engine."""
import contextlib
import copy
import csv
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('acceptance_benchmark', Path(__file__).with_name('acceptance-benchmark.py'))
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)

GATE = 16.667
MANIFEST = ('ManifestVersion: 2\nId: acceptance-test\nScene: Scenes/Test.olo\nRendererSettings:\n  Path: Deferred\n'
            '  VirtualShadowMaps: true\n  MSAASampleCount: 1\nMeasurement:\n  Frames: 120\n')
COLUMNS = ['camera', 'index', 'renderCallMs', 'cpuMs', 'fenceWaitMs', 'presentWaitMs', 'gpuFrameId', 'gpuMs',
           'gpuStatus', 'gpuResidentBytes', 'cpuTrackedBytes', 'drawCalls', 'recordingWallMs',
           'recordingJoinWaitMs', 'cpuFrameId']
MIB = 1 << 20


class Capture:
    """A minimal schema-v2 result directory the driver can read."""

    def __init__(self, frame_ms=10.0, cameras=('stationary', 'dolly'), frames=120, host='test-binary',
                 backend='opengl'):
        settings = {'path': 'Deferred', 'msaaSamples': 1, 'upscaleMode': 0, 'upscaleTechnique': 0,
                    'virtualShadowMaps': True, 'lightingTap': 'none'}
        self.result = {
            'resultSchemaVersion': 2,
            'manifest': {'id': 'acceptance-test', 'sourceHashFnv1a64': 'cafe'},
            'provenance': {'backend': backend, 'host': host, 'gpuRenderer': 'Test GPU', 'commitSha': 'deadbeef'},
            'output': {'actual': {'renderWidth': 1920, 'renderHeight': 1080, 'displayWidth': 1920,
                                  'displayHeight': 1080, 'renderScale': 1.0, 'internalSource': 'native',
                                  'upscaler': {'resolved': 'native', 'fallback': 'none'}}},
            'determinism': {'warmupTimedOut': False},
            'configuration': {'requested': {**settings, 'rayTracedShadows': False},
                              'selectedSettings': {**settings, 'rayTracedShadowsRequested': False}},
            'measurement': {'rawFile': 'measurement.csv', 'passRawFile': 'measurement-passes.jsonl',
                            'metric': 'test interval', 'sampleCount': frames * len(cameras), 'deadlineMs': 33.333,
                            'continuousEditorFrames': host == 'editor-mcp', 'traceOverflow': False,
                            'peakGpuResidentBytes': 100 * MIB, 'liveGpuResidentBytes': 90 * MIB,
                            'peakCpuTrackedBytes': 10 * MIB, 'gpuResidentBytesAfterSceneRelease': 50 * MIB},
            'memory': {'gpu': {'peakBytes': 120 * MIB, 'liveBytes': 90 * MIB, 'residentBytes': 95 * MIB},
                       'cpu': {'peakBytes': 12 * MIB, 'liveBytes': 9 * MIB},
                       'backend': {'name': 'OpenGL', 'reconciliation': {'status': 'estimated'},
                                   'residency': {'status': 'unknown'}},
                       'owners': [{'owner': 'GBuffer', 'gpuLiveBytes': 64 * MIB, 'gpuRetiringBytes': 0,
                                   'cpuLiveBytes': 0, 'count': 6},
                                  {'owner': 'Shadows', 'gpuLiveBytes': 16 * MIB, 'gpuRetiringBytes': 0,
                                   'cpuLiveBytes': 2 * MIB, 'count': 3}]},
        }
        self.manifest = MANIFEST
        self.rows = []
        self.pass_rows = []
        frame_id = 100
        cpu_frame = 1
        for camera in cameras:
            for index in range(frames):
                frame_id += 1
                value = frame_ms(camera, index) if callable(frame_ms) else frame_ms
                self.rows.append({'camera': camera, 'index': index, 'renderCallMs': value, 'cpuMs': 4.0,
                                  'fenceWaitMs': 0.5, 'presentWaitMs': 0.25, 'gpuFrameId': frame_id, 'gpuMs': 8.0,
                                  'gpuStatus': 'valid', 'gpuResidentBytes': 0, 'cpuTrackedBytes': 0, 'drawCalls': 9,
                                  'recordingWallMs': 1.0, 'recordingJoinWaitMs': 0.1, 'cpuFrameId': cpu_frame})
                cpu_frame += 1
                self.pass_rows.append({'camera': camera, 'index': index, 'gpuFrameId': frame_id, 'passes': [
                    {'pass': 'ScenePass', 'status': 'valid', 'gpuMs': 3.0, 'isSubPass': False, 'parent': ''},
                    {'pass': 'Depth', 'status': 'valid', 'gpuMs': 1.0, 'isSubPass': True, 'parent': 'ScenePass'},
                    {'pass': 'LightingPass', 'status': 'valid', 'gpuMs': 2.0, 'isSubPass': False, 'parent': ''}]})

    def write(self, directory: Path) -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'result.json').write_text(json.dumps(self.result), encoding='utf-8')
        if self.manifest is not None:
            (directory / 'manifest.yaml').write_text(self.manifest, encoding='utf-8')
        with (directory / 'measurement.csv').open('w', newline='', encoding='utf-8') as stream:
            writer = csv.DictWriter(stream, fieldnames=COLUMNS)
            writer.writeheader()
            writer.writerows(self.rows)
        (directory / 'measurement-passes.jsonl').write_text(
            '\n'.join(json.dumps(row) for row in self.pass_rows) + '\n', encoding='utf-8')
        return directory


class TempCase(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)


class PassAggregationTest(TempCase):
    def test_invalid_duplicate_and_subpass_rules(self):
        capture = Capture(frames=120, cameras=('stationary',))
        rows = capture.pass_rows
        rows[0]['passes'][0].update(status='pending', gpuMs=None)        # invalid status
        rows[1]['passes'][2].update(gpuMs=None)                          # valid but no value
        rows[2]['gpuFrameId'] = rows[3]['gpuFrameId']                    # same frame resolved twice
        rows[4]['gpuFrameId'] = 0                                        # no frame identity
        analysis = acceptance.analyse_run(capture.write(self.root / 'run'), GATE)
        scenario = analysis['scenarios']['stationary']
        # Every pass entry of row 4 and the later copy of the duplicated frame are excluded.
        self.assertEqual(scenario['passes']['ScenePass']['count'], 120 - 3)
        self.assertEqual(scenario['passes']['LightingPass']['count'], 120 - 3)
        self.assertEqual(scenario['subPasses']['ScenePass/Depth']['count'], 120 - 2)
        self.assertEqual(scenario['subPasses']['ScenePass/Depth']['parent'], 'ScenePass')
        self.assertEqual(analysis['passExcluded'],
                         {'status:pending': 1, 'invalid-value': 1, 'duplicate-frame-pass': 3, 'missing-frame-id': 3})
        self.assertEqual(analysis['passExcludedByPass']['ScenePass'],
                         {'status:pending': 1, 'duplicate-frame-pass': 1, 'missing-frame-id': 1})
        self.assertEqual(analysis['invalidReasons'], [])

    def test_subpass_is_never_added_to_its_parent(self):
        capture = Capture(cameras=('stationary',))
        analysis = acceptance.analyse_run(capture.write(self.root / 'run'), GATE)
        scenario = analysis['scenarios']['stationary']
        self.assertEqual(scenario['passes']['ScenePass']['p50Ms'], 3.0)
        self.assertNotIn('ScenePass/Depth', scenario['passes'])
        self.assertNotIn('Depth', scenario['passes'])

    def test_prefixed_subpass_name_is_not_prefixed_again(self):
        records = [{'camera': 'c', 'index': 0, 'gpuFrameId': 5, 'passes': [
            {'pass': 'Shadow/Cascade0', 'status': 'valid', 'gpuMs': 1.0, 'isSubPass': True, 'parent': 'Shadow'}]}]
        out, excluded, _, rows = acceptance.aggregate_passes(records, {'c': 1})
        self.assertEqual(out['c']['subPasses'], {'Shadow/Cascade0': [1.0]})
        self.assertEqual((excluded, rows), ({}, 1))

    def test_repeated_subpass_in_one_frame_is_summed(self):
        # One frame brackets each VSM clip level's foliage separately (#1590):
        # those intervals are disjoint, so the frame's sample is their sum.
        sub = {'status': 'valid', 'isSubPass': True, 'parent': 'ShadowPass'}
        records = [{'camera': 'c', 'index': 0, 'gpuFrameId': 5, 'passes': [
            {'pass': 'ShadowPass', 'status': 'valid', 'gpuMs': 9.0},
            dict(sub, **{'pass': 'VsmSun/FoliageCull', 'gpuMs': 1.0}),
            dict(sub, **{'pass': 'VsmSun/FoliageCull', 'gpuMs': 2.5}),
            dict(sub, **{'pass': 'Atlas/Terrain', 'gpuMs': 1.0}),
            dict(sub, **{'pass': 'Atlas/Terrain', 'status': 'pending', 'gpuMs': None})]},
            {'camera': 'c', 'index': 1, 'gpuFrameId': 5, 'passes': [
                dict(sub, **{'pass': 'VsmSun/FoliageCull', 'gpuMs': 7.0})]}]
        out, excluded, _, rows = acceptance.aggregate_passes(records, {'c': 2})
        self.assertEqual(out['c']['passes'], {'ShadowPass': [9.0]})
        self.assertEqual(out['c']['subPasses'], {'ShadowPass/VsmSun/FoliageCull': [3.5]})
        # One part pending excludes the frame's whole sum; a second row that
        # resolved the same frame is still a duplicate.
        self.assertEqual(excluded, {'status:pending': 1, 'duplicate-frame-pass': 1})
        self.assertEqual(rows, 2)

    def test_unexpected_pass_row_is_refused(self):
        records = [{'camera': 'elsewhere', 'index': 0, 'gpuFrameId': 1, 'passes': []}]
        with self.assertRaisesRegex(ValueError, 'unexpected or duplicate'):
            acceptance.aggregate_passes(records, {'c': 1})

    def test_truncated_pass_file_invalidates_the_run(self):
        capture = Capture(cameras=('stationary',))
        capture.pass_rows.pop()
        analysis = acceptance.analyse_run(capture.write(self.root / 'run'), GATE)
        self.assertTrue(any('truncated per-pass' in r for r in analysis['invalidReasons']))


class RunAnalysisTest(TempCase):
    def test_total_gpu_counts_only_valid_distinct_frames(self):
        capture = Capture(cameras=('stationary',))
        capture.rows[0].update(gpuStatus='pending', gpuMs='')
        capture.rows[1]['gpuFrameId'] = capture.rows[2]['gpuFrameId']
        capture.rows[3].update(gpuStatus='duplicate', gpuMs='')
        analysis = acceptance.analyse_run(capture.write(self.root / 'run'), GATE)
        scenario = analysis['scenarios']['stationary']
        self.assertEqual(scenario['gpu']['count'], 117)
        self.assertEqual(scenario['gpuExcluded'], {'pending': 1, 'duplicate-frame-id': 1, 'duplicate': 1})

    def test_distribution_and_gate_misses(self):
        capture = Capture(frame_ms=lambda camera, i: 20.0 if i < 10 else 10.0, cameras=('stationary',))
        frame = acceptance.analyse_run(capture.write(self.root / 'run'), GATE)['scenarios']['stationary']['frameTime']
        self.assertEqual(frame['deadlineMisses'], 10)
        self.assertEqual((frame['p50Ms'], frame['p95Ms'], frame['maxMs']), (10.0, 20.0, 20.0))

    def test_missing_actual_sizes_are_refused(self):
        capture = Capture()
        capture.result['output']['actual'] = None
        with self.assertRaisesRegex(ValueError, 'output.actual is absent'):
            acceptance.analyse_run(capture.write(self.root / 'a'), GATE)
        capture.result['output']['actual'] = {'renderWidth': 0, 'renderHeight': 1080,
                                              'displayWidth': 1920, 'displayHeight': 1080}
        with self.assertRaisesRegex(ValueError, 'renderWidth'):
            acceptance.analyse_run(capture.write(self.root / 'b'), GATE)

    def test_sizes_upscaler_and_vsm_are_recorded(self):
        capture = Capture()
        capture.result['output']['actual'].update(renderWidth=1280, renderHeight=720,
                                                  upscaler={'resolved': 'fsr1', 'fallback': 'fsr2-unsupported'})
        analysis = acceptance.analyse_run(capture.write(self.root / 'run'), GATE)
        self.assertEqual(analysis['sizes']['render'], [1280, 720])
        self.assertEqual(analysis['sizes']['upscaler'], {'resolved': 'fsr1', 'fallback': 'fsr2-unsupported'})
        self.assertIs(analysis['techniques']['virtualShadowMapsSelected'], True)
        self.assertEqual(analysis['memory']['report']['topGpuOwners'][0]['owner'], 'GBuffer')

    def test_pinned_setting_not_selected_invalidates_unpinned_does_not(self):
        capture = Capture()
        capture.result['configuration']['selectedSettings']['virtualShadowMaps'] = False
        capture.result['configuration']['selectedSettings']['rayTracedShadowsRequested'] = True  # not pinned
        analysis = acceptance.analyse_run(capture.write(self.root / 'a'), GATE)
        self.assertEqual(len(analysis['invalidReasons']), 1)
        self.assertIn('virtualShadowMaps', analysis['invalidReasons'][0])
        del capture.result['configuration']['selectedSettings']['virtualShadowMaps']
        analysis = acceptance.analyse_run(capture.write(self.root / 'b'), GATE)
        self.assertIn('not recorded', analysis['invalidReasons'][0])

    def test_truncation_timeout_and_short_runs_are_invalid(self):
        capture = Capture(frames=50, cameras=('stationary',))
        capture.result['measurement']['sampleCount'] = 51
        capture.result['determinism']['warmupTimedOut'] = True
        reasons = acceptance.analyse_run(capture.write(self.root / 'run'), GATE)['invalidReasons']
        self.assertTrue(any('truncated capture' in r for r in reasons))
        self.assertTrue(any('timed out' in r for r in reasons))
        self.assertTrue(any('at least 100' in r for r in reasons))

    def test_editor_trace_gap_is_invalid(self):
        capture = Capture(host='editor-mcp', backend='vulkan', cameras=('stationary',))
        capture.rows[60]['cpuFrameId'] += 1
        reasons = acceptance.analyse_run(capture.write(self.root / 'run'), GATE)['invalidReasons']
        self.assertTrue(any('gap or duplicate' in r for r in reasons))


def record(directory, status_hint=None, contention=(), cell='gl-test', binary='aa'):
    return {'cell': cell, 'host': 'gl', 'directory': str(directory), 'binarySha256': binary,
            'contentionReasons': list(contention)}


class AcrossRunTest(TempCase):
    def summarise(self, records, repeats=3):
        return acceptance.summarise(records, acceptance.resolve_record_dir, gate_ms=GATE, repeats=repeats,
                                    output=self.root / 'out')

    def runs(self, values, **kwargs):
        return [record(Capture(frame_ms=v).write(self.root / f'run-{i}'), **kwargs) for i, v in enumerate(values)]

    def test_range_over_runs_and_pass(self):
        summary = self.summarise(self.runs([10.0, 14.0, 12.0]))
        cell = summary['cells']['gl-test']
        frame = cell['scenarios']['stationary']['channels']['frameTime']['range']
        self.assertEqual(frame['p95Ms'], [10.0, 14.0])
        self.assertEqual(frame['deadlineMisses'], [0, 0])
        self.assertEqual(cell['scenarios']['dolly']['passes']['ScenePass']['range']['p50Ms'], [3.0, 3.0])
        self.assertEqual((cell['verdict'], summary['overall']), ('PASS', 'PASS'))
        self.assertTrue((self.root / 'out' / 'summary.md').read_text(encoding='utf-8').count('Top passes by p50'))

    def test_one_run_over_the_gate_fails_the_cell(self):
        cell = self.summarise(self.runs([10.0, 20.0, 12.0]))['cells']['gl-test']
        self.assertEqual(cell['verdict'], 'FAIL')
        self.assertIn('run 2', cell['verdictReason'])

    def test_contended_run_is_excluded_and_cell_inconclusive(self):
        runs = self.runs([10.0, 10.0, 25.0])
        runs[2]['contentionReasons'] = ['competing process OloEditor.exe (pid 42, C:/x/OloEditor.exe) at t']
        cell = self.summarise(runs)['cells']['gl-test']
        self.assertEqual((cell['verdict'], cell['countedRuns']), ('INCONCLUSIVE', 2))
        self.assertEqual(cell['notCounted'][0]['status'], 'excluded')
        self.assertIn('OloEditor', cell['notCounted'][0]['reasons'][0])

    def test_required_pass_without_valid_samples_is_not_counted(self):
        runs = self.runs([10.0, 10.0, 10.0])
        for run in runs:
            run['requirePasses'] = ['LightingPass', 'ScenePass/Depth']
        self.assertEqual(self.summarise(copy.deepcopy(runs))['cells']['gl-test']['verdict'], 'PASS')
        runs[0]['requirePasses'] = ['VirtualShadowMapMarkPass']
        cell = self.summarise(runs)['cells']['gl-test']
        self.assertEqual((cell['verdict'], cell['notCounted'][0]['status']), ('INCONCLUSIVE', 'invalid'))
        self.assertIn('VirtualShadowMapMarkPass has no valid GPU sample', cell['notCounted'][0]['reasons'][0])

    def test_run_without_sampler_record_is_excluded_unless_asked(self):
        runs = self.runs([10.0, 10.0, 10.0])
        runs[0]['contentionReasons'] = None
        self.assertEqual(self.summarise(runs)['cells']['gl-test']['verdict'], 'INCONCLUSIVE')

    def test_mixed_binaries_are_refused(self):
        runs = self.runs([10.0, 10.0, 10.0])
        runs[1]['binarySha256'] = 'bb'
        with self.assertRaisesRegex(ValueError, 'different binaries'):
            self.summarise(runs)

    def test_gate_verdict_rules(self):
        self.assertEqual(acceptance.gate_verdict([{'a': 10}] * 3, 3, GATE)['verdict'], 'PASS')
        self.assertEqual(acceptance.gate_verdict([{'a': 10}] * 2, 3, GATE)['verdict'], 'INCONCLUSIVE')
        self.assertEqual(acceptance.gate_verdict([{'a': 10, 'b': 17}], 3, GATE)['verdict'], 'FAIL')
        self.assertEqual(acceptance.gate_verdict([], 3, GATE)['verdict'], 'INCONCLUSIVE')
        self.assertEqual(acceptance.gate_verdict([{'a': GATE}] * 3, 3, GATE)['verdict'], 'PASS')

    def test_summarise_only_exit_codes(self):
        for values, code in (([10.0, 11.0, 12.0], 0), ([10.0, 30.0, 12.0], 1), ([10.0, 11.0], 2)):
            with self.subTest(values=values):
                campaign = self.root / f'campaign-{code}'
                records = []
                for i, value in enumerate(values):
                    Capture(frame_ms=value).write(campaign / 'gl-test' / f'round-{i + 1:02d}-attempt-1')
                    records.append({'cell': 'gl-test', 'host': 'gl', 'binarySha256': 'aa', 'contentionReasons': [],
                                    'directory': f'gl-test/round-{i + 1:02d}-attempt-1'})
                (campaign / 'runs.json').write_text(json.dumps(records), encoding='utf-8')
                (campaign / 'plan.json').write_text(json.dumps({'gateMs': GATE, 'repeats': 3}), encoding='utf-8')
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(acceptance.main(['--summarise-only', '--run-dir', str(campaign)]), code)
                written = json.loads((campaign / 'runs.json').read_text(encoding='utf-8'))
                self.assertEqual(written[0]['directory'], 'gl-test/round-01-attempt-1')
                self.assertEqual(written[0]['status'], 'counted')


class ContentionTest(unittest.TestCase):
    DRIVER = 1000

    def processes(self):
        return [
            {'pid': self.DRIVER, 'ppid': 1, 'created': 10, 'name': 'python.exe', 'path': 'C:/py/python.exe'},
            {'pid': 1001, 'ppid': self.DRIVER, 'created': 20, 'name': 'OloEngine-Tests.exe', 'path': 'C:/b/T.exe'},
            {'pid': 1002, 'ppid': 1001, 'created': 21, 'name': 'cl.exe', 'path': 'C:/vs/cl.exe'},
            # Older than its "parent": its real parent died and the PID was reused by our child.
            {'pid': 1003, 'ppid': 1001, 'created': 5, 'name': 'ninja.exe', 'path': 'C:/n/ninja.exe'},
            {'pid': 2000, 'ppid': 1, 'created': 15, 'name': 'OloEditor.exe', 'path': 'C:/other/OloEditor.exe'},
            {'pid': 2001, 'ppid': 1, 'created': 15, 'name': 'firefox.exe', 'path': 'C:/ff/firefox.exe'},
        ]

    def reduce(self, gpu_apps):
        return acceptance.reduce_sample('t', self.processes(), gpu_apps, 12.0, driver_pid=self.DRIVER,
                                        competitors=acceptance.COMPETITORS, gpu_baseline={'c:/ff/firefox.exe'},
                                        ignore=['SnippingTool'])

    def test_own_descendants_are_not_contention_strangers_are(self):
        sample = self.reduce([])
        self.assertEqual(sorted(p['pid'] for p in sample['foreignProcesses']), [1003, 2000])

    def test_new_gpu_processes_against_the_baseline(self):
        apps = [{'pid': 2001, 'path': 'C:/ff/firefox.exe'}, {'pid': 1001, 'path': 'C:/b/T.exe'},
                {'pid': 3000, 'path': 'C:/games/WowB.exe'}, {'pid': 3001, 'path': 'C:/x/Game.exe'},
                {'pid': 3002, 'path': 'C:/w/SnippingTool.exe'}]
        sample = self.reduce(apps)
        # WowB is a named competitor (reported as a process, not a new GPU user); ours and the baseline are not.
        self.assertEqual([p['pid'] for p in sample['newGpuProcesses']], [3001])
        self.assertIsNone(self.reduce(None)['newGpuProcesses'])

    def test_a_new_gpu_process_counts_only_while_it_uses_the_gpu(self):
        apps = [{'pid': 3001, 'path': 'C:/x/Game.exe'}, {'pid': 3003, 'path': 'C:/c/claude.exe'},
                {'pid': 3004, 'path': 'C:/v/Player.exe'}]

        def reduce(load):
            return acceptance.reduce_sample('t', self.processes(), apps, 12.0, driver_pid=self.DRIVER,
                                            competitors=acceptance.COMPETITORS, gpu_baseline=set(), ignore=[],
                                            gpu_load=load, new_gpu_util_max=5.0)

        # A context holder at or under the limit is not contention; one over it is, and a PID with no reading counts.
        sample = reduce({3001: 41.0, 3003: 0.2, 3004: 5.0})
        self.assertEqual([(p['pid'], p['gpuLoad']) for p in sample['newGpuProcesses']], [(3001, 41.0)])
        self.assertEqual([p['pid'] for p in reduce({3001: 41.0})['newGpuProcesses']], [3001, 3003, 3004])
        # Counters unreadable: every candidate counts, as before the load check existed.
        self.assertEqual([p['pid'] for p in reduce(None)['newGpuProcesses']], [3001, 3003, 3004])
        def gpu_reasons(samples):
            return [r for r in acceptance.contention_reasons(samples) if r.startswith('new GPU process')]

        self.assertEqual(gpu_reasons([reduce({3001: 41.0, 3003: 0.2, 3004: 0.0}),
                                      reduce({3001: 63.0, 3003: 0.2, 3004: 0.0})]),
                         ['new GPU process Game.exe (pid 3001, C:/x/Game.exe, up to 63% GPU) first seen at t'])
        self.assertIn('GPU load unreadable', gpu_reasons([reduce(None)])[0])

    def test_gpu_engine_load_is_the_busiest_engine_type_per_process(self):
        lines = ['pid_7_luid_0x0_0xfdbc_phys_0_eng_0_engtype_3d|20.5',
                 'pid_7_luid_0x0_0xfdbc_phys_0_eng_1_engtype_3D|10',
                 'pid_7_luid_0x0_0xfdbc_phys_0_eng_10_engtype_copy|25',
                 'pid_9_luid_0x0_0xfdbc_phys_0_eng_0_engtype_videodecode|3',
                 'not an engine|50', 'pid_11_luid_0x0_0xfdbc_phys_0_eng_0_engtype_3d|n/a']
        self.assertEqual(acceptance.parse_gpu_engine_load(lines), {7: 30.5, 9: 3.0})

    def test_a_non_finite_gpu_engine_reading_is_no_reading(self):
        lines = ['pid_7_luid_0x0_0xfdbc_phys_0_eng_0_engtype_3d|nan',
                 'pid_8_luid_0x0_0xfdbc_phys_0_eng_0_engtype_3d|inf',
                 'pid_9_luid_0x0_0xfdbc_phys_0_eng_0_engtype_3d|nan',
                 'pid_9_luid_0x0_0xfdbc_phys_0_eng_1_engtype_copy|12']
        self.assertEqual(acceptance.parse_gpu_engine_load(lines), {9: 12.0})

    def test_reasons_name_the_process_and_exclude_the_run(self):
        reasons = acceptance.contention_reasons([self.reduce([]), self.reduce([])])
        self.assertEqual(len(reasons), 2)  # deduplicated across samples
        self.assertTrue(any('OloEditor.exe' in r and 'pid 2000' in r for r in reasons))
        build = [{'time': 't', 'gpuUtilization': None, 'newGpuProcesses': None,
                  'foreignProcesses': [{'pid': pid, 'name': 'clang-cl.exe', 'path': 'C:/llvm/clang-cl.exe'}
                                       for pid in (5, 6, 7)]}]
        self.assertEqual(acceptance.contention_reasons(build),
                         ['competing process clang-cl.exe (3 instances, first pid 5, C:/llvm/clang-cl.exe) '
                          'first seen at t'])
        status, why = acceptance.classify({'host': 'gl', 'contentionReasons': reasons},
                                          {'invalidReasons': [], 'host': 'test-binary', 'backend': 'opengl'}, None)
        self.assertEqual((status, why), ('excluded', reasons))
        failed = acceptance.contention_reasons([{'time': 't', 'error': 'pwsh missing'}])
        self.assertIn('unverifiable', failed[0])

    def test_prelaunch_gpu_utilisation_limit(self):
        quiet = {'time': 't', 'gpuUtilization': 31.0, 'foreignProcesses': [], 'newGpuProcesses': []}
        self.assertIn('idle limit', acceptance.prelaunch_reasons(quiet, 30.0)[0])
        self.assertEqual(acceptance.prelaunch_reasons(quiet, 40.0), [])

    def test_wait_for_quiet(self):
        busy = {'time': 't', 'gpuUtilization': 5.0, 'newGpuProcesses': [],
                'foreignProcesses': [{'pid': 9, 'name': 'ninja.exe', 'path': ''}]}
        idle = {**busy, 'foreignProcesses': []}
        samples = iter([busy, idle])
        clock = iter(range(100))
        quiet, history = acceptance.wait_for_quiet(lambda util_readings: next(samples), 30, 600,
                                                   sleep=lambda s: None, clock=lambda: next(clock))
        self.assertEqual((quiet, len(history)), (True, 2))
        clock = iter(range(0, 10000, 100))
        quiet, history = acceptance.wait_for_quiet(lambda util_readings: busy, 30, 250,
                                                   sleep=lambda s: None, clock=lambda: next(clock))
        self.assertFalse(quiet)
        self.assertIn('ninja', history[-1]['reasons'][0])


class ScheduleAndRetryTest(TempCase):
    def test_rotation(self):
        self.assertEqual(acceptance.schedule(['a', 'b', 'c'], 3),
                         [['a', 'b', 'c'], ['b', 'c', 'a'], ['c', 'a', 'b']])

    def run_campaign(self, outcome, max_retries):
        plan = {'cells': [{'id': 'a', 'host': 'gl', 'manifest': 'm'}, {'id': 'b', 'host': 'vulkan', 'manifest': 'm'}],
                'repeats': 3, 'maxRetries': max_retries, 'shaderRoot': str(self.root / 's'),
                'assetRoot': str(self.root / 'x'), 'gateMs': GATE}
        output = self.root / 'out'
        output.mkdir()
        calls = []

        def runner(plan, cell, round_index, attempt, output, campaign, probe):
            calls.append((cell['id'], round_index + 1, attempt))
            status = outcome(cell['id'], round_index + 1, attempt)
            return {'cell': cell['id'], 'status': status, 'reasons': [] if status == 'counted' else ['x']}

        probe = type('Probe', (), {'gpu_baseline': []})()
        with patch.object(acceptance, 'build_provenance', return_value=({'assets': {}}, {}, [])), \
                patch.object(acceptance, 'fingerprint', return_value={}), \
                patch.object(acceptance, 'sha256_file', return_value='0'), \
                contextlib.redirect_stdout(io.StringIO()):
            acceptance.execute(plan, output, 60, probe=probe, attempt_runner=runner)
        return calls

    def test_excluded_run_is_retried_at_the_end_of_its_round(self):
        calls = self.run_campaign(lambda c, r, a: 'excluded' if (c, a) == ('a', 1) else 'counted', 2)
        self.assertEqual(calls, [('a', 1, 1), ('b', 1, 1), ('a', 1, 2), ('b', 2, 2), ('a', 2, 3),
                                 ('a', 3, 4), ('b', 3, 3)])

    def test_retry_budget_is_per_cell_and_bounded(self):
        calls = self.run_campaign(lambda c, r, a: 'excluded' if c == 'a' else 'counted', 1)
        self.assertEqual(sum(1 for c in calls if c[0] == 'a'), 4)
        self.assertEqual(sum(1 for c in calls if c[0] == 'b'), 3)

    def test_dry_run_prints_the_interleaved_schedule(self):
        plan = self.root / 'plan.json'
        plan.write_text(json.dumps({'buildConfig': 'Release', 'compiler': 'clang-cl', 'repo': str(self.root),
                                    'exe': 'build/Release/T.exe', 'editorExe': 'build/Release/E.exe',
                                    'cells': [{'id': 'gl', 'host': 'gl', 'manifest': 'm.yaml'},
                                              {'id': 'vk', 'host': 'vulkan', 'manifest': 'm.yaml'}]}), encoding='utf-8')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(acceptance.main(['--plan', str(plan), '--dry-run']), 0)
        self.assertIn('round 1: gl, vk', out.getvalue())
        self.assertIn('round 2: vk, gl', out.getvalue())


class PlanRefusalTest(TempCase):
    BASE = {'buildConfig': 'Release', 'compiler': 'clang-cl', 'exe': 'build/Release/T.exe',
            'cells': [{'id': 'gl', 'host': 'gl', 'manifest': 'm.yaml'}]}

    def load(self, **changes):
        raw = {**copy.deepcopy(self.BASE), **changes}
        return acceptance.load_plan(raw, default_repo=self.root, check_files=False)

    def test_release_with_declared_compiler_only(self):
        self.assertEqual(self.load()['gateMs'], GATE)
        with self.assertRaisesRegex(ValueError, 'Release'):
            self.load(buildConfig='Debug')
        with self.assertRaisesRegex(ValueError, 'compiler'):
            self.load(compiler='')
        with self.assertRaisesRegex(ValueError, 'Release only'):
            self.load(exe='build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe')
        with self.assertRaisesRegex(ValueError, 'repeats'):
            self.load(repeats=2)
        with self.assertRaisesRegex(ValueError, 'editorExe'):
            self.load(cells=[{'id': 'vk', 'host': 'vulkan', 'manifest': 'm.yaml'}])
        with self.assertRaisesRegex(ValueError, 'host'):
            self.load(cells=[{'id': 'x', 'host': 'dx12', 'manifest': 'm.yaml'}])

    def test_new_gpu_process_limit_defaults_and_is_validated(self):
        self.assertEqual(self.load()['newGpuProcessUtilMax'], 5.0)
        self.assertEqual(self.load(newGpuProcessUtilMax=0)['newGpuProcessUtilMax'], 0.0)
        for bad in (float('nan'), float('inf'), -1, True, '5'):
            with self.assertRaisesRegex(ValueError, 'newGpuProcessUtilMax'):
                self.load(newGpuProcessUtilMax=bad)

    def test_cell_env_is_kept_and_must_map_names_to_strings(self):
        self.assertEqual(self.load()['cells'][0]['env'], {})
        lever = [{'id': 'gl', 'host': 'gl', 'manifest': 'm.yaml', 'env': {'OLO_NO_SHADOWPASS_BRACKETS': '1'}}]
        self.assertEqual(self.load(cells=lever)['cells'][0]['env'], {'OLO_NO_SHADOWPASS_BRACKETS': '1'})
        for bad in (['OLO_X'], {'OLO_X': 1}, {'': '1'}):
            with self.assertRaisesRegex(ValueError, 'env'):
                self.load(cells=[{'id': 'gl', 'host': 'gl', 'manifest': 'm.yaml', 'env': bad}])

    def test_every_cells_env_keys_are_cleared_for_every_cell(self):
        # The control cell has no env of its own, yet the lever's key is in the
        # set cleared before each launch, so a shell-set lever cannot leak in.
        plan = self.load(cells=[{'id': 'base', 'host': 'gl', 'manifest': 'm.yaml'},
                                {'id': 'lever', 'host': 'gl', 'manifest': 'm.yaml',
                                 'env': {'OLO_NO_SHADOWPASS_BRACKETS': '1'}}])
        self.assertEqual(plan['cellEnvKeys'], ['OLO_NO_SHADOWPASS_BRACKETS'])

    def test_driver_owned_env_and_impossible_requirements_are_refused(self):
        for owned in ('OLO_SHADER_CACHE_DIR', 'OLO_CAPTURE_TEST_DELAY_MS'):
            with self.assertRaisesRegex(ValueError, 'the driver sets it'):
                self.load(cells=[{'id': 'gl', 'host': 'gl', 'manifest': 'm.yaml', 'env': {owned: '1'}}])
        with self.assertRaisesRegex(ValueError, 'never publishes'):
            self.load(cells=[{'id': 'gl', 'host': 'gl', 'manifest': 'm.yaml', 'requirePasses': ['ShadowPass/Atlas'],
                              'env': {'OLO_NO_SHADOWPASS_BRACKETS': '1'}}])
        # The pass's own bracket stays, so requiring ShadowPass itself is fine.
        self.load(cells=[{'id': 'gl', 'host': 'gl', 'manifest': 'm.yaml', 'requirePasses': ['ShadowPass'],
                          'env': {'OLO_NO_SHADOWPASS_BRACKETS': '1'}}])

    def test_cli_overrides(self):
        plan = acceptance.load_plan(copy.deepcopy(self.BASE), default_repo=self.root, gate_ms=33.333, repeats=5,
                                    max_retries=0, check_files=False)
        self.assertEqual((plan['gateMs'], plan['repeats'], plan['maxRetries']), (33.333, 5, 0))


class ProvenanceTest(TempCase):
    def setUp(self):
        super().setUp()
        assets = self.root / 'OloEditor/SandboxProject/Assets'
        (assets / 'Scenes').mkdir(parents=True)
        (assets / 'Models').mkdir()
        (assets / 'Models/tree.obj').write_text('v 0 0 0\n', encoding='utf-8')
        (assets / 'unused.bin').write_text('unused', encoding='utf-8')
        (self.root / 'OloEditor/assets/models/Fox').mkdir(parents=True)
        (self.root / 'OloEditor/assets/models/Fox/Fox.gltf').write_text('{}', encoding='utf-8')
        (assets / 'Scenes/Test.olo').write_text('Mesh: Assets/Models/tree.obj\nFile: assets/models/Fox/Fox.gltf\n'
                                                'Raw: d.raw\n', encoding='utf-8')
        self.manifest = self.root / 'm.yaml'
        self.manifest.write_text('Scene: Scenes/Test.olo\nAssets:\n- Path: assets/models/Fox/Fox.gltf\n  Sha256: '
                                 + '0' * 64 + '\n- Path: SandboxProject/Assets/Missing.png\n', encoding='utf-8')
        self.assets = assets

    def test_manifest_references(self):
        references, unresolved = acceptance.manifest_references(self.manifest, self.root, self.assets)
        names = sorted(acceptance._relative(p, self.root) for p in references)
        self.assertEqual(names, ['OloEditor/SandboxProject/Assets/Models/tree.obj',
                                 'OloEditor/SandboxProject/Assets/Scenes/Test.olo',
                                 'OloEditor/assets/models/Fox/Fox.gltf'])
        self.assertEqual(unresolved, ['SandboxProject/Assets/Missing.png'])

    def test_full_hash_within_budget_else_referenced_files_and_says_so(self):
        references, unresolved = acceptance.manifest_references(self.manifest, self.root, self.assets)
        full = acceptance.asset_provenance(self.assets, self.root, references, unresolved, budget_seconds=60)
        self.assertEqual((full['mode'], full['fileCount']), ('full-tree', 3))
        ticks = iter(range(0, 1000, 100))
        partial = acceptance.asset_provenance(self.assets, self.root, references, unresolved, budget_seconds=60,
                                              clock=lambda: next(ticks))
        self.assertEqual(partial['mode'], 'manifest-referenced')
        self.assertIn('exceeded 60 s', partial['reason'])
        self.assertEqual(partial['fileCount'], 3)
        self.assertEqual([m['path'] for m in partial['declaredSha256Mismatches']],
                         ['OloEditor/assets/models/Fox/Fox.gltf'])

    def test_manifest_pins_read_only_renderer_settings(self):
        self.assertEqual(acceptance.manifest_pins(MANIFEST),
                         {'Path': 'Deferred', 'VirtualShadowMaps': 'true', 'MSAASampleCount': '1'})


if __name__ == '__main__':
    unittest.main()
