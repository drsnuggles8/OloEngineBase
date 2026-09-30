"""Windows launcher failures must not replace the direct process exit code."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == 'win32', 'Windows linker launcher')
class MeasureLinkTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='olo-measure-link-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.wrapper = Path(__file__).with_name('measure-link.ps1')

    def run_child(self, destination, code, *extra):
        return subprocess.run(
            ['pwsh', '-NoProfile', '-File', str(self.wrapper), sys.executable,
             '-c', f'import sys,time; time.sleep(.1); print("child-ran"); print("child-diagnostic", file=sys.stderr); raise SystemExit({code})', *extra],
            env={**os.environ, 'OLO_LINK_METRICS_DIR': str(destination)},
            capture_output=True, text=True, timeout=30)

    def test_response_read_failure_still_runs_child_and_records_missing_provenance(self):
        destination = self.root / 'metrics'
        result = self.run_child(destination, 0, '@' + str(self.root / 'missing.rsp'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('child-ran', result.stdout)
        self.assertIn('child-diagnostic', result.stderr)
        self.assertNotIn('VoidTaskResult', result.stdout)
        record = json.loads(next(destination.glob('*.json')).read_text(encoding='utf-8-sig'))
        self.assertEqual(record['exitCode'], 0)
        self.assertTrue(record['responseFiles'][0]['error'])

    def test_unwritable_metrics_preserve_success_and_failure_exit_codes(self):
        destination = self.root / 'a-file'
        destination.write_text('cannot create a directory here', encoding='utf-8')
        for code in (0, 17):
            with self.subTest(code=code):
                result = self.run_child(destination, code)
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertIn('child-ran', result.stdout)
                self.assertIn('failed to write metrics', result.stdout)

    def test_linker_start_failure_remains_a_failure(self):
        result = subprocess.run(
            ['pwsh', '-NoProfile', '-File', str(self.wrapper), str(self.root / 'missing.exe')],
            env={**os.environ, 'OLO_LINK_METRICS_DIR': str(self.root / 'metrics')},
            capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
