"""The object manifest must predict exactly the objects a restored cache can serve."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from check_sccache_health import assess
from sccache_object_manifest import build_manifest, compare, normalise, parse_deps
from test_sccache_health import cli_environment, statistics

BUILD = os.path.abspath('build')
LAUNCHER = 'sccache.exe cl.exe /nologo'


def tree(count=600, shared='// tracing macros\n', private_suffix=''):
    """`count` objects that all include one shared header (Instrumentor.h's position)."""
    files = {normalise(os.path.join(BUILD, '../src/Shared.h')): shared.encode()}
    compdb, deps = [], {}
    for index in range(count):
        source = os.path.abspath(f'src/Unit{index}.cpp')
        header = f'../src/Unit{index}.h'
        files[normalise(source)] = f'int f{index}();'.encode()
        files[normalise(os.path.join(BUILD, header))] = f'#pragma once{private_suffix}'.encode()
        output = f'obj/Unit{index}.cpp.obj'
        compdb.append({'directory': BUILD, 'file': source, 'output': output,
                       'command': f'{LAUNCHER} /c {source}'})
        deps[output] = [header, '../src/Shared.h']
    return compdb, deps, files


def manifest(compdb, deps, files):
    return build_manifest(compdb, deps, BUILD, files.get)


class ParseDepsTest(unittest.TestCase):
    def test_only_valid_records_are_kept_with_their_headers(self):
        text = ('a.obj: #deps 2, deps mtime 1 (VALID)\n    x.h\n    y.h\n\n'
                'b.obj: #deps 1, deps mtime 1 (STALE)\n    z.h\n\n'
                'c.obj: #deps 0, deps mtime 1 (VALID)\n')
        self.assertEqual(parse_deps(text), {'a.obj': ['x.h', 'y.h'], 'c.obj': []})


class BuildManifestTest(unittest.TestCase):
    def test_identical_trees_predict_every_object_reusable(self):
        first, second = manifest(*tree()), manifest(*tree())
        self.assertEqual(compare(first, second)['predicted_reusable'], 600)

    def test_a_header_every_object_includes_leaves_nothing_reusable_and_names_it(self):
        # #1678: Instrumentor.h gained the Tracy section macros and every TU missed.
        before, after = manifest(*tree()), manifest(*tree(shared='// tracing + sections\n'))
        prediction = compare(before, after)
        self.assertEqual(prediction['predicted_reusable'], 0)
        self.assertEqual(prediction['changed_files'], 1)
        self.assertTrue(prediction['changed_file_sample'][0].lower().endswith('src/shared.h'))

    def test_a_compile_flag_change_invalidates_without_any_file_changing(self):
        compdb, deps, files = tree()
        before = manifest(compdb, deps, files)
        for entry in compdb:
            entry['command'] += ' /DNEW_FLAG'
        prediction = compare(before, manifest(compdb, deps, files))
        self.assertEqual((prediction['predicted_reusable'], prediction['changed_files']), (0, 0))

    def test_objects_the_manifest_cannot_vouch_for_are_never_predicted(self):
        compdb, deps, files = tree(4)
        compdb[0]['command'] += ' /Yu"pch.h"'
        del deps[compdb[1]['output']]
        files.pop(normalise(os.path.join(BUILD, '../src/Unit2.h')))
        compdb.append({'directory': BUILD, 'file': 'a.asm', 'output': 'obj/a.asm.obj',
                       'command': 'nasm a.asm'})
        result = manifest(compdb, deps, files)
        self.assertEqual(sorted(result['objects']), ['obj/Unit3.cpp.obj'])
        self.assertEqual(len(result['unpredictable']), 3)
        self.assertNotIn('obj/a.asm.obj', result['unpredictable'])  # not a cache request

    def test_an_unknown_schema_is_refused_rather_than_compared(self):
        with self.assertRaises(ValueError):
            compare({'schema': 0, 'objects': {}}, manifest(*tree(1)))


class PredictedHealthTest(unittest.TestCase):
    def test_the_1678_shape_is_inputs_changed_not_near_zero_warm(self):
        # The measured run: 106 hits / 2126 on a correct cache after a shared header changed.
        prediction = compare(manifest(*tree()), manifest(*tree(shared='// changed\n')))
        data = statistics(hits=106, misses=2020)
        self.assertEqual(assess(data, 'restored', 'success')['disposition'], 'near-zero-warm')
        self.assertEqual(assess(data, 'restored', 'success', prediction)['disposition'], 'inputs-changed')

    def test_unchanged_inputs_with_no_hits_still_fail(self):
        # Negative control: the regression the gate exists for (#1584's eviction) must survive.
        prediction = compare(manifest(*tree()), manifest(*tree()))
        result = assess(statistics(hits=0, misses=2000), 'restored', 'success', prediction)
        self.assertEqual(result['disposition'], 'reuse-shortfall')

    def test_reuse_is_judged_against_the_predicted_objects(self):
        prediction = compare(manifest(*tree()), manifest(*tree()))
        for hits, disposition in ((299, 'reuse-shortfall'), (300, 'healthy-warm')):
            with self.subTest(hits=hits):
                result = assess(statistics(hits=hits, misses=2000 - hits), 'restored', 'success', prediction)
                self.assertEqual(result['disposition'], disposition)


class HealthCliManifestTest(unittest.TestCase):
    def run_cli(self, directory, stats, restored=None, current=None):
        source = directory / 'stats.json'
        source.write_text(json.dumps(stats), encoding='utf-8')
        arguments = [sys.executable, str(Path(__file__).with_name('check_sccache_health.py')),
                     '--stats', str(source), '--output', str(directory / 'health.json'),
                     '--restored-manifest', str(directory / 'restored.json'),
                     '--current-manifest', str(directory / 'current.json')]
        for name, value in (('restored.json', restored), ('current.json', current)):
            if value is not None:
                (directory / name).write_text(json.dumps(value), encoding='utf-8')
        result = subprocess.run(arguments, env=cli_environment('restored'), capture_output=True,
                                text=True, timeout=10)
        return result, json.loads((directory / 'health.json').read_text())

    def test_manifests_turn_a_wide_header_change_into_a_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, health = self.run_cli(Path(temporary), statistics(106, 2020),
                                          manifest(*tree()), manifest(*tree(shared='// changed\n')))
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(health['disposition'], 'inputs-changed')
            self.assertIn('::notice::', result.stdout)

    def test_a_snapshot_without_a_manifest_falls_back_to_the_raw_rate_and_says_so(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, health = self.run_cli(Path(temporary), statistics(0, 2000), current=manifest(*tree()))
            self.assertEqual(result.returncode, 1)
            self.assertIn('no object manifest', health['reason'])

    def test_a_restored_manifest_without_this_builds_own_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, health = self.run_cli(Path(temporary), statistics(), restored=manifest(*tree()))
            self.assertEqual(result.returncode, 1)
            self.assertEqual(health['disposition'], 'unavailable')


if __name__ == '__main__':
    unittest.main()
