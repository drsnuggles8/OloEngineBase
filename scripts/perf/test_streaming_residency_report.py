"""Validity checks for descriptive streaming observations, not synthetic engine evidence."""
import importlib.util
import json
import math
import tempfile
import unittest
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    'streaming_report', Path(__file__).with_name('streaming-residency-report.py'))
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class StreamingReportTest(unittest.TestCase):
    def test_percentiles_are_empirical_and_short_tail_is_labelled(self):
        values = report.describe(list(range(100)))
        self.assertEqual((values['p50'], values['p95'], values['p99']), (49, 94, 98))
        self.assertIn('fewer than 1000', values['p99Evidence'])

    def test_missing_measurement_is_unavailable_not_zero(self):
        self.assertEqual(report.describe([])['status'], 'unavailable')
        self.assertEqual(report.telemetry(Path('.'), {'measurement': {}}, ['near'])['status'],
                         'unavailable')

    def test_non_finite_values_do_not_enter_tails(self):
        for values in ([math.nan], [math.inf], [-1]):
            with self.assertRaises(ValueError):
                report.describe(values)

    def telemetry(self, rows, filename='streaming.jsonl', host='editor-mcp'):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'streaming.jsonl').write_text(
                ''.join(json.dumps(row) + '\n' for row in rows), encoding='utf-8')
            return report.telemetry(directory, {'measurement': {'streamingRawFile': filename},
                                                'provenance': {'host': host}},
                                    ['near'])

    def test_cumulative_evictions_are_not_summed_and_missing_family_stays_missing(self):
        observed = self.telemetry([
            {'camera': 'near', 'cpuFrameId': 1, 'groom': {'evictions': 3}, 'uploadMs': 0},
            {'camera': 'near', 'cpuFrameId': 2, 'groom': {'evictions': 3}, 'uploadMs': 2}])
        near = observed['cameras']['near']
        self.assertEqual(near['groom']['evictions']['last'], 3)
        self.assertEqual(near['groom']['evictions']['max'], 3)
        self.assertEqual(near['vegetation']['residentCpuBytes']['status'], 'unavailable')
        self.assertEqual(near['uploadMs']['samples'], 2)

    def test_duplicate_frame_or_invalid_counter_is_rejected(self):
        row = {'camera': 'near', 'cpuFrameId': 1, 'groom': {'evictions': 0}}
        with self.assertRaises(ValueError):
            self.telemetry([row, row])
        with self.assertRaises(ValueError):
            self.telemetry([{**row, 'groom': {'evictions': -1}}])

    def test_telemetry_cannot_read_outside_capture(self):
        with self.assertRaises(ValueError):
            self.telemetry([], '../outside.jsonl')

    def test_test_host_uses_sample_indices_without_inventing_editor_frame_ids(self):
        row = {'camera': 'near', 'cpuFrameId': 0, 'index': 0, 'host': 'test-binary',
               'readBytes': 4096, 'preparationMicroseconds': 100}
        observed = self.telemetry([row, {**row, 'index': 1}], host='test-binary')
        self.assertEqual(observed['cameras']['near']['observedFrames'], 2)
        self.assertEqual(observed['cameras']['near']['readBytes']['last'], 4096)
        with self.assertRaises(ValueError):
            self.telemetry([row], host='editor-mcp')

    def test_empty_telemetry_is_not_an_observed_measurement(self):
        self.assertEqual(self.telemetry([])['status'], 'unavailable')

    def test_emitted_family_schema_preserves_cpu_memory_and_pending_cost(self):
        row = {'camera': 'near', 'cpuFrameId': 0, 'index': 0, 'host': 'test-binary',
               'groom': {'residentCpuBytes': 600, 'pinnedGpuBytes': 120,
                         'optionalGpuBytes': 300, 'pendingRequests': 1,
                         'fallbackDraws': 2, 'evictions': 3},
               'vegetation': {'canonicalCpuBytes': 200, 'pinnedGpuBytes': 80,
                              'optionalGpuBytes': 100, 'pendingCpuBytes': 40,
                              'pendingRequests': 1, 'fallbackDraws': 2,
                              'evictions': 3, 'reloads': 4}, 'regions': None}
        near = self.telemetry([row], host='test-binary')['cameras']['near']
        for family in ('groom', 'vegetation'):
            for name, value in row[family].items():
                self.assertEqual(near[family][name]['status'], 'observed')
                self.assertEqual(near[family][name]['last'], value)
        self.assertEqual(near['vegetation']['residentCpuBytes']['status'], 'unavailable')

    def test_region_pending_load_and_eviction_are_retained_without_summing(self):
        rows = [
            {'camera': 'near', 'cpuFrameId': 1, 'regions': None},
            {'camera': 'near', 'cpuFrameId': 2,
             'regions': {'loaded': 0, 'pending': 1, 'evictions': 0}},
            {'camera': 'near', 'cpuFrameId': 3,
             'regions': {'loaded': 1, 'pending': 0, 'evictions': 0}},
            {'camera': 'near', 'cpuFrameId': 4,
             'regions': {'loaded': 1, 'pending': 0, 'evictions': 1}}]
        regions = self.telemetry(rows)['cameras']['near']['regions']
        self.assertEqual(regions['activeFrames'], 3)
        self.assertEqual(regions['inactiveOrUnavailableFrames'], 1)
        self.assertEqual(regions['pending']['max'], 1)
        self.assertEqual(regions['loaded']['min'], 0)
        self.assertEqual(regions['loaded']['last'], 1)
        self.assertEqual(regions['evictions']['last'], 1)
        self.assertEqual(regions['evictions']['samples'], 3)

    def test_inactive_region_state_is_unavailable_and_invalid_counts_are_rejected(self):
        row = {'camera': 'near', 'cpuFrameId': 1, 'regions': None}
        regions = self.telemetry([row])['cameras']['near']['regions']
        self.assertEqual(regions['activeFrames'], 0)
        self.assertEqual(regions['loaded']['status'], 'unavailable')
        with self.assertRaises(ValueError):
            self.telemetry([{**row, 'regions': {'loaded': -1}}])
        with self.assertRaises(ValueError):
            self.telemetry([{**row, 'regions': []}])

    def test_file_read_cpu_wall_cost_is_preserved_separately_from_preparation(self):
        row = {'camera': 'near', 'cpuFrameId': 1, 'preparationMicroseconds': 100,
               'readMicroseconds': 20, 'readBytes': 4096}
        near = self.telemetry([row, {**row, 'cpuFrameId': 2, 'readMicroseconds': 30}])['cameras']['near']
        self.assertEqual(near['preparationMicroseconds']['last'], 100)
        self.assertEqual(near['readMicroseconds']['min'], 20)
        self.assertEqual(near['readMicroseconds']['last'], 30)
        self.assertEqual(near['ioMs']['status'], 'unavailable')
        missing = self.telemetry([{'camera': 'near', 'cpuFrameId': 1}])['cameras']['near']
        self.assertEqual(missing['readMicroseconds']['status'], 'unavailable')


if __name__ == '__main__':
    unittest.main()
