"""Invalid capture inputs must not turn into a passing timing gate."""
import csv
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from capture_process import editor_preferences, check_editor_preference_load

spec = importlib.util.spec_from_file_location('controlled_benchmark', Path(__file__).with_name('controlled-benchmark.py'))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CaptureValidityTest(unittest.TestCase):
    def test_editor_preference_parse_warning_rejects_apparent_valid_settings(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'OloEditor/OloEngine.log'
            path.parent.mkdir(parents=True)
            path.write_text('EditorPreferences: failed to parse one or more values, using defaults\n')
            with self.assertRaisesRegex(ValueError, 'rejected preferences'):
                check_editor_preference_load(temporary, 0)
            path.write_text('Editor ready\n')
            check_editor_preference_load(temporary, 0)
            with self.assertRaisesRegex(ValueError, 'stale'):
                check_editor_preference_load(temporary, path.stat().st_mtime + 1)

    @unittest.skipUnless(os.name == 'nt', 'Windows host-process probe')
    def test_running_engine_test_process_is_contention(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / 'OloEngine-Tests.exe'
            shutil.copyfile(Path(os.environ['SystemRoot']) / 'System32/cmd.exe', executable)
            process = subprocess.Popen([str(executable), '/d', '/c', 'pause'],
                                       stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
            try:
                state = capture.host_state()
                self.assertIn(process.pid, [p['Id'] for p in state['busyProcesses']])
            finally:
                process.terminate()
                process.wait(timeout=10)
                process.stdin.close()

    def test_editor_rejects_workload_throttling_and_missing_preferences(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'OloEditor/SandboxProject/EditorPreferences.yaml'
            path.parent.mkdir(parents=True)
            arm = {'repo': temporary}
            for throttle in ('true', 'false'):
                path.write_text('EditorPreferences:\n  ThrottleEditMode: ' + throttle +
                                '\n  ThrottlePlayMode: false\n  EnableAutoSave: false\n  FrameRateCap: 0\n')
                if throttle == 'true':
                    with self.assertRaisesRegex(ValueError, 'ThrottleEditMode'):
                        editor_preferences(arm)
                else:
                    self.assertFalse(editor_preferences(arm)['values']['ThrottleEditMode'])
            path.write_text('EditorPreferences: {}\n')
            with self.assertRaisesRegex(ValueError, 'ThrottleEditMode'):
                editor_preferences(arm)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.workload = {'cameras': ['small'], 'expectedFrames': 100}
        self.result = {
            'determinism': {'warmupTimedOut': False},
            'output': {'actual': {'renderWidth': 512, 'renderHeight': 512,
                                  'displayWidth': 512, 'displayHeight': 512}},
            'configuration': {'requested': {'upscaleMode': 0}},
            'measurement': {'deadlineMs': 16.67, 'rawFile': 'measurement.csv',
                            'sampleCount': 100, 'metric': 'test interval'},
            'manifest': {'sourceHashFnv1a64': 'test'},
            'provenance': {'backend': 'opengl', 'host': 'test-binary', 'gpuRenderer': 'test'},
        }
        self.rows = [{'camera': 'small', 'renderCallMs': 10, 'cpuMs': 8,
                      'fenceWaitMs': 1, 'presentWaitMs': 1, 'gpuMs': 5,
                      'gpuStatus': 'valid', 'gpuFrameId': i + 1} for i in range(100)]

    def read(self):
        (self.directory / 'result.json').write_text(json.dumps(self.result), encoding='utf-8')
        with (self.directory / 'measurement.csv').open('w', newline='', encoding='utf-8') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(self.rows[0]))
            writer.writeheader()
            writer.writerows(self.rows)
        return capture.read_capture(self.directory, self.workload)

    def test_complete_capture_and_valid_zero(self):
        self.rows[0]['gpuMs'] = 0
        data, _ = self.read()
        self.assertEqual(len(data['samples']['small']['gpuMs']), 100)
        self.assertEqual(data['gpuExcluded'], {})

    def test_invalid_and_duplicate_gpu_samples_have_reasons(self):
        self.rows[1]['gpuStatus'] = 'pending'
        self.rows[2]['gpuFrameId'] = 1
        data, _ = self.read()
        self.assertEqual(len(data['samples']['small']['gpuMs']), 98)
        self.assertEqual(data['gpuExcluded'], {'pending': 1, 'missing-or-duplicate-frame-id': 1})

    def test_truncated_capture_even_when_summary_also_truncated(self):
        self.rows.pop()
        self.result['measurement']['sampleCount'] = 99
        with self.assertRaisesRegex(ValueError, 'truncated'):
            self.read()

    def test_unknown_resolution_is_not_requested_resolution(self):
        self.result['output']['actual'] = None
        with self.assertRaisesRegex(ValueError, 'resolution'):
            self.read()

    def test_upscaler_requested_without_consumption_proof(self):
        self.result['configuration']['requested']['upscaleMode'] = 1
        with self.assertRaisesRegex(ValueError, 'consumption'):
            self.read()

    def test_nonfinite_cpu_value_and_warmup_timeout(self):
        self.rows[0]['cpuMs'] = 'nan'
        with self.assertRaises(ValueError):
            self.read()
        self.result['determinism']['warmupTimedOut'] = True
        with self.assertRaisesRegex(ValueError, 'timed out'):
            self.read()

    def test_p99_requires_enough_tail_samples(self):
        with self.assertRaisesRegex(ValueError, '1000'):
            capture.scalar([10] * 100, 'p99', 16.67)

    def test_whole_frame_channel_requires_editor_host(self):
        data, _ = self.read()
        self.assertNotIn('frameTimeMs', data['samples']['small'])
        self.result['provenance']['host'] = 'editor-mcp'
        with self.assertRaisesRegex(ValueError, 'continuous'):
            self.read()
        self.result['measurement'].update(continuousEditorFrames=True, completedSteps=100, traceOverflow=False)
        self.result['measurement']['scenarios'] = [{'camera': 'small', 'sampleCount': 100,
                                                   'firstCpuFrameId': 10, 'lastCpuFrameId': 109}]
        for i, row in enumerate(self.rows):
            row.update(cpuFrameId=10 + i, index=i)
        data, _ = self.read()
        self.assertEqual(data['samples']['small']['frameTimeMs'], [10] * 100)
        self.rows[50]['cpuFrameId'] += 1
        with self.assertRaisesRegex(ValueError, 'gap or duplicate'):
            self.read()

    def test_pass_distribution_validity_and_truncation(self):
        self.workload['gates'] = [{'channel': 'passGpuMs:ScenePass/Depth', 'statistic': q}
                                 for q in ('p50', 'p95', 'p99')]
        self.result['measurement']['passRawFile'] = 'passes.jsonl'
        rows = [{'camera': 'small', 'index': i, 'gpuFrameId': i + 1,
                 'passes': [{'pass': 'ScenePass/Depth', 'parent': 'ScenePass',
                             'isSubPass': True, 'status': 'valid', 'gpuMs': 0}]} for i in range(100)]
        rows[1]['gpuFrameId'] = 1
        rows[2]['passes'][0].update(status='dropped', gpuMs=None)
        path = self.directory / 'passes.jsonl'
        path.write_text('\n'.join(json.dumps(row) for row in rows), encoding='utf-8')
        data, _ = self.read()
        self.assertEqual(len(data['samples']['small']['passGpuMs:ScenePass/Depth']), 98)
        self.assertEqual(data['gpuExcluded']['passGpuMs:ScenePass/Depth:dropped'], 1)
        self.assertEqual(data['gpuExcluded']['passGpuMs:ScenePass/Depth:missing-or-duplicate-frame-id'], 1)
        path.write_text('\n'.join(json.dumps(row) for row in rows[:-1]), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'truncated per-pass'):
            self.read()

    def test_job_exit_status_preserves_regression_and_inconclusive(self):
        plan = self.directory / 'plan.json'
        plan.write_text('{}', encoding='utf-8')
        for status, code in [('pass', 0), ('regression', 1), ('inconclusive', 2)]:
            output = self.directory / status
            with patch('sys.argv', ['controlled-benchmark', '--plan', str(plan), '--output', str(output)]), \
                 patch.object(capture, 'execute', return_value={'status': status}), patch('builtins.print'):
                self.assertEqual(capture.main(), code)
            self.assertEqual(json.loads((output / 'report.json').read_text())['status'], status)

    def test_unconfigured_host_reports_limitation(self):
        with self.assertRaisesRegex(ValueError, 'runner identity'):
            capture.execute({'controlled': False}, self.directory)


if __name__ == '__main__':
    unittest.main()
