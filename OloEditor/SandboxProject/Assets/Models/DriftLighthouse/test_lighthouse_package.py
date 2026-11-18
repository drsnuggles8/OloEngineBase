"""Failure-path checks for the offline publisher. No Blender or GPU required."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from lighthouse_package import publish_package, staging_parent


class PublicationTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        root = Path(scratch.name)
        self.staged, self.live = root / "stage", root / "live"
        self.staged.mkdir()
        self.live.mkdir()
        self.names = ["new.png", "mesh.bin", "scene.gltf"]
        for name in self.names:
            (self.staged / name).write_bytes(b"new")
        for name in self.names[1:]:
            (self.live / name).write_bytes(b"old")
        self.recovery = self.live / ".stacks-light-recovery"

    def test_interrupt_after_rename_restores_old_files_and_removes_new_file(self):
        replace = os.replace

        def interrupt(src, dst):
            replace(src, dst)
            if src.name == "mesh.bin.pending":
                raise KeyboardInterrupt()

        with (
            patch("lighthouse_package.os.replace", side_effect=interrupt),
            self.assertRaises(KeyboardInterrupt),
        ):
            publish_package(self.staged, self.live, self.names)
        self.assertFalse((self.live / "new.png").exists())
        self.assertEqual((self.live / "mesh.bin").read_bytes(), b"old")
        self.assertEqual((self.live / "scene.gltf").read_bytes(), b"old")
        self.assertFalse(self.recovery.exists())

        publish_package(self.staged, self.live, self.names)
        self.assertTrue(
            all((self.live / name).read_bytes() == b"new" for name in self.names)
        )

    def test_failed_restore_preserves_originals_and_attempts_remaining_files(self):
        replace = os.replace

        def refuse(src, dst):
            if src.name in ("scene.gltf.pending", "mesh.bin.restore"):
                raise PermissionError(src.name)
            replace(src, dst)

        with (
            patch("lighthouse_package.os.replace", side_effect=refuse),
            self.assertRaisesRegex(
                RuntimeError, "scene.gltf.pending.*mesh.bin.restore"
            ),
        ):
            publish_package(self.staged, self.live, self.names)
        self.assertFalse((self.live / "new.png").exists())
        self.assertTrue((self.recovery / "transaction.plan").is_file())
        for name in self.names[1:]:
            self.assertEqual(
                (self.recovery / (name + ".previous")).read_bytes(), b"old"
            )
        with self.assertRaisesRegex(
            RuntimeError, "Publication already active or interrupted"
        ):
            publish_package(self.staged, self.live, self.names)

    def test_custom_temp_directory_cannot_land_in_the_watched_project(self):
        (self.live / "Example.oloproj").touch()
        scratch = self.live / "custom-temp"
        scratch.mkdir()
        with patch("lighthouse_package.tempfile.gettempdir", return_value=str(scratch)):
            self.assertFalse(staging_parent().is_relative_to(self.live))


if __name__ == "__main__":
    unittest.main()
