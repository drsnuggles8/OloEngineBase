"""A restored compiler cache must demonstrate reuse after a complete hosted build."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from check_sccache_health import assess


def statistics(hits=1998, misses=2):
    return {'stats': {'compile_requests': hits + misses, 'requests_executed': hits + misses,
                      'cache_hits': {'counts': {'C/C++': hits}, 'adv_counts': {}},
                      'cache_misses': {'counts': {'C/C++': misses}, 'adv_counts': {}},
                      'cache_read_errors': 0, 'cache_write_errors': 0, 'compile_fails': 0},
            'cache_size': 2146000000, 'max_cache_size': 2147483648}


class SccacheHealthTest(unittest.TestCase):
    def test_healthy_prefix_restore_is_warm_even_without_an_exact_key_hit(self):
        result = assess(statistics(), 'sccache-windows-release-previous-run', 'success')
        self.assertEqual(result['disposition'], 'healthy-warm')
        self.assertAlmostEqual(result['hit_rate_percent'], 99.9)

    def test_zero_and_near_zero_warm_reuse_fail(self):
        for hits in (0, 1, 99):
            with self.subTest(hits=hits):
                self.assertEqual(assess(statistics(hits, 2000 - hits), 'restored', 'success')
                                 ['disposition'], 'near-zero-warm')

    def test_legitimate_cold_build_does_not_need_hits(self):
        self.assertEqual(assess(statistics(0, 2000), '', 'success')['disposition'], 'cold')

    def test_partial_failed_skipped_and_empty_builds_cannot_claim_health(self):
        for outcome in ('failure', 'cancelled', 'skipped', ''):
            self.assertEqual(assess(statistics(), 'restored', outcome)['disposition'], 'incomplete')
        for hits, misses in ((0, 0), (8, 2)):
            self.assertEqual(assess(statistics(hits, misses), 'restored', 'success')
                             ['disposition'], 'insufficient-sample')

    def test_malformed_or_missing_counters_are_unavailable(self):
        cases = [None, {}, {'stats': {}}, statistics()]
        cases[-1]['stats']['cache_hits']['counts']['C/C++'] = -1
        for value in (True, '2000', 2.5):
            bad = statistics()
            bad['stats']['requests_executed'] = value
            cases.append(bad)
        for case in cases:
            with self.subTest(case=case):
                self.assertEqual(assess(case, 'restored', 'success')['disposition'], 'unavailable')

    def test_inconsistent_totals_or_cache_errors_fail(self):
        for counter in ('requests_executed', 'cache_read_errors', 'cache_write_errors', 'compile_fails'):
            bad = copy.deepcopy(statistics())
            bad['stats'][counter] += 1
            self.assertEqual(assess(bad, 'restored', 'success')['disposition'], 'unavailable')

    def test_language_totals_do_not_double_count_advanced_breakdowns(self):
        data = statistics()
        data['stats']['cache_hits']['adv_counts'] = {'C/C++': 1998}
        self.assertEqual(assess(data, 'restored', 'success')['hits'], 1998)

    def test_cli_records_failure_for_missing_empty_and_invalid_statistics(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / 'stats.json'
            destination = directory / 'health.json'
            for content in (None, '', '{broken'):
                if content is not None:
                    source.write_text(content, encoding='utf-8')
                result = subprocess.run([sys.executable, str(Path(__file__).with_name('check_sccache_health.py')),
                                         '--stats', str(source), '--output', str(destination)],
                                        env={**os.environ, 'OLO_CACHE_RESTORED_KEY': 'restored',
                                             'OLO_CACHE_BUILD_OUTCOME': 'success'},
                                        capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('::error::', result.stdout)
                self.assertEqual(json.loads(destination.read_text())['disposition'], 'unavailable')

    def test_cli_emits_machine_report_summary_and_warm_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / 'stats.json'
            destination = directory / 'health.json'
            summary = directory / 'summary.md'
            source.write_text(json.dumps(statistics(0, 2000)), encoding='utf-8')
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('check_sccache_health.py')),
                                     '--stats', str(source), '--output', str(destination)],
                                    env={**os.environ, 'OLO_CACHE_RESTORED_KEY': 'restored',
                                         'OLO_CACHE_BUILD_OUTCOME': 'success', 'GITHUB_STEP_SUMMARY': str(summary)},
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(destination.read_text())['misses'], 2000)
            self.assertIn('near-zero-warm', summary.read_text())

    def test_cli_allows_complete_cold_and_healthy_warm_builds(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for key, data in (('', statistics(0, 2000)), ('restored', statistics())):
                source = directory / 'stats.json'
                source.write_text(json.dumps(data), encoding='utf-8')
                result = subprocess.run([sys.executable, str(Path(__file__).with_name('check_sccache_health.py')),
                                         '--stats', str(source), '--output', str(directory / 'health.json')],
                                        env={**os.environ, 'OLO_CACHE_RESTORED_KEY': key,
                                             'OLO_CACHE_BUILD_OUTCOME': 'success'},
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('::notice::', result.stdout)


if __name__ == '__main__':
    unittest.main()
