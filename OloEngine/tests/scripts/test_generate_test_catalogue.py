"""The nightly reports body cost, retaining startup in the suite total (#1668)."""
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import generate_test_catalogue as catalogue


class DurationReportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.xml = Path(self.temp.name) / "gtest.xml"
        self.cfg = {"case_budget_s": {"default": 30, "L8": 120}}

    def report(self, cases, top=20):
        self.xml.write_text(f"<testsuites><testsuite>{cases}</testsuite></testsuites>", encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output), patch.object(catalogue, "suite_layers", return_value={
                "AssetSceneLoad": "integration", "Evidence": "L8"}):
            over = catalogue.report_durations(self.cfg, {}, self.xml, True, top)
        return over, output.getvalue()

    def test_init_is_subtracted_only_from_its_case_and_not_from_the_total(self):
        over, output = self.report('''
            <testcase classname="AssetSceneLoad" name="Load" time="70">
              <properties><property name="olo_process_renderer_init_s" value="50"/></properties>
            </testcase>
            <testcase classname="Other" name="Slow" time="31"/>
        ''')
        self.assertEqual(over, 1)
        self.assertIn("1.7 min including process init", output)
        self.assertIn("process renderer init: 50.000 s", output)
        self.assertRegex(output, r"20\.0 s\s+integration\s+budget\s+30 s\s+AssetSceneLoad.Load")
        self.assertIn("::warning title=Test over its case budget::Other.Slow", output)
        self.assertNotIn("case budget::AssetSceneLoad", output)

    def test_exact_override_takes_precedence_and_prints_its_reason(self):
        self.cfg["case_budget_overrides"] = {
            "Evidence.Gates": {"budget_s": 140, "reason": "Every camera on three raster paths."}}
        over, output = self.report('''
            <testcase classname="Evidence" name="Gates" time="130"/>
            <testcase classname="Evidence" name="Sibling" time="130"/>
            <testcase classname="Prefix/Evidence" name="Parameterized/0" time="120"/>
        ''')
        self.assertEqual(over, 1)
        self.assertRegex(output, r"budget\s+140 s\s+Evidence.Gates")
        self.assertIn("override: Every camera on three raster paths.", output)
        self.assertIn("case budget::Evidence.Sibling", output)
        self.assertNotIn("case budget::Prefix/Evidence", output)

    def test_missing_property_keeps_legacy_cost_and_top_does_not_hide_overages(self):
        over, output = self.report('''
            <testcase classname="Other" name="Fast" time="1"/>
            <testcase classname="Other" name="Slow" time="31"/>
        ''', top=0)
        self.assertEqual(over, 1)
        self.assertIn("process renderer init: 0.000 s", output)
        self.assertNotIn("Other.Fast", output)
        self.assertIn("Other.Slow", output)

    def test_invalid_init_cannot_hide_an_overage_and_rounding_never_goes_negative(self):
        for value in ("nan", "inf", "-1", "broken", "999"):
            with self.subTest(value=value):
                over, _ = self.report(f'''<testcase classname="Other" name="Slow" time="31">
                    <properties><property name="olo_process_renderer_init_s" value="{value}"/></properties>
                    </testcase>''')
                self.assertEqual(over, 1)
        over, output = self.report('''<testcase classname="Other" name="Init" time="0.001">
            <properties><property name="olo_process_renderer_init_s" value="0.0014"/></properties>
            </testcase>''')
        self.assertEqual(over, 0)
        self.assertNotIn("-0.0 s", output)

    def test_override_requires_a_finite_positive_budget_and_a_reason(self):
        for override in (30, {}, {"budget_s": 30}, {"budget_s": 30, "reason": " "},
                         *({"budget_s": v, "reason": "Full coverage"}
                           for v in (0, -1, float("inf"), float("nan"), True, "30"))):
            with self.subTest(override=override):
                self.cfg["case_budget_overrides"] = {"Other.Slow": override}
                with self.assertRaises(ValueError):
                    catalogue.validate_budget_overrides(self.cfg)

    def test_cli_over_budget_remains_a_warning_and_exits_successfully(self):
        self.xml.write_text('''<testsuites><testsuite>
            <testcase classname="AssetSceneLoad" name="Fixture" time="999"/>
            </testsuite></testsuites>''', encoding="utf-8")
        result = subprocess.run([sys.executable, str(Path(catalogue.__file__)),
                                 "--durations", str(self.xml), "--github"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::warning", result.stdout)


if __name__ == "__main__":
    unittest.main()
