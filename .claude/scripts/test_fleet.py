"""Offline tests for fleet.py and the worktree_status.py gate it builds on.

The gate decides what /cleanup-worktree deletes and the fleet decides what /orchestrate
launches, so their pure decisions are pinned here. Run:
    python -m unittest discover -s .claude/scripts -p test_fleet.py
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fleet  # noqa: E402
import worktree_status as ws  # noqa: E402

REPO = Path(__file__).resolve().parents[2]


def tree(**kw) -> ws.Worktree:
    base = {"path": "C:/repos/OloEngine-x", "head": "abc", "branch": "feature/x"}
    base.update({k: kw.pop(k) for k in list(kw) if k in base})
    t = ws.Worktree(**base)
    for k, v in kw.items():
        setattr(t, k, v)
    return t


class GateTest(unittest.TestCase):
    def test_scaffolded_never_started_worktree_is_standby_not_completed(self):
        # The bug this status exists for: on a master commit, only HANDOVER.md changed, no PR.
        t = tree(landed="ancestor", has_handover=True, ever_had_pr=False)
        self.assertEqual(ws.decide_status(t), "incomplete-standby")

    def test_pr_less_feature_branch_without_handover_is_standby_too(self):
        t = tree(landed="ancestor", has_handover=False, ever_had_pr=False)
        self.assertEqual(ws.decide_status(t), "incomplete-standby")

    def test_merged_task_is_completed(self):
        t = tree(landed="ancestor", has_handover=True, ever_had_pr=True)
        self.assertEqual(ws.decide_status(t), "completed")

    def test_dirty_beats_everything(self):
        t = tree(landed="ancestor", ever_had_pr=False, wip_files=[" M a.cpp"])
        self.assertEqual(ws.decide_status(t), "incomplete-dirty")

    def test_open_pr(self):
        t = tree(landed="", ever_had_pr=True, open_pr=12)
        self.assertEqual(ws.decide_status(t), "incomplete-open-pr")

    def test_other_branch_without_pr_is_judged_by_landing_alone(self):
        t = tree(branch="renovate/foo", kind="other", landed="ancestor", ever_had_pr=False)
        self.assertEqual(ws.decide_status(t), "completed")

    def test_pushed_but_no_pr_is_unmerged(self):
        t = tree(landed="", ever_had_pr=True)
        self.assertEqual(ws.decide_status(t), "incomplete-unmerged")


class HandoverParsingTest(unittest.TestCase):
    def test_model_and_effort(self):
        text = "## Recommended model + effort\n\n**Opus 5.5, effort high**: because.\n"
        self.assertEqual(fleet.parse_model_effort(text), ("claude-opus-5-5", "high"))
        self.assertEqual(fleet.parse_model_effort("**Fable 5.2, effort medium**"), ("claude-fable-5-2", "medium"))

    def test_missing_line_is_none_not_a_default(self):
        self.assertIsNone(fleet.parse_model_effort("Use a strong model."))

    def test_goal_has_one_source_in_start_work(self):
        text = (REPO / fleet.START_WORK_SKILL).read_text(encoding="utf-8")
        goal = fleet.extract_goal(text)
        self.assertIsNotNone(goal, "start-work SKILL.md must hold exactly one /goal line")
        self.assertIn("Phase 6 exit gate", goal)

    def test_two_goal_lines_are_ambiguous(self):
        self.assertIsNone(fleet.extract_goal("/goal a\n/goal b\n"))

    def test_launch_id(self):
        self.assertEqual(fleet.parse_launch_id("backgrounded · eca39275 · fleet-smoke\n"), "eca39275")


class ChecksAndGateTest(unittest.TestCase):
    ROLLUP = [
        {"__typename": "CheckRun", "name": "build", "status": "COMPLETED", "conclusion": "SUCCESS"},
        {"__typename": "CheckRun", "name": "tests", "status": "IN_PROGRESS", "conclusion": ""},
        {"__typename": "CheckRun", "name": "lint", "status": "COMPLETED", "conclusion": "FAILURE"},
        {"__typename": "CheckRun", "name": "CodeRabbit", "status": "COMPLETED", "conclusion": "NEUTRAL"},
        {"__typename": "StatusContext", "context": "sonar", "state": "SUCCESS"},
    ]

    def test_summarize(self):
        c = fleet.summarize_checks(self.ROLLUP)
        self.assertEqual(c["success"], ["build", "sonar"])
        self.assertEqual(c["pending"], ["tests"])
        self.assertEqual(c["failed"], ["lint"])
        self.assertEqual(c["other"], ["CodeRabbit=NEUTRAL"])

    def test_neutral_is_not_success(self):
        pr = {"mergeable": "MERGEABLE", "threads": 0,
              "checks": {"success": ["a"], "pending": [], "failed": [], "other": ["CodeRabbit=NEUTRAL"]}}
        met, missing = fleet.gate_status(pr)
        self.assertFalse(met)
        self.assertIn("CodeRabbit=NEUTRAL", missing[0])

    def test_unknown_thread_count_is_not_zero(self):
        pr = {"mergeable": "MERGEABLE", "threads": None,
              "checks": {"success": ["a"], "pending": [], "failed": [], "other": []}}
        self.assertEqual(fleet.gate_status(pr), (False, ["thread count unknown"]))

    def test_gate_met(self):
        pr = {"mergeable": "MERGEABLE", "threads": 0,
              "checks": {"success": ["a"], "pending": [], "failed": [], "other": []}}
        self.assertEqual(fleet.gate_status(pr), (True, []))


class HoldAndQuietTest(unittest.TestCase):
    def test_hold_clears_only_when_every_pr_merged(self):
        hold = {"after_prs": [1824, 1828]}
        self.assertFalse(fleet.hold_cleared(hold, {1828}))
        self.assertTrue(fleet.hold_cleared(hold, {1824, 1828}))

    def test_note_only_hold_never_clears_by_itself(self):
        self.assertFalse(fleet.hold_cleared({"after_prs": [], "note": "quiet GPU"}, {1, 2}))

    def test_quiet_counts_only_idle_and_untouched(self):
        prev = {"activity": "a", "quiet_ticks": 2, "session": {"status": "idle"}}
        self.assertEqual(fleet.next_quiet_ticks(prev, {"activity": "a", "session": {"status": "idle"}}), 3)
        self.assertEqual(fleet.next_quiet_ticks(prev, {"activity": "b", "session": {"status": "idle"}}), 0)
        self.assertEqual(fleet.next_quiet_ticks(prev, {"activity": "a", "session": {"status": "busy"}}), 0)


class DiffTest(unittest.TestCase):
    def snap(self, **kw):
        s = {"worktrees": {}, "prs": {}, "ready_to_launch": [], "builds_running": 0, "new_issues": []}
        s.update(kw)
        return s

    def pr(self, **kw):
        p = {"branch": "feature/x", "head": "aaa", "mergeable": "MERGEABLE", "threads": 0, "gate": False,
             "checks": {"success": [], "pending": [], "failed": [], "other": []}}
        p.update(kw)
        return p

    def test_no_change_is_no_events(self):
        s = self.snap(prs={"1": self.pr()})
        self.assertEqual(fleet.diff_events(s, s), [])

    def test_first_run_is_a_baseline(self):
        self.assertTrue(fleet.diff_events(None, self.snap())[0].startswith("baseline"))

    def test_push_failure_and_gate(self):
        prev = self.snap(prs={"1": self.pr()})
        failed = {"success": [], "pending": [], "failed": ["lint"], "other": []}
        cur = self.snap(prs={"1": self.pr(head="bbb", checks=failed, gate=True)})
        events = fleet.diff_events(prev, cur)
        self.assertTrue(any(e.startswith("pr-push #1") for e in events))
        self.assertIn("pr-check-failed #1: lint", events)
        self.assertIn("pr-gate #1: MET", events)

    def test_merge_and_hold_clear(self):
        prev = self.snap(prs={"1": self.pr()})
        cur = self.snap(closed_prs={"1": "MERGED"}, ready_to_launch=["l"])
        events = fleet.diff_events(prev, cur)
        self.assertIn("pr-left-open #1 (feature/x): now MERGED", events)
        self.assertIn("ready-to-launch l: its hold cleared", events)


if __name__ == "__main__":
    unittest.main()
