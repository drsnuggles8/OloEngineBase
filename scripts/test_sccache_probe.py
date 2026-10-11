import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from windows_cache_probe import report


class CacheProbeTest(unittest.TestCase):
    def test_coloured_log_counts_unique_current_objects_without_obsolete_bytes(self):
        first, second, obsolete = 'a' * 64, 'b' * 64, 'c' * 64
        log = '\n'.join([
            f'\x1b[0m [same.cpp.obj]: Hash key: {first}',
            '[same.cpp.obj]: Cache hit in 0.1s',
            f'\x1b[0m [same.cpp.obj]: Hash key: {second}',
            '[same.cpp.obj]: Cache miss in 0.1s',
            f'[same.cpp.obj]: Hash key: {first}',
            '[same.cpp.obj]: Cache hit in 0.1s',
        ])
        result = report({first: 10, obsolete: 100}, log,
                        {first: 10, second: 20, obsolete: 100})
        self.assertEqual(result['requests'], 3)
        self.assertEqual(result['requested_entries'], 2)
        self.assertEqual(result['requested_bytes'], 30)
        self.assertEqual(result['final_bytes'], 130)
        self.assertEqual(result['missing_requested_entries'], 0)
        self.assertEqual((result['hits'], result['misses']), (2, 1))
        self.assertEqual(result['resident_at_restore'], 2)
        self.assertEqual({r['object'] for r in result['records']}, {'same.cpp.obj'})

    def test_complete_log_does_not_claim_a_complete_object_set_after_eviction(self):
        first, second = 'a' * 64, 'b' * 64
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache = root / 'cache'
            entry = cache / 'a' / 'a' / first
            entry.parent.mkdir(parents=True)
            entry.write_bytes(b'object')
            before, log, stats, output = [root / name for name in
                                         ('before.json', 'daemon.log', 'stats.json', 'report.json')]
            before.write_text(json.dumps({first: 6}))
            log.write_text(f'[one.obj]: Hash key: {first}\n[one.obj]: Cache hit in 0.1s\n'
                           f'[two.obj]: Hash key: {second}\n[two.obj]: Cache miss in 0.1s\n')
            stats.write_text(json.dumps({'stats': {'requests_executed': 2}}))
            result = subprocess.run([
                sys.executable, str(Path(__file__).with_name('windows_cache_probe.py')),
                'report', '--cache', str(cache), '--before', str(before), '--log', str(log),
                '--stats', str(stats), '--output', str(output),
            ], capture_output=True, text=True, check=True)
            self.assertIn('"object_set_complete": false', result.stdout)
            data = json.loads(output.read_text())
            self.assertTrue(data['complete'])
            self.assertFalse(data['object_set_complete'])
            self.assertEqual(data['missing_requested_entries'], 1)
            self.assertEqual(data['requested_bytes'], 6)


if __name__ == '__main__':
    unittest.main()
