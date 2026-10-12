"""Contention tracking must distinguish processes sharing one executable."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch


spec = importlib.util.spec_from_file_location(
    'foliage_cost_baseline', Path(__file__).with_name('foliage-cost-baseline.py'))
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


class ContentionTests(unittest.TestCase):
    def sample(self, cpu_seconds):
        output = (f'101|10.0|C:/repo/OloEngine-Tests.exe\n'
                  f'102|{cpu_seconds}|C:/repo/OloEngine-Tests.exe\n')
        with patch.object(baseline.subprocess, 'run', return_value=SimpleNamespace(stdout=output)), \
                patch.object(baseline.subprocess, 'check_output', return_value='3000, 100, 90'):
            return baseline.sample_gpu_users(101)

    def test_excludes_only_the_measured_pid(self):
        self.assertEqual([p['pid'] for p in self.sample(2.0)['foreign']], [102])

    def test_same_checkout_competitor_invalidates_its_interval(self):
        before, after = self.sample(2.0), self.sample(4.0)
        before['time'], after['time'] = '2026-10-12T12:00:00', '2026-10-12T12:00:30'
        self.assertEqual(baseline.contention_from_samples([before, after]), [
            {'start': before['time'], 'end': after['time'], 'what': 'C:/repo/OloEngine-Tests.exe'}])

    def test_timeout_kills_child_and_retains_pid_scoped_samples(self):
        sampled_child = threading.Event()
        child = MagicMock()
        child.pid = 101
        child.__enter__.return_value = child

        def sample(own_pid=None):
            if own_pid == child.pid:
                sampled_child.set()
            return {'time': '2026-10-12T12:00:00', 'foreign': [], 'gpu': None}

        def wait(timeout=None):
            if timeout is not None:
                self.assertTrue(sampled_child.wait(5), 'sampler never observed the child PID')
                raise subprocess.TimeoutExpired('test child', timeout)
            return -9

        child.wait.side_effect = wait
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(baseline.subprocess, 'Popen', return_value=child), \
                patch.object(baseline, 'probe_host', return_value={'busyProcesses': []}), \
                patch.object(baseline, 'sample_gpu_users', side_effect=sample) as sampler, \
                patch.object(baseline, 'SAMPLE_SECONDS', 0.001):
            output = Path(temporary) / 'run'
            with self.assertRaises(SystemExit):
                baseline.run_once(Path('test.exe'), Path(temporary), output, {}, 0.1)
            self.assertTrue(json.loads((output / 'host.json').read_text())['timedOut'])
            self.assertIn(unittest.mock.call(101), sampler.call_args_list)
        child.kill.assert_called_once()
        self.assertEqual(child.wait.call_count, 2)


if __name__ == '__main__':
    unittest.main()
