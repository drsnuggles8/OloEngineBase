import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent))
spec = importlib.util.spec_from_file_location("acceptance_live", Path(__file__).with_name("acceptance-live.py"))
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)


def snapshot(index, *, parked=False, ticking=True, updates=None, generation=7):
    return {"renderFrameNumber": index, "renderFrameGeneration": generation, "liveness": {"frameIndex": index if updates is None else updates, "iconified": parked, "ticking": ticking}, "fps": 3}


class CameraWindowTest(unittest.TestCase):
    def client(self, snapshots):
        class Client:
            def tool(self, name):
                if name != "olo_perf_snapshot":
                    raise AssertionError("Expected a completed-frame snapshot")
                return next(self.snapshots)
        client = Client()
        client.snapshots = iter(snapshots)
        return client

    def test_slow_camera_extends_window_instead_of_accepting_old_frames(self):
        # Reproduced live: 66 new frames cannot supply a 120-frame camera history.
        client = self.client([snapshot(166), snapshot(241)])
        with patch.object(acceptance.time, "sleep") as sleep:
            end = acceptance.finish_frame_window(client, 100, 120, 100, 7)
        self.assertEqual(end, 241)
        sleep.assert_called_once()
        self.assertGreater(sleep.call_args.args[0], 0)

    def test_throttled_update_ticks_do_not_count_as_rendered_frames(self):
        client = self.client([snapshot(160, updates=220), snapshot(220, updates=340)])
        with patch.object(acceptance.time, "sleep") as sleep:
            self.assertEqual(acceptance.finish_frame_window(client, 100, 120, 100, 7), 220)
        sleep.assert_called_once()

    def test_long_hitch_with_progress_is_not_a_parked_editor(self):
        client = self.client([snapshot(220, ticking=False)])
        self.assertEqual(acceptance.finish_frame_window(client, 100, 120, 1500, 7), 220)

    def test_completed_window_needs_no_extra_wait(self):
        with patch.object(acceptance.time, "sleep") as sleep:
            self.assertEqual(acceptance.finish_frame_window(self.client([snapshot(220)]), 100, 120, 100, 7), 220)
        sleep.assert_not_called()

    def test_no_render_progress_hits_bounded_timeout(self):
        client = self.client([snapshot(100, ticking=False)])
        with patch.object(acceptance.time, "monotonic", side_effect=[0, 301]):
            with self.assertRaisesRegex(RuntimeError, "did not complete"):
                acceptance.finish_frame_window(client, 100, 120, 100, 7)

    def test_parked_editor_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "parked"):
            acceptance.finish_frame_window(self.client([snapshot(220, parked=True)]), 100, 120, 100, 7)

    def test_reset_with_monotonic_renderer_counter_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "history reset"):
            acceptance.finish_frame_window(self.client([snapshot(222, generation=8)]), 100, 120, 100, 7)

    def test_history_reset_after_end_probe_is_rejected(self):
        class Client:
            def tool(self, name, **args):
                return {"renderFrameGeneration": 8, "series": [{"frameTimeMs": 100}] * 120}
        with self.assertRaisesRegex(RuntimeError, "history reset"):
            acceptance.camera_history(Client(), 120, 7)

    def test_unwritten_history_is_rejected(self):
        class Client:
            def tool(self, name, **args):
                return {"renderFrameGeneration": 7, "series": [{"frameTimeMs": 0}] * 119 + [{"frameTimeMs": 100}]}
        with self.assertRaisesRegex(RuntimeError, "cannot supply"):
            acceptance.camera_history(Client(), 120, 7)

    def test_reset_counter_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "reset"):
            acceptance.finish_frame_window(self.client([snapshot(1)]), 100, 120, 100, 7)


if __name__ == "__main__":
    unittest.main()
