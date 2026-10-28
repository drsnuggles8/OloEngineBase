"""A neutral golden must not change while its vendor twin stays stale (#1516)."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from check_vendor_golden_twins import WAIVERS, collect, find_problems

VISUAL = "OloEditor/assets/tests/visual/"
GOLDEN = "OloEditor/assets/tests/golden/"
WIND = VISUAL + "FoliageWind_GL_Forward_MSAA1_Ground.png"
WIND_AMD = VISUAL + "amd/FoliageWind_GL_Forward_MSAA1_Ground.png"
FXAA = GOLDEN + "fxaa_hard_edge.png"
FXAA_AMD = GOLDEN + "amd/fxaa_hard_edge.png"
WATER = VISUAL + "Water_Overview.png"
TRACKED = {WIND, WIND_AMD, FXAA, FXAA_AMD, WATER}


def check(changed, waivers="", blobs=None):
    blobs = blobs or {p: "blob-" + p for p in TRACKED}
    return find_problems(changed, TRACKED, blobs.get, waivers)


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

    def test_added_and_deleted_neutral_goldens_are_not_a_twin_change(self):
        self.assertEqual(check({WIND: "A"}), [])
        self.assertEqual(check({WIND: "D"}), [])

    def test_a_waiver_pinned_to_the_new_blob_passes(self):
        self.assertEqual(check({WIND: "M"}, f"# comment\n{WIND}  blob-{WIND}  Vulkan-only image\n"), [])

    def test_a_waiver_for_an_older_blob_is_stale_and_does_not_cover_the_change(self):
        report = check({WIND: "M"}, f"{WIND}  0123abcd  Vulkan-only image\n")
        self.assertTrue(any("did not" in line for line in report), report)
        self.assertTrue(any("has changed since it was waived" in line for line in report), report)

    def test_a_waiver_without_a_reason_or_for_a_missing_file_fails(self):
        self.assertTrue(any("needs" in line for line in check({}, f"{WIND}  blob-{WIND}\n")))
        gone = VISUAL + "Gone.png"
        self.assertTrue(any("not a tracked file" in line for line in check({}, f"{gone}  abc  reason\n")))


def git(root, *args):
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=True).stdout.strip()


class GitPlumbingTest(unittest.TestCase):
    """The same rule through real `git diff` / `ls-tree` / `hash-object`, committed and uncommitted."""

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
        changed, tracked, blob_of = collect(self.root, self.base, head)
        waivers = self.root / WAIVERS
        text = waivers.read_text(encoding="utf-8") if waivers.is_file() else ""
        return find_problems(changed, tracked, blob_of, text)

    def test_committed_and_uncommitted_stale_twin_fails_and_rebaked_twin_passes(self):
        self.write(WIND, b"new")
        self.assertEqual(len(self.problems()), 1)
        git(self.root, "commit", "-q", "-am", "neutral only")
        self.assertEqual(len(self.problems(head="HEAD")), 1)
        self.write(WIND_AMD, b"new amd")
        git(self.root, "commit", "-q", "-am", "twin")
        self.assertEqual(self.problems(head="HEAD"), [])
        self.assertEqual(self.problems(), [])

    def test_waiver_uses_the_blob_id_git_reports(self):
        self.write(WIND, b"new")
        blob = git(self.root, "hash-object", WIND)
        self.write(WAIVERS, f"{WIND}  {blob}  Vulkan-only image\n".encode())
        self.assertEqual(self.problems(), [])
        git(self.root, "add", ".")
        git(self.root, "commit", "-q", "-m", "waived")
        self.assertEqual(self.problems(head="HEAD"), [])


PR_1667_MERGE = "415ff9428"


def repo_has(commit):
    root = Path(__file__).resolve().parent.parent
    probe = subprocess.run(["git", "cat-file", "-e", commit + "^{commit}"], cwd=root, capture_output=True)
    return probe.returncode == 0


@unittest.skipUnless(repo_has(PR_1667_MERGE), "needs the history of #1667 (shallow clone)")
class NegativeControlTest(unittest.TestCase):
    def test_it_fires_on_the_merge_that_left_28_amd_foliage_goldens_stale(self):
        root = Path(__file__).resolve().parent.parent
        changed, tracked, blob_of = collect(root, PR_1667_MERGE + "^1", PR_1667_MERGE)
        report = find_problems(changed, tracked, blob_of, "")
        self.assertEqual(len(report), 28, report)
        self.assertTrue(all("/amd/Foliage" in line for line in report), report)


if __name__ == "__main__":
    unittest.main()
