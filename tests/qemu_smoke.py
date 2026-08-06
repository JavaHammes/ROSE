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
from typing import Iterator, Optional, Union


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

    def graphical_shortcut(self, key: str) -> str:
        """Send one desktop shortcut through the emulated VirtIO keyboard."""
        assert self.process.stdin is not None
        self.process.stdin.write(b"\x01c")
        self.process.stdin.flush()
        output = self.read_until(b"(qemu) ", 5.0)
        self.process.stdin.write(f"sendkey {key}\n".encode())
        self.process.stdin.flush()
        output += self.read_until(b"(qemu) ", 5.0)
        self.process.stdin.write(b"\x01c")
        self.process.stdin.flush()
        return output

    def graphical_power_action(self, action: str) -> str:
        """Choose Restart or Shut Down through the graphical power dialog."""
        if action not in ("restart", "shutdown"):
            raise ValueError(f"unsupported graphical power action: {action}")
        assert self.process.stdin is not None

        # -nographic multiplexes the serial port and HMP monitor. Switching to
        # HMP lets this test exercise VirtIO keyboard input instead of UART.
        self.process.stdin.write(b"\x01c")
        self.process.stdin.flush()
        output = self.read_until(b"(qemu) ", 5.0)

        # The power dialog deliberately focuses Cancel. Reverse traversal
        # reaches Restart first and Shut Down second.
        keys = ["ctrl-alt-q", "shift-tab"]
        if action == "shutdown":
            keys.append("shift-tab")
        keys.append("ret")
        for key in keys:
            self.process.stdin.write(f"sendkey {key}\n".encode())
            self.process.stdin.flush()
            output += self.read_until(b"(qemu) ", 5.0)

        self.process.stdin.write(b"\x01c")
        self.process.stdin.flush()
        return output

    def graphical_keyboard_exit(self) -> str:
        """Launch the desktop and shut it down through the power dialog."""
        assert self.process.stdin is not None
        self.process.stdin.write(b"desktop\n")
        self.process.stdin.flush()
        time.sleep(0.2)
        output = self.graphical_power_action("shutdown")
        return output + self.read_until(b"rose> ", 5.0)

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


def process_pid(table: str, command: str) -> Optional[int]:
    """Return a command's PID from the structured `ps` table."""
    for line in table.splitlines():
        columns = line.split(maxsplit=4)
        if len(columns) == 5 and columns[0].isdigit() and columns[4] == command:
            return int(columns[0])
    return None


def wait_for_process(session: QemuSession, command: str) -> tuple[int, str]:
    """Poll the rescue shell until a graphical-session process appears."""
    output = ""
    for _ in range(20):
        output = session.command("ps")
        pid = process_pid(output, command)
        if pid is not None:
            return pid, output
        time.sleep(0.05)
    raise AssertionError(f"{command} did not start:\n{output}")


def wait_for_message(
    session: QemuSession, existing: str, marker: str, timeout: float = 10.0
) -> str:
    """Accept a supervisor message captured before or after a shell prompt."""
    if marker in existing:
        return existing
    return existing + session.read_until(marker.encode(), timeout)


def run_graphical_recovery_test(command: list[str]) -> None:
    """Prove an empty desktop, manual Terminal launch, and crash recovery."""
    session = QemuSession(command)
    try:
        boot = session.read_until(b"rose> ", 15.0)
        require(
            boot,
            "ROSE init: configuration selected graphical target",
            "ROSE init: starting graphical target with serial rescue shell",
        )
        first_desktop, processes = wait_for_process(session, "/bin/desktop")
        if "/bin/gui-terminal" in processes:
            raise AssertionError(f"desktop auto-started Terminal:\n{processes}")
        session.graphical_shortcut("ctrl-alt-t")
        _, processes = wait_for_process(session, "/bin/gui-terminal")
        require(processes, "/bin/gui-terminal")

        restart = session.graphical_power_action("restart")
        restart += session.read_until(
            b"ROSE init: writable disk root online", 15.0
        )
        restart += session.read_until(b"rose> ", 15.0)
        require(
            restart,
            "ROSE init: system restart requested",
            "ROSE: restarting system",
            "ROSE init: writable disk root online",
        )
        first_desktop, processes = wait_for_process(session, "/bin/desktop")
        if "/bin/gui-terminal" in processes:
            raise AssertionError(
                f"restarted desktop auto-started Terminal:\n{processes}"
            )
        session.graphical_shortcut("ctrl-alt-t")
        _, processes = wait_for_process(session, "/bin/gui-terminal")
        require(processes, "/bin/gui-terminal")

        first_failure = session.command(f"kill -9 {first_desktop}")
        first_failure = wait_for_message(
            session,
            first_failure,
            "ROSE init: restarting graphical session once",
        )
        require(first_failure, "graphical session terminated by a signal")

        second_desktop, processes = wait_for_process(session, "/bin/desktop")
        if second_desktop == first_desktop:
            raise AssertionError("graphical session restart reused its PID")
        if "/bin/gui-terminal" in processes:
            raise AssertionError(
                f"recovered desktop auto-started Terminal:\n{processes}"
            )
        session.graphical_shortcut("ctrl-alt-t")
        _, processes = wait_for_process(session, "/bin/gui-terminal")
        require(processes, "/bin/gui-terminal")

        second_failure = session.command(f"kill -9 {second_desktop}")
        second_failure = wait_for_message(
            session,
            second_failure,
            "diagnostic text session is active",
        )
        require(second_failure, "ROSE init: graphical session failed twice")
        session.shutdown()
    finally:
        session.close()


def run_graphical_shutdown_test(command: list[str]) -> None:
    """Require the graphical power menu to stop the virtual machine."""
    session = QemuSession(command)
    try:
        session.read_until(b"rose> ", 15.0)
        _, processes = wait_for_process(session, "/bin/desktop")
        if "/bin/gui-terminal" in processes:
            raise AssertionError(f"desktop auto-started Terminal:\n{processes}")
        shutdown = session.graphical_power_action("shutdown")
        shutdown += session.read_until(
            b"ROSE init: system shutdown requested", 10.0
        )
        require(shutdown, "ROSE init: system shutdown requested")
        try:
            return_code = session.process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            session._fail("graphical shutdown did not stop QEMU")
        if return_code != 0:
            session._fail(f"graphical shutdown returned status {return_code}")
    finally:
        session.close()


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
    graphical_session_command: Optional[list[str]] = None
    if graphics:
        command += [
            "-device",
            "virtio-gpu-device",
            "-device",
            "virtio-keyboard-device",
            "-device",
            "virtio-tablet-device",
        ]
        graphical_session_command = command.copy()
        command += ["-append", "rose.rescue=1"]

    session = QemuSession(command)
    try:
        boot = session.read_until(b"rose> ", 15.0)
        if not graphics:
            boot += session.read_until(
                b"diagnostic text session is active", 15.0
            )
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
                "ROSE init: boot override selected text target",
            )
            require(
                session.command("desktop --test", timeout=15.0),
                "Graphics userspace test passed",
            )
            require(
                session.command("desktop --test-controls", timeout=15.0),
                "Window control test passed",
            )
            require(
                session.command("desktop --stress", timeout=45.0),
                "GUI capacity stress passed",
            )
            require(
                session.graphical_keyboard_exit(),
                "rose> ",
            )
        else:
            require(
                boot,
                "ROSE init: configuration selected graphical target",
                "ROSE init: restarting graphical session once",
                "ROSE init: graphical session failed twice",
            )

        if graphics_only:
            session.shutdown()
            if ext2_free_counts(root_image) != initial_free_counts:
                raise AssertionError(
                    "graphics guest test leaked blocks or inodes"
                )
            assert graphical_session_command is not None
            run_graphical_recovery_test(graphical_session_command)
            run_graphical_shutdown_test(graphical_session_command)
            if ext2_free_counts(root_image) != initial_free_counts:
                raise AssertionError(
                    "graphical recovery test leaked blocks or inodes"
                )
            print(f"QEMU graphics and recovery smoke test passed ({memory})")
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
        require(session.command("echo startup=$ROSE_VERSION"), "startup=4")
        require(
            session.command("type cd ll cp"),
            "cd is a shell builtin",
            "ll is an alias for 'ls'",
            "cp is /bin/cp",
        )
        require(session.command("ll /etc"), "motd", "roserc")
        session.command("export EXPORTED=value")
        require(session.command("echo $EXPORTED"), "\nvalue\n")
        session.command("unset EXPORTED")
        require(session.command("echo x${EXPORTED}x"), "\nxx\n")
        session.command("alias greet='echo alias works'")
        require(session.command("greet"), "\nalias works\n")
        session.command("unalias greet")
        require(
            session.command("history"), "alias greet='echo alias works'", "history"
        )
        session.command("echo personal-startup > /.roserc")
        session.command("echo exit >> /.roserc")
        require(session.command("/bin/sh"), "personal-startup", "Shutting down")
        session.command("rm /.roserc")

        require(
            session.command("echo bad |"), "sh: syntax error: expected command"
        )
        require(
            session.command("echo 'bad"), "sh: syntax error: unterminated quote"
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
            session.command("ps"),
            "PID PPID PGID STATE COMMAND",
            "/sbin/init",
            "/bin/sh",
            "/bin/ps",
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
        require(session.edited_command(b"cat /etc/mo\t\n"), motd)

        require(session.command("mkdir /tmp"), "rose> ")
        session.command("touch /tmp/empty")
        session.command("echo first line > /tmp/lines")
        session.command("echo second row >> /tmp/lines")
        require(session.command("head -n 1 /tmp/lines"), "\nfirst line\n")
        require(session.command("wc /tmp/lines"), "2 4 22 /tmp/lines")
        session.command("cp /tmp/lines /tmp/copy")
        require(session.command("cat /tmp/copy"), "first line", "second row")
        session.command("mv /tmp/copy /tmp/moved")
        require(
            session.command("find /tmp"), "/tmp/empty", "/tmp/lines", "/tmp/moved"
        )
        require(session.command("sleep 0"), "rose> ")
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
        session.command("rm /tmp/empty")
        session.command("rm /tmp/lines")
        session.command("rm /tmp/moved")
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

        require(session.command("sleep 5 &"), "Running")
        require(session.command("sleep 5 &"), "Running")
        previous_kill = session.command("kill %-")
        current_kill = session.command("kill %+")
        if "no such job" in previous_kill + current_kill:
            raise AssertionError(
                "current/previous job selection failed:\n"
                f"{previous_kill}{current_kill}"
            )
        session.command("sleep 0")

        stopped_sleep = session.command("sleep 5 &")
        require(stopped_sleep, "Running")
        session.command("kill -19 %+")
        require(session.command("jobs"), "Stopped")
        require(session.command("bg"), "Running")
        session.command("kill %+")
        session.command("jobs")

        require(session.command("sleep 1 &"), "Running")
        require(session.command("sleep 2", timeout=5.0), "Done")

        external_kill_job = session.command("sleep 5 &")
        running_lines = [
            line for line in external_kill_job.splitlines()
            if "] Running " in line
        ]
        if not running_lines:
            raise AssertionError(f"missing background PID:\n{external_kill_job}")
        background_pid = running_lines[-1].rsplit(" ", 1)[-1]
        session.command(f"/bin/kill {background_pid}")
        session.command("jobs")

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
