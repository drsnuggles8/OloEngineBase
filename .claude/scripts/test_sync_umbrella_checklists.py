"""Only the box of a done child's own checklist line is ticked; every other byte survives."""
import unittest

import sync_umbrella_checklists as sync
from sync_umbrella_checklists import RefState


class TickTest(unittest.TestCase):
    def test_ticks_only_items_whose_first_token_is_a_done_reference(self):
        body = ("- [ ] #1 — done child\n"
                "- [ ] #2 — open child\n"
                "- [ ] blocked by #1, a mention, not the item\n"
                "- [x] #1 — already ticked\n")
        new_body, ticked = sync.tick(body, {1})
        self.assertEqual(ticked, [1])
        self.assertEqual(new_body.splitlines()[0], "- [x] #1 — done child")
        self.assertEqual(new_body.splitlines()[1:], body.splitlines()[1:])

    def test_keeps_crlf_indentation_and_other_bullets(self):
        body = "intro\r\n  * [ ] #7 — nested\r\n+ [ ]  #8 two spaces\r\n"
        new_body, ticked = sync.tick(body, {7, 8})
        self.assertEqual(ticked, [7, 8])
        self.assertEqual(new_body, "intro\r\n  * [x] #7 — nested\r\n+ [x]  #8 two spaces\r\n")

    def test_number_prefix_does_not_match_a_longer_number(self):
        new_body, ticked = sync.tick("- [ ] #135 x\n- [ ] #1355 y\n", {135})
        self.assertEqual(ticked, [135])
        self.assertEqual(new_body, "- [x] #135 x\n- [ ] #1355 y\n")

    def test_score_block_and_prose_survive_unchanged(self):
        body = "<!-- olo-score\nfun_by: user\n-->\n- [ ] #3 a\nText with [ ] in it.\n"
        new_body, _ = sync.tick(body, {3})
        self.assertEqual(new_body, body.replace("- [ ] #3", "- [x] #3"))

    def test_fenced_example_is_not_an_item(self):
        body = ("```markdown\n"
                "- [ ] #7 example\n"
                "```\n"
                "- [ ] #7 example\n"
                "~~~~\n"
                "- [ ] #7 inside a tilde fence, ``` does not close it\n"
                "```\n"
                "~~~~\n")
        self.assertEqual(sync.unticked_refs(body), [7])
        new_body, ticked = sync.tick(body, {7})
        self.assertEqual(ticked, [7])
        self.assertEqual(new_body, body.replace("```\n- [ ] #7", "```\n- [x] #7", 1))

    def test_unclosed_fence_runs_to_the_end(self):
        body = "- [ ] #1 a\n```\n- [ ] #1 b\n"
        self.assertEqual(sync.tick(body, {1}), ("- [x] #1 a\n```\n- [ ] #1 b\n", [1]))

    def test_guard_rejects_any_other_change(self):
        with self.assertRaises(AssertionError):
            sync.assert_only_boxes_changed("- [ ] #1 a", "- [ ] #1 b")
        with self.assertRaises(AssertionError):
            sync.assert_only_boxes_changed("a b", "axb")


class RefStateTest(unittest.TestCase):
    def test_done_means_completed_issue_or_merged_pr(self):
        self.assertTrue(RefState("issue", "CLOSED", "COMPLETED").done)
        self.assertTrue(RefState("issue", "CLOSED", None).done)
        self.assertTrue(RefState("pr", "MERGED", None).done)

    def test_not_planned_duplicate_and_unmerged_pr_are_left_for_a_person(self):
        for state in (RefState("issue", "CLOSED", "NOT_PLANNED"),
                      RefState("issue", "CLOSED", "DUPLICATE"),
                      RefState("pr", "CLOSED", None)):
            self.assertFalse(state.done, state)
            self.assertTrue(state.closed_not_done, state)

    def test_open_items_are_neither(self):
        state = RefState("issue", "OPEN", None)
        self.assertFalse(state.done)
        self.assertFalse(state.closed_not_done)


if __name__ == "__main__":
    unittest.main()
