#!/usr/bin/env python3
"""Boot ROSE in QEMU and exercise its userspace shell interface."""

from __future__ import annotations

import os
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Union


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
        self.cursor = 0

    @staticmethod
    def _decode(output: Union[bytes, bytearray]) -> str:
        return output.decode("utf-8", errors="replace").replace("\r", "")

    def read_until(self, marker: bytes, timeout: float = 10.0) -> str:
        """Collect output until marker appears, or fail with the transcript."""
        deadline = time.monotonic() + timeout
        start = self.cursor

        while True:
            marker_index = self.output.find(marker, self.cursor)
            if marker_index >= 0:
                end = marker_index + len(marker)
                self.cursor = end
                return self._decode(self.output[start:end])
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

    def command(self, text: str, timeout: float = 10.0) -> str:
        """Submit one shell command and return everything through its prompt."""
        assert self.process.stdin is not None
        self.process.stdin.write((text + "\n").encode())
        self.process.stdin.flush()
        return self.read_until(b"rose> ", timeout)

    def edited_command(self, input_bytes: bytes, timeout: float = 10.0) -> str:
        """Submit raw line-editor input, including navigation sequences."""
        assert self.process.stdin is not None
        self.process.stdin.write(input_bytes)
        self.process.stdin.flush()
        # Cursor movement redraws the current prompt. Wait for a prompt at the
        # start of a new line so an intermediate redraw cannot finish the test.
        return self.read_until(b"\nrose> ", timeout)

    def command_with_input(
        self, text: str, wait_marker: bytes, input_bytes: bytes, timeout: float = 10.0
    ) -> str:
        """Start a blocking command, wait for readiness, then inject input."""
        assert self.process.stdin is not None
        self.process.stdin.write((text + "\n").encode())
        self.process.stdin.flush()
        before_input = self.read_until(wait_marker, timeout)
        self.process.stdin.write(input_bytes)
        self.process.stdin.flush()
        return before_input + self.read_until(b"rose> ", timeout)

    def graphical_keyboard_exit(self) -> str:
        """Launch the desktop and exit it through the emulated keyboard."""
        assert self.process.stdin is not None
        self.process.stdin.write(b"desktop\n")
        self.process.stdin.flush()
        time.sleep(0.2)

        # -nographic multiplexes the serial port and HMP monitor. Switching to
        # HMP lets this test exercise VirtIO keyboard input instead of UART.
        self.process.stdin.write(b"\x01c")
        self.process.stdin.flush()
        output = self.read_until(b"(qemu) ", 5.0)
        # Escape belongs to the focused terminal. The desktop-wide chord is
        # deliberately harder to trigger from a character-mode application.
        self.process.stdin.write(b"sendkey ctrl-alt-esc\n")
        self.process.stdin.flush()
        output += self.read_until(b"rose> ", 5.0)
        self.process.stdin.write(b"\x01c")
        self.process.stdin.flush()
        return output

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
        if self.process.stdin is not None:
            try:
                self.process.stdin.close()
            except BrokenPipeError:
                pass
        if self.process.stdout is not None:
            self.process.stdout.close()

    def _fail(self, message: str) -> None:
        transcript = self._decode(self.output)
        raise RuntimeError(f"{message}\n\nQEMU transcript:\n{transcript}")


def require(output: str, *needles: str) -> None:
    """Assert that one command response contains every expected fragment."""
    missing = [needle for needle in needles if needle not in output]
    if missing:
        raise AssertionError(f"missing {missing!r} in command output:\n{output}")


def ext2_free_counts(path: Path) -> tuple[int, int]:
    """Read the free block and inode counts from the 1 KiB ext2 superblock."""
    with path.open("rb") as image:
        image.seek(1024 + 12)
        counts = image.read(8)
    if len(counts) != 8:
        raise AssertionError(f"short ext2 superblock in {path}")
    return struct.unpack("<II", counts)


@contextmanager
def temporary_root_image(source: Path) -> Iterator[Path]:
    """Give one QEMU run a disposable copy of the writable root image."""
    with tempfile.TemporaryDirectory(prefix="rose-qemu-") as directory:
        working_copy = Path(directory) / "root.ext2"
        shutil.copyfile(source, working_copy)
        yield working_copy


def run_smoke_test(
    qemu: str,
    kernel: Path,
    root_image: Path,
    memory: str,
    graphics: bool,
    graphics_only: bool,
) -> int:
    initial_free_counts = ext2_free_counts(root_image)
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
    if graphics:
        command += [
            "-device",
            "virtio-gpu-device",
            "-device",
            "virtio-keyboard-device",
            "-device",
            "virtio-tablet-device",
        ]

    session = QemuSession(command)
    try:
        boot = session.read_until(b"rose> ", 15.0)
        require(
            boot,
            "ROSE init: writable disk root online",
            "ROSE userspace shell",
        )
        if graphics:
            require(
                boot,
                "ROSE graphics: 1024x768",
                "ROSE input: VirtIO keyboard/tablet online",
            )
            require(
                session.command("desktop --test", timeout=15.0),
                "Graphics userspace test passed",
            )
            require(
                session.command("desktop --stress", timeout=45.0),
                "GUI capacity stress passed",
            )
            require(session.graphical_keyboard_exit(), "rose> ")

        if graphics_only:
            session.shutdown()
            if ext2_free_counts(root_image) != initial_free_counts:
                raise AssertionError(
                    "graphics guest test leaked blocks or inodes"
                )
            print(f"QEMU graphics smoke test passed ({memory})")
            return 0

        # The prompt, line editor, quote/backslash parser, and built-ins now run
        # in /bin/sh rather than supervisor mode.
        require(
            session.command("echo 'hello world' \"from sh\" escaped\\ value"),
            "hello world from sh escaped value",
        )
        require(
            session.edited_command(
                b"echo hllo" + b"\x1b[D" * 4 + b"\x1b[C" + b"e\n"
            ),
            "\nhello\n",
        )
        require(
            session.edited_command(b"BADecho home\x1b[H" + b"\x1b[3~" * 3 + b"\n"),
            "\nhome\n",
        )
        require(session.edited_command(b"\x1b[A\n"), "\nhome\n")
        require(session.command("pwd"), "\n/\n")
        session.command("cd /etc")
        require(session.command("pwd"), "\n/etc\n")
        session.command("cd /")
        require(
            session.command("help"),
            "Built-ins:",
            "Commands: ls cat echo pwd env mkdir rm",
            "Syntax: command",
            "Expansion: $NAME ${NAME} $? $$",
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
            "Time and event waiting passed",
            "Descriptor inheritance passed",
            "Fork semantics passed",
            "Copy-on-write passed",
            "Anonymous mmap passed",
            "Signal delivery passed",
            "Job control passed",
            "Process hierarchy passed",
            "Userspace heap passed",
            "Shared memory passed",
            "Pseudo-terminal passed",
            "System telemetry passed",
            "Syscall validation passed",
        )
        require(
            session.command("missing"),
            "sh: command not found: missing",
        )
        require(session.command("echo status=$?"), "\nstatus=127\n")
        shell_pid = session.command("echo shell-pid=$$")
        pid_lines = [
            line for line in shell_pid.splitlines() if line.startswith("shell-pid=")
        ]
        if len(pid_lines) != 1 or not pid_lines[0][len("shell-pid=") :].isdigit():
            raise AssertionError(f"invalid shell PID expansion:\n{shell_pid}")

        motd = "Welcome to ROSE. This message was read from writable ext2."
        require(session.command("cat /etc/motd"), motd)

        require(session.command("mkdir /tmp"), "rose> ")
        session.command("setenv ROSE_OUTPUT /tmp/expanded-output")
        session.command('echo expanded-redirection > "$ROSE_OUTPUT"')
        require(session.command("cat /tmp/expanded-output"), "expanded-redirection")
        session.command("echo redirection works > /tmp/message")
        session.command("echo append works >> /tmp/message")
        session.command("echo numbered output 1> /tmp/numbered-output")
        session.command("cat /first-missing 2> /tmp/error-output")
        session.command("cat /second-missing 2>> /tmp/error-output")
        session.command("cd /missing 2> /tmp/builtin-error")
        session.command("missing-command 2> /tmp/not-found-error")
        session.command("cat /third-missing > /tmp/combined-output 2>&1")
        session.command(
            "cat /fourth-missing 2>&1 | cat > /tmp/pipeline-error"
        )
        require(session.command("ls /tmp"), "message")
        require(
            session.command("cat < /tmp/message"),
            "redirection works",
            "append works",
        )
        require(session.command("cat 0< /tmp/numbered-output"), "numbered output")
        require(
            session.command("cat /tmp/error-output"),
            "cat: unable to open: /first-missing",
            "cat: unable to open: /second-missing",
        )
        require(
            session.command("cat /tmp/builtin-error"),
            "cd: unable to change directory: /missing",
        )
        require(
            session.command("cat /tmp/not-found-error"),
            "sh: command not found: missing-command",
        )
        require(
            session.command("cat /tmp/combined-output"),
            "cat: unable to open: /third-missing",
        )
        require(
            session.command("cat /tmp/pipeline-error"),
            "cat: unable to open: /fourth-missing",
        )
        session.command("echo two-stage-data | cat > /tmp/two-stage")
        require(session.command("cat /tmp/two-stage"), "two-stage-data")
        session.command("echo three-stage-data | cat | cat > /tmp/three-stage")
        require(session.command("cat /tmp/three-stage"), "three-stage-data")
        require(session.command("pwd | cat"), "\n/\n")
        session.command("rm /tmp/message")
        session.command("rm /tmp/numbered-output")
        session.command("rm /tmp/error-output")
        session.command("rm /tmp/builtin-error")
        session.command("rm /tmp/not-found-error")
        session.command("rm /tmp/combined-output")
        session.command("rm /tmp/pipeline-error")
        session.command("rm /tmp/two-stage")
        session.command("rm /tmp/three-stage")
        session.command("rm /tmp/expanded-output")
        session.command("rm /tmp")
        session.command("unsetenv ROSE_OUTPUT")

        require(
            session.command("fs-test"),
            "Filesystem mutation passed",
        )

        session.command("setenv ROSE_TEST passed")
        require(
            session.command("echo before-${ROSE_TEST}-after"),
            "\nbefore-passed-after\n",
        )
        require(
            session.command("echo '$ROSE_TEST' \\$ROSE_TEST \"$ROSE_TEST\""),
            "\n$ROSE_TEST $ROSE_TEST passed\n",
        )
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
        if ext2_free_counts(root_image) != initial_free_counts:
            raise AssertionError(
                "guest filesystem operations leaked blocks or inodes"
            )
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
            "Time and event waiting passed",
            "Descriptor inheritance passed",
            "Fork semantics passed",
            "Copy-on-write passed",
            "Anonymous mmap passed",
            "Signal delivery passed",
            "Job control passed",
            "Shared memory passed",
            "Pseudo-terminal passed",
            "System telemetry passed",
            "Syscall validation passed",
        )
        fallback.shutdown()
    finally:
        fallback.close()

    print(f"QEMU userspace-shell smoke test passed ({memory})")
    return 0


def main() -> int:
    # Environment overrides mirror Makefile variables and keep the test usable
    # with differently named cross-toolchains or QEMU installations.
    repository = Path(__file__).resolve().parents[1]
    qemu = os.environ.get("QEMU", "qemu-system-riscv64")
    kernel = Path(os.environ.get("KERNEL", repository / "kernel/build/kernel.elf"))
    source_root_image = Path(
        os.environ.get("ROOT_IMAGE", repository / "kernel/build/root.ext2")
    )
    memory = os.environ.get("QEMU_MEMORY", "128M")
    graphics = os.environ.get("QEMU_GRAPHICS") == "1"
    graphics_only = os.environ.get("QEMU_GRAPHICS_ONLY") == "1"

    with temporary_root_image(source_root_image) as root_image:
        return run_smoke_test(
            qemu, kernel, root_image, memory, graphics, graphics_only
        )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
