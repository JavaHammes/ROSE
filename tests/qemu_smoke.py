#!/usr/bin/env python3
"""Boot ROSE in QEMU and exercise its userspace shell interface."""

from __future__ import annotations

import os
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
        require(
            boot,
            "ROSE init: writable disk root online",
            "ROSE userspace shell",
        )

        # The prompt, line editor, quote/backslash parser, and built-ins now run
        # in /bin/sh rather than supervisor mode.
        require(
            session.command("echo 'hello world' \"from sh\" escaped\\ value"),
            "hello world from sh escaped value",
        )
        require(session.command("pwd"), "\n/\n")
        session.command("cd /etc")
        require(session.command("pwd"), "\n/etc\n")
        session.command("cd /")
        require(
            session.command("help"),
            "Built-ins:",
            "Commands: ls cat echo pwd env mkdir rm",
            "Syntax: command",
        )

        require(
            session.command("ls /"),
            "bin/",
            "dev/",
            "etc/",
            "sbin/",
        )
        require(
            session.command("ls /bin"),
            "cat",
            "echo",
            "env",
            "ls",
            "mkdir",
            "pwd",
            "rm",
            "sh",
        )

        require(
            session.command("hello"),
            "Hello from U-mode C",
        )
        require(
            session.command("fault"),
            "terminated by exception 13",
        )
        require(
            session.command("syscall-test"),
            "Working directory passed",
            "Descriptor duplication passed",
            "Descriptor inheritance passed",
            "Fork semantics passed",
            "Signal delivery passed",
            "Job control passed",
            "Process hierarchy passed",
            "Userspace heap passed",
            "Syscall validation passed",
        )
        require(
            session.command("missing"),
            "sh: command not found: missing",
        )

        motd = "Welcome to ROSE. This message was read from writable ext2."
        require(session.command("cat /etc/motd"), motd)

        require(session.command("mkdir /tmp"), "rose> ")
        session.command("echo redirection works > /tmp/message")
        require(session.command("ls /tmp"), "message")
        require(session.command("cat < /tmp/message"), "redirection works")
        session.command("echo two-stage-data | cat > /tmp/two-stage")
        require(session.command("cat /tmp/two-stage"), "two-stage-data")
        session.command("echo three-stage-data | cat | cat > /tmp/three-stage")
        require(session.command("cat /tmp/three-stage"), "three-stage-data")
        require(session.command("pwd | cat"), "\n/\n")
        session.command("rm /tmp/message")
        session.command("rm /tmp/two-stage")
        session.command("rm /tmp/three-stage")
        session.command("rm /tmp")

        require(
            session.command("fs-test"),
            "Filesystem mutation passed",
        )

        session.command("setenv ROSE_TEST passed")
        require(
            session.command("args-env alpha beta"),
            "Program arguments and environment passed",
        )
        require(
            session.command("env"),
            "HOME=/",
            "PATH=/bin:/sbin",
            "TERM=rose",
            "ROSE_TEST=passed",
        )
        require(
            session.command("env | cat"),
            "HOME=/",
            "PATH=/bin:/sbin",
            "ROSE_TEST=passed",
        )

        require(
            session.command("execve"),
            "Execve replacement passed",
        )

        console = session.command_with_input(
            "console-read", b"Console reader waiting\r\n", b"Z"
        )
        require(
            console,
            "Console reader waiting",
            "Console read: Z",
        )

        interrupted = session.command_with_input(
            "console-read", b"Console reader waiting\r\n", b"\x03"
        )
        require(interrupted, "Console reader waiting", "rose> ")

        stopped = session.command_with_input(
            "console-read", b"Console reader waiting\r\n", b"\x1a"
        )
        require(stopped, "[1] Stopped")
        require(session.command("jobs"), "[1] Stopped")
        resumed = session.command_with_input("fg 1", b"fg 1\r\n", b"Y")
        require(resumed, "Console read: Y")
        require(session.command("jobs"), "No jobs")

        background = session.command("hello &")
        require(background, "[1] Running")
        if "Hello from U-mode C" not in background:
            session.read_until(b"Hello from U-mode C", 5.0)
        require(session.command("jobs"), "[1] Done")

        background_reader = session.command("console-read &")
        require(background_reader, "[1] Running")
        if "Console reader waiting" not in background_reader:
            session.read_until(b"Console reader waiting", 5.0)
        time.sleep(0.05)
        require(session.command("jobs"), "[1] Stopped")
        resumed_reader = session.command_with_input("fg 1", b"fg 1\r\n", b"B")
        require(resumed_reader, "Console read: B")

        require(
            session.command("pipe-test"),
            "Pipe communication passed",
        )

        # The compatibility alias still supplies the old default demonstration,
        # while parsing and child management remain entirely in userspace.
        require(
            session.command("run"),
            "Hello from U-mode C",
            "Process exited with status 0",
        )

        session.shutdown()
    finally:
        session.close()

    # The disk is the primary boot path, but a missing device must retain the
    # embedded diagnostic environment rather than preventing recovery.
    fallback = QemuSession(base_command)
    try:
        boot = fallback.read_until(b"rose> ", 15.0)
        require(boot, "ROSE userspace shell")
        require(
            fallback.command("hello"),
            "Hello from U-mode C",
        )
        require(fallback.command("ls /bin"), "cat", "echo", "ls", "sh")
        require(
            fallback.command("cat /etc/motd | cat"),
            "Welcome to ROSE",
        )
        require(
            fallback.command("syscall-test"),
            "Descriptor inheritance passed",
            "Fork semantics passed",
            "Signal delivery passed",
            "Job control passed",
            "Syscall validation passed",
        )
        fallback.shutdown()
    finally:
        fallback.close()

    print(f"QEMU userspace-shell smoke test passed ({memory})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
