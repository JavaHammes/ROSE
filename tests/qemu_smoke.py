#!/usr/bin/env python3
"""Boot ROSE in QEMU and exercise its public terminal interface."""

from __future__ import annotations

import os
import re
import select
import subprocess
import sys
import time
from pathlib import Path


class QemuSession:
    """Own one QEMU process and retain a transcript for useful failures."""

    def __init__(self, command: list[str]) -> None:
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.output = bytearray()

    def read_until(self, marker: bytes, timeout: float = 10.0) -> str:
        """Collect output until marker appears, or fail with the transcript."""
        deadline = time.monotonic() + timeout
        start = len(self.output)

        while marker not in self.output[start:]:
            if self.process.poll() is not None:
                self._fail(f"QEMU exited with status {self.process.returncode}")
            if time.monotonic() >= deadline:
                self._fail(f"timed out waiting for {marker!r}")

            assert self.process.stdout is not None
            readable, _, _ = select.select([self.process.stdout], [], [], 0.1)
            if readable:
                chunk = os.read(self.process.stdout.fileno(), 4096)
                if chunk:
                    self.output.extend(chunk)

        return self.output[start:].decode("utf-8", errors="replace").replace("\r", "")

    def command(self, text: str, timeout: float = 10.0) -> str:
        """Submit one shell command and return everything through its prompt."""
        start = len(self.output)
        assert self.process.stdin is not None
        self.process.stdin.write((text + "\n").encode())
        self.process.stdin.flush()
        self.read_until(b"rose> ", timeout)
        return self.output[start:].decode("utf-8", errors="replace").replace("\r", "")

    def command_with_input(
        self, text: str, wait_marker: bytes, input_bytes: bytes, timeout: float = 10.0
    ) -> str:
        """Start a blocking command, wait for readiness, then inject input."""
        start = len(self.output)
        assert self.process.stdin is not None
        self.process.stdin.write((text + "\n").encode())
        self.process.stdin.flush()
        self.read_until(wait_marker, timeout)
        self.process.stdin.write(input_bytes)
        self.process.stdin.flush()
        self.read_until(b"rose> ", timeout)
        return self.output[start:].decode("utf-8", errors="replace").replace("\r", "")

    def shutdown(self) -> None:
        """Exercise the guest exit command and require a successful QEMU exit."""
        assert self.process.stdin is not None
        self.process.stdin.write(b"exit\n")
        self.process.stdin.flush()
        self.read_until(b"Shutting down...", 5.0)
        try:
            return_code = self.process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self._fail("QEMU did not stop after the exit command")
        if return_code != 0:
            self._fail(f"QEMU shutdown returned status {return_code}")

    def close(self) -> None:
        """Best-effort cleanup used when an assertion interrupts the test."""
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def _fail(self, message: str) -> None:
        transcript = self.output.decode("utf-8", errors="replace").replace("\r", "")
        raise RuntimeError(f"{message}\n\nQEMU transcript:\n{transcript}")


def require(output: str, *needles: str) -> None:
    """Assert that one command response contains every expected fragment."""
    missing = [needle for needle in needles if needle not in output]
    if missing:
        raise AssertionError(f"missing {missing!r} in command output:\n{output}")


def used_pages(output: str) -> int:
    """Extract the allocator's used-page count from meminfo output."""
    match = re.search(r"used:\s+(\d+) pages", output)
    if match is None:
        raise AssertionError(f"could not parse memory usage:\n{output}")
    return int(match.group(1))


def spawned_pid(output: str) -> int:
    """Extract the PID assigned by the spawn command."""
    match = re.search(r"Spawned process (\d+)", output)
    if match is None:
        raise AssertionError(f"could not parse process ID:\n{output}")
    return int(match.group(1))


def main() -> int:
    # Environment overrides mirror Makefile variables and keep the test usable
    # with differently named cross-toolchains or QEMU installations.
    repository = Path(__file__).resolve().parents[1]
    qemu = os.environ.get("QEMU", "qemu-system-riscv64")
    kernel = Path(os.environ.get("KERNEL", repository / "kernel/build/kernel.elf"))
    root_image = Path(
        os.environ.get("ROOT_IMAGE", repository / "kernel/build/root.ext2")
    )
    memory = os.environ.get("QEMU_MEMORY", "128M")
    base_command = [
        qemu,
        "-machine",
        "virt",
        "-smp",
        "1",
        "-m",
        memory,
        "-nographic",
        "-bios",
        "default",
        "-kernel",
        str(kernel),
    ]
    command = base_command + [
        "-drive",
        f"file={root_image},format=raw,if=none,id=rose-root",
        "-device",
        "virtio-blk-device,drive=rose-root",
        "-global",
        "virtio-mmio.force-legacy=false",
    ]

    session = QemuSession(command)
    try:
        boot = session.read_until(b"rose> ", 15.0)
        require(boot, "ROSE init: writable disk root online")

        info = session.command("info")
        memory_match = re.fullmatch(r"(\d+)M", memory)
        if memory_match is not None:
            expected_ram_end = 0x80000000 + int(memory_match.group(1)) * 1024 * 1024
            require(
                info,
                "RAM: 0x0000000080000000 - " f"0x{expected_ram_end:016x}",
            )
        require(
            info,
            "Virtual memory: Sv39",
            "RAM: 0x",
            "Timer frequency: 10000000 Hz",
            "UART: 0x0000000010000000 (IRQ 10)",
            "PLIC: 0x000000000c000000",
            "Root filesystem: writable ext2",
            "Block device: VirtIO (512-byte sectors: 8192)",
        )

        # The upper bound guards against accidentally returning to the original
        # all-4-KiB kernel identity map, which consumed about 70 table pages.
        baseline = used_pages(session.command("meminfo"))
        if baseline > 8:
            raise AssertionError(f"kernel page tables use {baseline} pages; expected at most 8")

        require(
            session.command("run"),
            "Hello from U-mode C",
            "exited with status 0",
            "Scheduler blocks:",
        )
        require(
            session.command("run /bin/fault"),
            "terminated by exception 13",
            "exited with status 1",
        )
        require(
            session.command("run /bin/syscall-test"),
            "Syscall validation passed",
            "exited with status 0",
            "Scheduler blocks:",
        )
        require(
            session.command("run /bin/missing"),
            "Unable to load program: /bin/missing",
        )

        motd = "Welcome to ROSE. This message was read from writable ext2."
        require(session.command("run /bin/cat"), motd, "exited with status 0")

        require(
            session.command("run /bin/fs-test"),
            "Filesystem mutation passed",
            "exited with status 0",
        )

        console = session.command_with_input(
            "run /bin/console-read", b"Console reader waiting\r\n", b"Z"
        )
        require(
            console,
            "Console reader waiting",
            "Console read: Z",
            "exited with status 0",
            "Scheduler blocks:",
        )

        # Direct runs leave zombies by design. Reap them before filling the
        # fixed process table with concurrency tests.
        require(session.command("reap"), "Reaped 6 process(es)")

        first_cat = spawned_pid(session.command("spawn /bin/cat"))
        second_cat = spawned_pid(session.command("spawn /bin/cat"))
        cat_pair = session.command("wait")
        if cat_pair.count(motd) != 2:
            raise AssertionError(f"independent file offsets failed:\n{cat_pair}")
        require(
            cat_pair,
            f"Process {first_cat} exited with status 0",
            f"Process {second_cat} exited with status 0",
        )
        require(session.command("reap"), "Reaped 2 process(es)")

        # Separate spawn from wait so READY persistence and ps are tested before
        # the foreground scheduler consumes the processes.
        process_a = spawned_pid(session.command("spawn /bin/process-a"))
        process_b = spawned_pid(session.command("spawn /bin/process-b"))
        process_table = session.command("ps")
        require(process_table, f"{process_a}    ready", f"{process_b}    ready")

        scheduler = session.command("wait", 15.0)
        require(
            scheduler,
            "Process A: running",
            "Process B: running",
            "Scheduler switches:",
            "preemptions)",
            "Scheduler blocks:",
        )
        preemptions = re.search(r"\((\d+) preemptions\)", scheduler)
        if preemptions is None or int(preemptions.group(1)) == 0:
            raise AssertionError(f"timer did not preempt the user processes:\n{scheduler}")

        killed = spawned_pid(session.command("spawn /bin/hello"))
        require(session.command(f"kill {killed}"), f"Terminated process {killed}")
        require(
            session.command("wait"),
            "No ready processes",
            f"Process {killed} exited with status 137",
        )

        require(session.command("reap"), "Reaped ")
        require(session.command("ps"), "(no processes)")

        # All process-owned stacks, ELF pages, and page tables must be gone.
        final_used = used_pages(session.command("meminfo"))
        if final_used != baseline:
            raise AssertionError(
                f"page leak detected: baseline {baseline}, final {final_used}"
            )

        session.shutdown()
    finally:
        session.close()

    # The disk is the primary boot path, but a missing device must retain the
    # embedded diagnostic environment rather than preventing recovery.
    fallback = QemuSession(base_command)
    try:
        fallback.read_until(b"rose> ", 15.0)
        require(
            fallback.command("info"),
            "Root filesystem: embedded ramfs fallback",
        )
        require(
            fallback.command("run /bin/hello"),
            "Hello from U-mode C",
            "exited with status 0",
        )
        fallback.shutdown()
    finally:
        fallback.close()

    print(f"QEMU smoke test passed ({memory}, {baseline} kernel pages in use)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
