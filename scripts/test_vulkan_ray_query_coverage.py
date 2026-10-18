"""A designated ray-query run fails when a required case did not execute (#1294)."""
import io
import os
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from vulkan_ray_query_coverage import main, parse_test_list

SUITES = ['RayTracingDevice', 'ReSTIRPTDevice']
OPT_IN = 'RayTracingDevice.VegetationDetailedVersusCardExperiment'

# What OloEngine-Tests.exe --gtest_list_tests prints, engine log lines included.
LISTING = """[11:35:45] OloEngine: [Levers] active: OLO_RENDERGRAPH_DIAGNOSTICS=on (set at runtime)
RayTracingDevice.
  AStaticTriangleBuildsCompactsAndStaysTraceable
  VegetationDetailedVersusCardExperiment
ReSTIRPTDevice.
  EveryMappingRunsTemporalSpatialAndResolveOnRealGeometry
  DiffuseAndNearSpecularEnvironmentAgreeWithTheWholeReferenceIntegral
"""

PASSED = '<testcase name="{name}" status="run" result="completed" classname="{suite}"/>'
SKIPPED = ('<testcase name="{name}" status="run" result="skipped" classname="{suite}">'
           '<skipped message="RayTracingDeviceTest.cpp:247&#x0A;This device has no VK_KHR_ray_query"/></testcase>')


def report(**overrides):
    """A gtest XML where every listed case passed, the opt-in skipped, and `overrides` replace cases."""
    cases = {
        'RayTracingDevice.AStaticTriangleBuildsCompactsAndStaysTraceable': PASSED,
        OPT_IN: SKIPPED,
        'ReSTIRPTDevice.EveryMappingRunsTemporalSpatialAndResolveOnRealGeometry': PASSED,
        'ReSTIRPTDevice.DiffuseAndNearSpecularEnvironmentAgreeWithTheWholeReferenceIntegral': PASSED,
    }
    cases.update(overrides)
    rows = [template.format(suite=full.split('.')[0], name=full.split('.')[1])
            for full, template in cases.items() if template is not None]
    return '<?xml version="1.0"?><testsuites>' + ''.join(rows) + '</testsuites>'


class GateTest(unittest.TestCase):
    def run_gate(self, xml, require=True, listing=LISTING):
        with tempfile.TemporaryDirectory() as directory:
            xml_path = Path(directory) / 'report.xml'
            list_path = Path(directory) / 'list.txt'
            xml_path.write_text(xml, encoding='utf-8')
            list_path.write_text(listing, encoding='utf-8')
            argv = ['--report', str(xml_path), '--expected-list', str(list_path), '--allow-skip', OPT_IN]
            for suite in SUITES:
                argv += ['--suite', suite]
            if require:
                argv.append('--require')
            environment = {k: v for k, v in os.environ.items() if k != 'GITHUB_STEP_SUMMARY'}
            output = io.StringIO()
            with patch.dict(os.environ, environment, clear=True), redirect_stdout(output):
                return main(argv), output.getvalue()

    def test_every_case_executed_passes(self):
        status, output = self.run_gate(report())
        self.assertEqual(status, 0, output)
        self.assertIn('Every required case executed.', output)

    def test_a_skip_outside_the_opt_in_fails_with_its_reason(self):
        skipped = {'ReSTIRPTDevice.EveryMappingRunsTemporalSpatialAndResolveOnRealGeometry': SKIPPED}
        status, output = self.run_gate(report(**skipped))
        self.assertEqual(status, 1)
        self.assertIn('RayTracingDeviceTest.cpp:247 This device has no VK_KHR_ray_query', output)

    def test_a_case_the_filter_dropped_fails_as_did_not_run(self):
        dropped = {'ReSTIRPTDevice.DiffuseAndNearSpecularEnvironmentAgreeWithTheWholeReferenceIntegral': None}
        status, output = self.run_gate(report(**dropped))
        self.assertEqual(status, 1)
        self.assertIn('DiffuseAndNearSpecularEnvironmentAgreeWithTheWholeReferenceIntegral did not run', output)

    def test_a_suite_with_no_listed_case_fails(self):
        # A renamed suite: the gate's --suite no longer matches anything the binary defines or runs.
        renamed = {'ReSTIRPTDevice.EveryMappingRunsTemporalSpatialAndResolveOnRealGeometry': None,
                   'ReSTIRPTDevice.DiffuseAndNearSpecularEnvironmentAgreeWithTheWholeReferenceIntegral': None}
        status, output = self.run_gate(report(**renamed), listing=LISTING.split('ReSTIRPTDevice.')[0])
        self.assertEqual(status, 1)
        self.assertIn('ReSTIRPTDevice defines no case in this run', output)

    def test_required_mode_refuses_to_run_without_the_defined_set(self):
        with redirect_stdout(io.StringIO()), patch('sys.stderr', io.StringIO()):
            with self.assertRaises(SystemExit) as refused:
                main(['--report', 'unused.xml', '--suite', 'ReSTIRPTDevice', '--require'])
        self.assertNotEqual(refused.exception.code, 0)

    def test_report_mode_tolerates_skips(self):
        skipped = {'ReSTIRPTDevice.EveryMappingRunsTemporalSpatialAndResolveOnRealGeometry': SKIPPED}
        status, output = self.run_gate(report(**skipped), require=False)
        self.assertEqual(status, 0, output)

    def test_the_listing_ignores_engine_log_lines(self):
        cases = parse_test_list(LISTING, SUITES)
        self.assertEqual(len(cases['RayTracingDevice']), 2)
        self.assertEqual(len(cases['ReSTIRPTDevice']), 2)


if __name__ == '__main__':
    unittest.main()
