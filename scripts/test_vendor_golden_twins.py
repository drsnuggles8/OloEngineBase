"""A neutral golden must not change while its vendor twin stays stale (#1516)."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from check_vendor_golden_twins import BASE_ENV, WAIVERS, collect, find_problems, main

VISUAL = "OloEditor/assets/tests/visual/"
GOLDEN = "OloEditor/assets/tests/golden/"
WIND = VISUAL + "FoliageWind_GL_Forward_MSAA1_Ground.png"
WIND_AMD = VISUAL + "amd/FoliageWind_GL_Forward_MSAA1_Ground.png"
NEW_WIND = VISUAL + "FoliageWind_GL_Forward_MSAA4_Ground.png"
NEW_WIND_AMD = VISUAL + "amd/FoliageWind_GL_Forward_MSAA4_Ground.png"
FXAA = GOLDEN + "fxaa_hard_edge.png"
FXAA_AMD = GOLDEN + "amd/fxaa_hard_edge.png"
WATER = VISUAL + "Water_Overview.png"
TRACKED = {WIND, WIND_AMD, FXAA, FXAA_AMD, WATER}


def check(changed, waivers="", tracked=TRACKED):
    blobs = {p: "blob-" + p for p in tracked}
    return find_problems(changed, tracked, blobs.get, waivers)


class FindProblemsTest(unittest.TestCase):
    def test_neutral_change_with_a_stale_twin_fails_in_both_roots(self):
        report = check({WIND: "M", FXAA: "M"})
        self.assertEqual(len(report), 2)
        self.assertIn(WIND_AMD, report[0] + report[1])
        self.assertIn(FXAA_AMD, report[0] + report[1])

    def test_neutral_change_with_its_twin_rebaked_passes(self):
        self.assertEqual(check({WIND: "M", WIND_AMD: "M"}), [])

    def test_a_golden_without_a_twin_and_a_twin_only_rebake_pass(self):
        self.assertEqual(check({WATER: "M"}), [])
        self.assertEqual(check({WIND_AMD: "M"}), [])

    def test_an_added_golden_in_a_family_with_twins_needs_its_own_twin(self):
        tracked = TRACKED | {NEW_WIND}
        report = check({NEW_WIND: "A"}, tracked=tracked)
        self.assertEqual(len(report), 1)
        self.assertIn(NEW_WIND_AMD, report[0])
        self.assertEqual(check({NEW_WIND: "A", NEW_WIND_AMD: "A"}, tracked=tracked | {NEW_WIND_AMD}), [])
        water2 = VISUAL + "Water_Closeup.png"
        self.assertEqual(check({water2: "A"}, tracked=TRACKED | {water2}), [])

    def test_a_deleted_twin_of_a_tracked_golden_fails_and_a_deleted_neutral_does_not(self):
        tracked = TRACKED - {WIND_AMD}
        report = check({WIND: "M", WIND_AMD: "D"}, tracked=tracked)
        self.assertEqual(len(report), 1)
        self.assertIn("deleted", report[0])
        self.assertEqual(check({WIND: "D", WIND_AMD: "D"}, tracked=TRACKED - {WIND, WIND_AMD}), [])

    def test_a_waiver_does_not_cover_a_deleted_twin(self):
        tracked = TRACKED - {WIND_AMD}
        waived = f"{WIND}  blob-{WIND}  Vulkan-only image\n"
        report = check({WIND: "M", WIND_AMD: "D"}, waived, tracked=tracked)
        self.assertEqual(len(report), 1, report)
        self.assertIn("deleted", report[0])

    def test_a_waiver_pinned_to_the_new_blob_passes(self):
        self.assertEqual(check({WIND: "M"}, f"# comment\n{WIND}  blob-{WIND}  Vulkan-only image\n"), [])

    def test_a_waiver_for_an_older_blob_is_stale_and_does_not_cover_the_change(self):
        report = check({WIND: "M"}, f"{WIND}  0123abcd  Vulkan-only image\n")
        self.assertTrue(any("was not" in line for line in report), report)
        self.assertTrue(any("has changed since it was waived" in line for line in report), report)

    def test_a_malformed_duplicate_or_dangling_waiver_fails(self):
        self.assertTrue(any("needs" in line for line in check({}, f"{WIND}  blob-{WIND}\n")))
        twice = f"{WIND}  blob-{WIND}  a\n{WIND}  blob-{WIND}  b\n"
        self.assertTrue(any("already waived on line 1" in line for line in check({}, twice)))
        gone = VISUAL + "Gone.png"
        self.assertTrue(any("not a tracked file" in line for line in check({}, f"{gone}  abc  reason\n")))


def git(root, *args):
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=True).stdout.strip()


class GitPlumbingTest(unittest.TestCase):
    """The same rules through real `git diff` / `ls-tree` / `hash-object`, committed and uncommitted."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        git(self.root, "init", "-q")
        git(self.root, "config", "user.email", "t@example.invalid")
        git(self.root, "config", "user.name", "t")
        for path in (WIND, WIND_AMD, WATER):
            self.write(path, b"old " + path.encode())
        git(self.root, "add", ".")
        git(self.root, "commit", "-q", "-m", "base")
        self.base = git(self.root, "rev-parse", "HEAD")

    def tearDown(self):
        self.temp.cleanup()

    def write(self, path, data):
        (self.root / path).parent.mkdir(parents=True, exist_ok=True)
        (self.root / path).write_bytes(data)

    def problems(self, head=None):
        return find_problems(*collect(self.root, self.base, head))

    def run_main(self, *args, env_base=None):
        env = {k: v for k, v in os.environ.items() if k != BASE_ENV}
        if env_base:
            env[BASE_ENV] = env_base
        out = io.StringIO()
        cwd = os.getcwd()
        try:
            os.chdir(self.root)
            with patch.dict(os.environ, env, clear=True), contextlib.redirect_stdout(out):
                return main(list(args)), out.getvalue()
        finally:
            os.chdir(cwd)

    def test_committed_and_uncommitted_stale_twin_fails_and_rebaked_twin_passes(self):
        self.write(WIND, b"new")
        self.assertEqual(len(self.problems()), 1)
        git(self.root, "commit", "-q", "-am", "neutral only")
        self.assertEqual(len(self.problems(head="HEAD")), 1)
        self.write(WIND_AMD, b"new amd")
        git(self.root, "commit", "-q", "-am", "twin")
        self.assertEqual(self.problems(head="HEAD"), [])
        self.assertEqual(self.problems(), [])

    def test_waiver_uses_the_blob_id_git_reports_and_head_mode_reads_the_heads_waivers(self):
        self.write(WIND, b"new")
        blob = git(self.root, "hash-object", WIND)
        self.write(WAIVERS, f"{WIND}  {blob}  Vulkan-only image\n".encode())
        self.assertEqual(self.problems(), [])
        git(self.root, "add", ".")
        git(self.root, "commit", "-q", "-m", "waived")
        self.assertEqual(self.problems(head="HEAD"), [])
        # The working tree's waivers file must not leak into a --head check of an older commit.
        self.assertEqual(len(self.problems(head=self.base)), 0)
        self.write(WAIVERS, b"")
        self.assertEqual(self.problems(head="HEAD"), [])

    def test_hook_mode_reports_locally_and_fails_only_with_the_ci_base(self):
        self.write(WIND, b"new")
        git(self.root, "commit", "-q", "-am", "neutral only")
        status, out = self.run_main("--hook", "--base", self.base)
        self.assertEqual(status, 0)
        self.assertIn("Reported, not failed", out)
        status, _ = self.run_main("--hook", env_base=self.base)
        self.assertEqual(status, 1)
        status, _ = self.run_main("--base", self.base)
        self.assertEqual(status, 1)

    def test_an_unknown_base_is_a_clear_error_not_a_traceback(self):
        status, out = self.run_main("--base", "0" * 40)
        self.assertEqual(status, 2)
        self.assertIn("Is the base commit fetched?", out)
        status, _ = self.run_main("--hook", "--base", "0" * 40)
        self.assertEqual(status, 0)


PR_1667_MERGE = "415ff9428"


def repo_has(commit):
    root = Path(__file__).resolve().parent.parent
    probe = subprocess.run(["git", "cat-file", "-e", commit + "^{commit}"], cwd=root, capture_output=True)
    return probe.returncode == 0


@unittest.skipUnless(repo_has(PR_1667_MERGE), "needs the history of #1667 (shallow clone)")
class NegativeControlTest(unittest.TestCase):
    def test_it_fires_on_the_merge_that_left_28_amd_foliage_goldens_stale(self):
        root = Path(__file__).resolve().parent.parent
        report = find_problems(*collect(root, PR_1667_MERGE + "^1", PR_1667_MERGE))
        self.assertEqual(len(report), 28, report)
        self.assertTrue(all("/amd/Foliage" in line for line in report), report)


if __name__ == "__main__":
    unittest.main()
