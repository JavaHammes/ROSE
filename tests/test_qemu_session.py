from __future__ import annotations

import sys
import unittest

from tests.qemu_smoke import QemuSession


class QemuSessionTests(unittest.TestCase):
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
