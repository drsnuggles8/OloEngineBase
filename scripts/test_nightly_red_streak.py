"""A scheduled run judged by several jobs is red when any job is red (#1711)."""
import io
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

import nightly_red_streak as streak

REPO = "owner/repo"
WORKFLOW = "video-ffmpeg.yml"


def job(name, conclusion, minutes=30, failed_step=None):
    steps = [{"name": failed_step, "conclusion": "failure"}] if failed_step else []
    return {"name": name, "conclusion": conclusion, "steps": steps,
            "started_at": "2026-11-16T06:00:00Z",
            "completed_at": f"2026-11-16T{6 + minutes // 60:02d}:{minutes % 60:02d}:00Z"}


def run(run_id, conclusion="failure"):
    return {"id": run_id, "status": "completed", "conclusion": conclusion,
            "created_at": f"2026-11-{run_id:02d}T06:00:00Z",
            "html_url": f"https://github.com/{REPO}/actions/runs/{run_id}"}


class FakeGitHub:
    """Answers the three API reads the script makes; records every write."""

    def __init__(self, jobs_by_run, scheduled, open_issues=()):
        self.jobs_by_run = jobs_by_run
        self.scheduled = scheduled
        self.open_issues = list(open_issues)
        self.writes = []
        self.bodies = []

    def gh(self, *args, stdin=None):
        if args[0] == "api":
            path = args[1]
            if path.endswith("/jobs"):
                run_id = int(path.split("/")[-2])
                return streak.json.dumps({"jobs": self.jobs_by_run.get(run_id, [])})
            if "/workflows/" in path:
                return streak.json.dumps({"workflow_runs": self.scheduled})
            if "/issues?" in path:
                return streak.json.dumps(self.open_issues)
            raise AssertionError(f"unexpected API read {path}")
        self.writes.append(args)
        self.bodies.append(stdin)
        return ""


def main(fake, *extra):
    out = io.StringIO()
    with patch.object(streak, "gh", fake.gh), redirect_stdout(out):
        code = streak.main(["--repo", REPO, "--workflow", WORKFLOW, "--job", "windows", "--job", "linux",
                            "--timeout-minutes", "360", "--threshold", "1", "--run-id", "9", *extra])
    return code, out.getvalue()


class CombineTest(unittest.TestCase):
    def test_any_red_leg_makes_the_run_red(self):
        self.assertEqual(streak.combine(["green", "red"]), "red")
        self.assertEqual(streak.combine(["neutral", "red"]), "red")

    def test_green_needs_every_leg_green(self):
        self.assertEqual(streak.combine(["green", "green"]), "green")
        self.assertEqual(streak.combine(["green", "neutral"]), "neutral")
        self.assertEqual(streak.combine([]), "neutral")

    def test_a_missing_job_in_a_green_past_run_is_never_green(self):
        self.assertEqual(streak.job_verdict(run(1, "success"), None, 360), "neutral")
        self.assertEqual(streak.job_verdict(run(1, "failure"), None, 360), "red")


class MultiJobMainTest(unittest.TestCase):
    def test_red_linux_leg_beside_green_windows_opens_the_issue_on_the_first_red_week(self):
        tonight = [job("windows", "success", 240), job("linux", "failure", 20, "Configure (FFmpeg ON)")]
        fake = FakeGitHub({9: tonight, 2: [job("windows", "success"), job("linux", "success")]},
                          scheduled=[run(9), run(2, "success")])
        code, out = main(fake, "--result", "failure")
        self.assertEqual(code, 0)
        self.assertIn("1 consecutive red scheduled night(s)", out)
        self.assertEqual([w[:2] for w in fake.writes], [("label", "create"), ("issue", "create")])
        self.assertIn("linux: Configure (FFmpeg ON)", fake.bodies[1])
        self.assertNotIn("windows:", fake.bodies[1])

    def test_dry_run_counts_the_streak_across_both_legs_and_writes_nothing(self):
        # Week 2 red on linux only, week 3 red on windows only, week 1 green on both.
        fake = FakeGitHub({9: [job("windows", "failure", 200, "Run video tests"), job("linux", "success")],
                           3: [job("windows", "failure"), job("linux", "success")],
                           2: [job("windows", "success"), job("linux", "failure")],
                           1: [job("windows", "success"), job("linux", "success")]},
                          scheduled=[run(3), run(2), run(1, "success")])
        code, out = main(fake, "--result", "failure", "--dry-run")
        self.assertEqual(code, 0)
        self.assertIn("3 consecutive red scheduled night(s)", out)
        self.assertIn("[dry run] would open an issue", out)
        self.assertIn("windows: Run video tests", out)
        self.assertEqual(fake.writes, [])

    def test_a_leg_cancelled_by_its_timeout_is_red_although_the_other_is_green(self):
        tonight = [job("windows", "cancelled", 358), job("linux", "success")]
        fake = FakeGitHub({9: tonight}, scheduled=[run(9)])
        _, out = main(fake, "--result", "cancelled", "--dry-run")
        self.assertIn("would open an issue", out)

    def test_both_legs_green_closes_the_open_issue(self):
        fake = FakeGitHub({9: [job("windows", "success"), job("linux", "success")]}, scheduled=[],
                          open_issues=[{"number": 77, "body": f"... `{WORKFLOW}` ..."}])
        code, out = main(fake, "--result", "success")
        self.assertEqual(code, 0)
        self.assertEqual([w[:2] for w in fake.writes], [("issue", "comment"), ("issue", "close")])

    def test_a_job_name_that_no_longer_matches_fails_the_step(self):
        fake = FakeGitHub({9: [job("windows", "success"), job("linux (renamed)", "success")]}, scheduled=[])
        code, out = main(fake, "--result", "success")
        self.assertEqual(code, 1)
        self.assertIn("no job named 'linux'", out)


if __name__ == "__main__":
    unittest.main()
