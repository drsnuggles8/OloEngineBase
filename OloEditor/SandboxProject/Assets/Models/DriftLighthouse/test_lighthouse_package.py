"""Failure-path checks for the offline publisher. No Blender or GPU required."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from lighthouse_package import create_staging_directory, publish_package


class PublicationTests(unittest.TestCase):
    def setUp(self):
        scratch = create_staging_directory()
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
        with (
            patch("lighthouse_package.tempfile.gettempdir", return_value=str(scratch)),
            create_staging_directory() as choice,
        ):
            self.assertFalse(Path(choice).resolve().is_relative_to(self.live.resolve()))

    def test_an_unwritable_project_sibling_falls_back_to_a_safe_temp_root(self):
        project = self.live / "project"
        custom = project / "tmp"
        custom.mkdir(parents=True)
        (project / "Example.oloproj").touch()
        alternate = self.staged
        real_temp = tempfile.TemporaryDirectory

        def allocate(*, dir, prefix):
            if dir == self.live.resolve():
                raise PermissionError("project sibling is read-only")
            return real_temp(dir=dir, prefix=prefix)

        with (
            patch("lighthouse_package.tempfile.gettempdir", return_value=str(custom)),
            patch.dict(os.environ, {"TMPDIR": str(custom), "TEMP": str(alternate)}),
            patch(
                "lighthouse_package.tempfile.TemporaryDirectory", side_effect=allocate
            ),
            create_staging_directory() as choice,
        ):
            self.assertEqual(Path(choice).resolve().parent, alternate.resolve())

    def test_non_listable_ancestor_is_not_mistaken_for_an_unwatched_root(self):
        custom = self.live / "tmp"
        custom.mkdir()
        real_scan = os.scandir

        def restricted(folder):
            if Path(folder) == self.live.resolve():
                raise PermissionError("cannot list project markers")
            return real_scan(folder)

        with (
            patch("lighthouse_package.tempfile.gettempdir", return_value=str(custom)),
            patch.dict(os.environ, {"TMPDIR": str(custom), "TEMP": str(self.staged)}),
            patch("lighthouse_package.os.scandir", side_effect=restricted),
        ):
            choice = create_staging_directory()
        with choice as selected:
            self.assertEqual(Path(selected).resolve().parent, self.staged.resolve())

    def test_unavailable_staging_reports_how_to_select_another_directory(self):
        with (
            patch(
                "lighthouse_package.tempfile.TemporaryDirectory",
                side_effect=PermissionError(),
            ),
            self.assertRaisesRegex(
                OSError, "Set TMPDIR to a writable directory outside"
            ),
        ):
            create_staging_directory()


if __name__ == "__main__":
    unittest.main()
