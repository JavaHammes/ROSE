from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from tools import mkrosefs


class Ext2Image:
    def __init__(self, contents: bytes) -> None:
        self.contents = contents

    def inode(self, number: int) -> tuple[int, int, int, tuple[int, ...]]:
        offset = (
            mkrosefs.INODE_TABLE_BLOCK * mkrosefs.BLOCK_SIZE
            + (number - 1) * mkrosefs.INODE_SIZE
        )
        mode = struct.unpack_from("<H", self.contents, offset)[0]
        size = struct.unpack_from("<I", self.contents, offset + 4)[0]
        sector_count = struct.unpack_from("<I", self.contents, offset + 28)[0]
        blocks = struct.unpack_from(
            f"<{mkrosefs.DIRECT_BLOCK_COUNT}I", self.contents, offset + 40
        )
        return mode, size, sector_count, blocks

    def children(self, inode_number: int) -> dict[str, int]:
        _, size, _, blocks = self.inode(inode_number)
        result: dict[str, int] = {}
        position = 0
        while position < size:
            offset = blocks[position // mkrosefs.BLOCK_SIZE] * mkrosefs.BLOCK_SIZE
            offset += position % mkrosefs.BLOCK_SIZE
            inode, record_length, name_length, _ = struct.unpack_from(
                "<IHBB", self.contents, offset
            )
            self.assert_directory_record(record_length, name_length)
            name = self.contents[offset + 8 : offset + 8 + name_length].decode(
                "ascii"
            )
            if inode != 0:
                result[name] = inode
            position += record_length
        return result

    @staticmethod
    def assert_directory_record(record_length: int, name_length: int) -> None:
        if record_length < 8 or record_length % 4 != 0:
            raise AssertionError(f"invalid directory record length: {record_length}")
        if name_length > record_length - 8:
            raise AssertionError(f"invalid directory name length: {name_length}")

    def resolve(self, path: str) -> int:
        inode = mkrosefs.ROOT_INODE
        for component in path.strip("/").split("/"):
            if component:
                inode = self.children(inode)[component]
        return inode

    def read_file(self, path: str) -> bytes:
        _, size, _, blocks = self.inode(self.resolve(path))
        output = bytearray()
        for block in blocks:
            if block == 0 or len(output) >= size:
                break
            start = block * mkrosefs.BLOCK_SIZE
            output.extend(self.contents[start : start + mkrosefs.BLOCK_SIZE])
        return bytes(output[:size])


class RootImageTests(unittest.TestCase):
    def test_build_is_deterministic_and_contains_expected_tree(self) -> None:
        files = {
            "/bin/hello": b"hello\n",
            "/etc/config": b"configured\n",
            "/empty": b"",
        }
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.ext2"
            second = Path(directory) / "second.ext2"
            mkrosefs.build_image(first, files)
            mkrosefs.build_image(second, dict(reversed(list(files.items()))))

            first_contents = first.read_bytes()
            self.assertEqual(first_contents, second.read_bytes())

        self.assertEqual(
            len(first_contents), mkrosefs.BLOCK_COUNT * mkrosefs.BLOCK_SIZE
        )
        self.assertEqual(
            struct.unpack_from("<H", first_contents, mkrosefs.BLOCK_SIZE + 56)[0],
            0xEF53,
        )
        self.assertEqual(
            struct.unpack_from("<I", first_contents, mkrosefs.BLOCK_SIZE + 12)[0],
            struct.unpack_from("<H", first_contents, 2 * mkrosefs.BLOCK_SIZE + 12)[
                0
            ],
        )
        self.assertEqual(
            struct.unpack_from("<I", first_contents, mkrosefs.BLOCK_SIZE + 16)[0],
            struct.unpack_from("<H", first_contents, 2 * mkrosefs.BLOCK_SIZE + 14)[
                0
            ],
        )

        image = Ext2Image(first_contents)
        self.assertIn("dev", image.children(mkrosefs.ROOT_INODE))
        self.assertEqual(image.read_file("/bin/hello"), b"hello\n")
        self.assertEqual(image.read_file("/etc/config"), b"configured\n")
        self.assertEqual(image.read_file("/empty"), b"")

    def test_rejects_paths_the_guest_cannot_resolve(self) -> None:
        invalid_paths = (
            "relative",
            "/",
            "/trailing/",
            "//double",
            "/./file",
            "/../file",
            "/nul\0file",
            "/dev/console",
            "/non-ascii-\N{SNOWMAN}",
            "/" + "a" * mkrosefs.DIRECTORY_NAME_MAX,
            "/" + "a" * (mkrosefs.GUEST_PATH_MAX - 1),
        )
        for path in invalid_paths:
            with self.subTest(path=path), self.assertRaises(ValueError):
                mkrosefs.build_nodes({path: b"data"})

    def test_rejects_a_file_used_as_a_parent_directory(self) -> None:
        with self.assertRaisesRegex(ValueError, "parent directory"):
            mkrosefs.build_nodes({"/bin": b"file", "/bin/program": b"program"})

    def test_rejects_duplicate_command_line_mappings(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            host = Path(directory) / "host"
            host.write_bytes(b"data")
            with self.assertRaisesRegex(ValueError, "duplicate guest path"):
                mkrosefs.parse_files(
                    [f"/bin/program={host}", f"/bin/program={host}"]
                )

    def test_failed_build_does_not_replace_an_existing_image(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "root.ext2"
            output.write_bytes(b"existing image")
            oversized = b"x" * (
                mkrosefs.DIRECT_BLOCK_COUNT * mkrosefs.BLOCK_SIZE + 1
            )
            with self.assertRaisesRegex(ValueError, "direct-block"):
                mkrosefs.build_image(output, {"/oversized": oversized})
            self.assertEqual(output.read_bytes(), b"existing image")


if __name__ == "__main__":
    unittest.main()
