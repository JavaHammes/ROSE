#!/usr/bin/env python3
"""Build the deterministic, writable ext2 root image used by ROSE."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path

BLOCK_SIZE = 1024
BLOCK_COUNT = 4096
INODE_COUNT = 128
INODE_SIZE = 128
FIRST_DATA_BLOCK = 1
BLOCK_BITMAP_BLOCK = 3
INODE_BITMAP_BLOCK = 4
INODE_TABLE_BLOCK = 5
INODE_TABLE_BLOCKS = INODE_COUNT * INODE_SIZE // BLOCK_SIZE
FIRST_CONTENT_BLOCK = INODE_TABLE_BLOCK + INODE_TABLE_BLOCKS
FIRST_NORMAL_INODE = 11
ROOT_INODE = 2
DIRECT_BLOCK_COUNT = 12


@dataclass
class Node:
    path: str
    directory: bool
    data: bytes = b""
    inode: int = 0
    blocks: list[int] = field(default_factory=list)

    @property
    def name(self) -> str:
        return self.path.rsplit("/", 1)[-1]

    @property
    def parent_path(self) -> str:
        parent = self.path.rsplit("/", 1)[0]
        return parent or "/"


def put_u16(buffer: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", buffer, offset, value)


def put_u32(buffer: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<I", buffer, offset, value)


def directory_entry(inode: int, name: str, file_type: int, length: int) -> bytes:
    encoded = name.encode("ascii")
    minimum = (8 + len(encoded) + 3) & ~3
    if len(encoded) > 255 or length < minimum:
        raise ValueError(f"invalid ext2 directory entry {name!r}")
    result = bytearray(length)
    struct.pack_into("<IHBB", result, 0, inode, length, len(encoded), file_type)
    result[8 : 8 + len(encoded)] = encoded
    return bytes(result)


def parse_files(values: list[str]) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"file mapping lacks '=': {value}")
        guest, host = value.split("=", 1)
        if not guest.startswith("/") or guest.endswith("/"):
            raise ValueError(f"invalid guest path: {guest}")
        files[guest] = Path(host).read_bytes()
    return files


def build_nodes(files: dict[str, bytes]) -> dict[str, Node]:
    nodes = {
        "/": Node("/", True, inode=ROOT_INODE),
        "/dev": Node("/dev", True),
    }
    for path, data in sorted(files.items()):
        components = path.strip("/").split("/")
        current = ""
        for component in components[:-1]:
            current += "/" + component
            nodes.setdefault(current, Node(current, True))
        nodes[path] = Node(path, False, data=data)

    next_inode = FIRST_NORMAL_INODE
    for path in sorted(nodes, key=lambda item: (item.count("/"), item)):
        if path == "/":
            continue
        nodes[path].inode = next_inode
        next_inode += 1
    if next_inode - 1 > INODE_COUNT:
        raise ValueError("root image has too many inodes")
    return nodes


def allocate_blocks(nodes: dict[str, Node]) -> int:
    next_block = FIRST_CONTENT_BLOCK
    for path in sorted(nodes, key=lambda item: (item.count("/"), item)):
        node = nodes[path]
        count = 1 if node.directory else math.ceil(len(node.data) / BLOCK_SIZE)
        if count > DIRECT_BLOCK_COUNT:
            raise ValueError(f"{path} exceeds the direct-block file limit")
        node.blocks = list(range(next_block, next_block + count))
        next_block += count
    if next_block > BLOCK_COUNT:
        raise ValueError("root image is full")
    return next_block


def write_superblock(image: bytearray, free_blocks: int, free_inodes: int) -> None:
    block = memoryview(image)[BLOCK_SIZE : 2 * BLOCK_SIZE]
    put_u32(block, 0, INODE_COUNT)
    put_u32(block, 4, BLOCK_COUNT)
    put_u32(block, 8, 0)
    put_u32(block, 12, free_blocks)
    put_u32(block, 16, free_inodes)
    put_u32(block, 20, FIRST_DATA_BLOCK)
    put_u32(block, 24, 0)
    put_u32(block, 28, 0)
    put_u32(block, 32, 8192)
    put_u32(block, 36, 8192)
    put_u32(block, 40, INODE_COUNT)
    put_u16(block, 52, 0)
    put_u16(block, 54, 0xFFFF)
    put_u16(block, 56, 0xEF53)
    put_u16(block, 58, 1)
    put_u16(block, 60, 1)
    put_u32(block, 72, 0)
    put_u32(block, 76, 1)
    put_u32(block, 84, FIRST_NORMAL_INODE)
    put_u16(block, 88, INODE_SIZE)
    put_u32(block, 92, 0)
    put_u32(block, 96, 0x2)
    put_u32(block, 100, 0)
    block[104:120] = bytes.fromhex("524f5345000000000000000000000001")
    block[120:136] = b"ROSE root".ljust(16, b"\0")
    block[136:200] = b"/".ljust(64, b"\0")


def write_group_descriptor(
    image: bytearray, free_blocks: int, free_inodes: int, directory_count: int
) -> None:
    block = memoryview(image)[2 * BLOCK_SIZE : 3 * BLOCK_SIZE]
    put_u32(block, 0, BLOCK_BITMAP_BLOCK)
    put_u32(block, 4, INODE_BITMAP_BLOCK)
    put_u32(block, 8, INODE_TABLE_BLOCK)
    put_u16(block, 12, free_blocks)
    put_u16(block, 14, free_inodes)
    put_u16(block, 16, directory_count)


def mark_bit(bitmap: memoryview, index: int) -> None:
    bitmap[index // 8] |= 1 << (index % 8)


def write_bitmaps(image: bytearray, nodes: dict[str, Node], next_block: int) -> int:
    block_bitmap = memoryview(image)[
        BLOCK_BITMAP_BLOCK * BLOCK_SIZE : (BLOCK_BITMAP_BLOCK + 1) * BLOCK_SIZE
    ]
    for block in range(FIRST_DATA_BLOCK, next_block):
        mark_bit(block_bitmap, block - FIRST_DATA_BLOCK)

    inode_bitmap = memoryview(image)[
        INODE_BITMAP_BLOCK * BLOCK_SIZE : (INODE_BITMAP_BLOCK + 1) * BLOCK_SIZE
    ]
    used_inodes = set(range(1, FIRST_NORMAL_INODE))
    used_inodes.update(node.inode for node in nodes.values())
    for inode in used_inodes:
        mark_bit(inode_bitmap, inode - 1)
    return len(used_inodes)


def write_inode(image: bytearray, node: Node, child_directory_count: int) -> None:
    table_offset = INODE_TABLE_BLOCK * BLOCK_SIZE
    offset = table_offset + (node.inode - 1) * INODE_SIZE
    mode = (0x4000 | 0o755) if node.directory else (0x8000 | 0o755)
    size = BLOCK_SIZE if node.directory else len(node.data)
    links = 2 + child_directory_count if node.directory else 1
    put_u16(image, offset, mode)
    put_u32(image, offset + 4, size)
    put_u16(image, offset + 26, links)
    put_u32(image, offset + 28, len(node.blocks) * (BLOCK_SIZE // 512))
    for index, block in enumerate(node.blocks):
        put_u32(image, offset + 40 + index * 4, block)


def write_directories(image: bytearray, nodes: dict[str, Node]) -> None:
    for node in nodes.values():
        if not node.directory:
            continue
        parent_inode = (
            node.inode if node.path == "/" else nodes[node.parent_path].inode
        )
        children = sorted(
            (candidate for candidate in nodes.values() if candidate.path != "/" and candidate.parent_path == node.path),
            key=lambda candidate: candidate.name,
        )
        entries = [(node.inode, ".", 2), (parent_inode, "..", 2)]
        entries.extend(
            (child.inode, child.name, 2 if child.directory else 1)
            for child in children
        )
        payload = bytearray()
        for index, (inode, name, file_type) in enumerate(entries):
            minimum = (8 + len(name.encode("ascii")) + 3) & ~3
            length = BLOCK_SIZE - len(payload) if index == len(entries) - 1 else minimum
            payload.extend(directory_entry(inode, name, file_type, length))
        if len(payload) != BLOCK_SIZE:
            raise ValueError(f"directory {node.path} does not fit one block")
        start = node.blocks[0] * BLOCK_SIZE
        image[start : start + BLOCK_SIZE] = payload


def build_image(output: Path, files: dict[str, bytes]) -> None:
    nodes = build_nodes(files)
    next_block = allocate_blocks(nodes)
    image = bytearray(BLOCK_COUNT * BLOCK_SIZE)

    for node in nodes.values():
        if node.directory:
            continue
        for index, block in enumerate(node.blocks):
            chunk = node.data[index * BLOCK_SIZE : (index + 1) * BLOCK_SIZE]
            start = block * BLOCK_SIZE
            image[start : start + len(chunk)] = chunk

    used_inode_count = write_bitmaps(image, nodes, next_block)
    free_blocks = BLOCK_COUNT - next_block
    free_inodes = INODE_COUNT - used_inode_count
    directory_count = sum(node.directory for node in nodes.values())
    write_superblock(image, free_blocks, free_inodes)
    write_group_descriptor(image, free_blocks, free_inodes, directory_count)

    for node in nodes.values():
        child_directories = sum(
            candidate.directory
            and candidate.path != "/"
            and candidate.parent_path == node.path
            for candidate in nodes.values()
        )
        write_inode(image, node, child_directories)
    write_directories(image, nodes)

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_bytes(image)
    temporary.replace(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--file", action="append", default=[])
    arguments = parser.parse_args()
    files = parse_files(arguments.file)
    files["/etc/motd"] = (
        b"Welcome to ROSE. This message was read from writable ext2.\n"
    )
    build_image(arguments.output, files)


if __name__ == "__main__":
    main()
