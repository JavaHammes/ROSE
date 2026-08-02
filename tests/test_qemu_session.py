from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

from tests.qemu_smoke import QemuSession, temporary_root_image


class QemuSessionTests(unittest.TestCase):
    def test_temporary_root_image_is_isolated_and_removed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.ext2"
            source.write_bytes(b"pristine root image")

            with temporary_root_image(source) as working_copy:
                self.assertNotEqual(working_copy, source)
                self.assertEqual(working_copy.read_bytes(), source.read_bytes())
                working_copy.write_bytes(b"guest mutation")
                working_path = working_copy

            self.assertEqual(source.read_bytes(), b"pristine root image")
            self.assertFalse(working_path.exists())

    def test_read_until_preserves_already_buffered_output(self) -> None:
        session = QemuSession(
            [
                sys.executable,
                "-u",
                "-c",
                (
                    "import sys, time; "
                    "sys.stdout.buffer.write(b'first> buffered> '); "
                    "sys.stdout.buffer.flush(); time.sleep(1)"
                ),
            ]
        )
        try:
            self.assertEqual(session.read_until(b"> ", 1.0), "first> ")
            self.assertEqual(session.read_until(b"> ", 1.0), "buffered> ")
        finally:
            session.close()

    def test_read_until_handles_a_marker_split_across_reads(self) -> None:
        session = QemuSession(
            [
                sys.executable,
                "-u",
                "-c",
                (
                    "import sys, time; "
                    "sys.stdout.buffer.write(b'ro'); "
                    "sys.stdout.buffer.flush(); time.sleep(0.05); "
                    "sys.stdout.buffer.write(b'se> '); "
                    "sys.stdout.buffer.flush(); time.sleep(1)"
                ),
            ]
        )
        try:
            self.assertEqual(session.read_until(b"rose> ", 1.0), "rose> ")
        finally:
            session.close()


if __name__ == "__main__":
    unittest.main()
