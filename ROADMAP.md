# ROSE Shell and Desktop Roadmap

## Goal

Make ROSE feel coherent and responsive as an everyday small operating system,
with the shell and graphical desktop as its primary user experience.

This roadmap does not aim for immediate POSIX compatibility or feature parity
with Linux. The target is a polished, internally consistent ROSE environment:

- responsive input and predictable idle behavior;
- a terminal and shell that are comfortable for sustained use;
- applications that can create and manage windows dynamically;
- a reusable GUI toolkit and consistent visual language;
- a graphical session that boots, recovers, and shuts down cleanly;
- automated tests for the interactions that make the system feel dependable.

## Starting point

ROSE already has most of the difficult foundations needed for this work:

- protected user processes with preemption, signals, process groups, `fork`,
  `execve`, `spawn`, and `waitpid`;
- pipes, descriptor inheritance, PTYs, and terminal foreground groups;
- a writable ext2 filesystem and a useful set of file syscalls;
- an interactive userspace shell with quoting, parameter expansion, pipelines,
  redirection, background jobs, `jobs`, and `fg`;
- a userspace compositor with overlapping shared-memory surfaces, focus,
  dragging, close requests, input routing, and damage tracking;
- graphical Terminal, Files, and System Monitor processes.

The most visible current constraints are:

- shell input supports typing and backspace, but not cursor editing, history,
  completion, or rich job notifications;
- the graphical terminal uses a fixed grid, discards most escape sequences,
  has no scrollback, and cannot resize;
- the desktop creates exactly three fixed applications and has no general
  client connection or application-launch protocol;
- windows cannot be resized, minimized, maximized, or created after desktop
  startup;
- GUI clients and the compositor poll and call `yield` while idle instead of
  waiting for events;
- drawing is limited to rectangles and a small uppercase bitmap font;
- small fixed process, descriptor, shared-memory, and window limits will become
  user-visible as the desktop grows.

## Product principles

1. Keep policy in userspace. Window behavior, shell syntax, session management,
   widgets, and application policy belong outside the kernel.
2. Add general kernel primitives, not GUI-specific shortcuts. Time, blocking
   waits, descriptor readiness, terminal modes, and IPC should serve both shell
   and graphical programs.
3. Prefer versioned message protocols over shared structure assumptions. Pixel
   buffers can remain shared memory, while lifecycle and input use explicit
   messages with validated lengths and versions.
4. Make every milestone usable on its own. Each stage should end in a visible
   improvement and a testable release, not only internal refactoring.
5. Preserve the serial console as a rescue and automation path even when the
   normal boot target becomes graphical.
6. Stay bounded before becoming dynamic. Raising well-checked limits is an
   acceptable intermediate step; unbounded allocation is not required for the
   first polished desktop.

## Milestone 0: Define the experience and protect the baseline

**Outcome:** There is a measurable definition of “feels real,” and existing
kernel, shell, filesystem, and graphics behavior remains stable while the UI is
rebuilt.

- Add a graphical session smoke test that launches an app, focuses it, types
  into it, moves it, closes it, and exits the session.
- Record baseline boot time, idle scheduler activity, input-to-frame latency,
  memory use, and repeated app launch/close resource counts.
- Add screenshot-based golden checks only for a few stable states: desktop
  startup, focused terminal, launcher, and a dialog. Keep behavioral tests as
  the primary signal so harmless pixel changes are not painful.
- Define a compact visual specification: spacing scale, colors, type sizes,
  focus treatment, window controls, pointer states, and standard shortcuts.
- Keep `make test`, `make test-graphics`, and the serial shell as release gates.

**Exit criteria**

- A test can drive a complete graphical session without a human.
- Repeatedly opening and closing every bundled app leaks no processes, pages,
  descriptors, or shared-memory objects.
- Baseline performance numbers are documented and reproducible.

## Milestone 1: Event-driven OS primitives

**Outcome:** Interactive programs sleep until something happens. The desktop
feels responsive, monitoring has real time semantics, and later UI work does
not depend on busy loops.

### Time and waiting

- Add monotonic time and sleep syscalls backed by scheduler timer wait channels.
- Add descriptor readiness waiting (`poll` is enough) for console, pipes, and
  PTYs.
- Make graphics/input events waitable. Either expose input as a descriptor or
  add a general waitable event primitive; avoid a compositor-only sleep syscall.
- Support timeouts so an event loop can wait for input, child state, IPC, or its
  next animation/update deadline in one place.

### Terminal semantics

- Add terminal attributes for canonical/raw input, echo, and signal-generating
  control characters.
- Add PTY window size state and a resize notification such as `SIGWINCH`.
- Complete session and controlling-terminal semantics sufficiently for nested
  shells and independent graphical terminals.

### Capacity

- Raise and stress-test the process, open-file, per-process descriptor,
  shared-memory, and mapping limits needed for at least 12 simultaneous GUI
  windows.
- Make exhaustion errors visible and recoverable rather than allowing one app
  to take down the session.

**Exit criteria**

- An idle desktop and terminal block instead of continuously calling `yield`.
- Timer-based UI updates happen at stable wall-clock intervals.
- PTY programs can switch between canonical and raw mode and receive their
  current terminal size.
- Twelve small applications can be opened and closed repeatedly without leaks.

## Milestone 2: A real terminal emulator

**Outcome:** The graphical terminal is a faithful frontend for character-mode
programs instead of a painted transcript.

- Replace the one-bit escape-state logic with a bounded ANSI/VT parser.
- Implement the useful core first: cursor movement, erase line/screen, save and
  restore cursor, SGR reset/bold/inverse, and 16 foreground/background colors.
- Store character attributes per cell and render a visible active/inactive
  cursor.
- Replace the fixed 92-by-40 grid with dimensions derived from the window size.
- Add a bounded scrollback ring, Page Up/Page Down navigation, and a “jump to
  latest output” behavior.
- Add mouse selection and copy/paste after the clipboard service in Milestone 4
  exists; keep the selection model inside the terminal.
- Support key repeat and translate navigation keys consistently.
- Load a complete printable-ASCII bitmap font from a userspace resource rather
  than embedding separate font copies in the compositor and GUI library.
- Damage only changed rows/cells instead of redrawing the complete surface.

**Exit criteria**

- `clear`, colored output, cursor-addressing programs, and full-screen test
  patterns render correctly.
- Resizing a terminal changes PTY rows/columns and does not lose the running
  shell.
- At least 1,000 lines of output can be reviewed without corrupting live input.
- Two independently launched terminal windows can run shells simultaneously.

## Milestone 3: An everyday interactive shell

**Outcome:** `/bin/sh` is pleasant for normal navigation, file work, program
launching, and job control while remaining intentionally smaller than a full
POSIX shell.

### Line editor and prompt

- Add left/right movement, Home/End, Delete, and insertion in the middle of a
  line.
- Add bounded command history with Up/Down search and optional persistence in
  `~/.rose_history`.
- Add Ctrl-A/E, Ctrl-U/K/W, Ctrl-L, and Ctrl-R or prefix history search.
- Add context-aware Tab completion for commands, paths, variables, and job IDs.
- Use a configurable prompt that shows the working directory and makes a
  failing previous status visible without becoming noisy.

### Command language

- Add `;`, `&&`, and `||` with a small syntax tree rather than extending the
  current pipeline structure ad hoc.
- Add pathname globbing and conventional field splitting with clear quoting
  rules.
- Add descriptor close redirection such as `2>&-`.
- Add command substitution only after nested parsing and output capture have
  dedicated tests.
- Add script files, comments, and a `source`/`.` builtin. Defer loops,
  functions, and full POSIX grammar until real scripts require them.

### Jobs and utilities

- Add `bg`, `kill`, current/previous job selection, and asynchronous Done or
  Stopped notifications before the next prompt.
- Add `export`, `unset`, `alias`, `unalias`, `type`, and `history` builtins.
- Add essential standalone tools in small groups: `cp`, `mv`, `touch`, `head`,
  `wc`, `find`, `ps`, `kill`, and `sleep`.
- Add a shell startup file such as `/etc/roserc` followed by `~/.roserc`.
- Move reusable string, parsing, and allocation helpers into a small userspace
  runtime as the command set grows.

**Exit criteria**

- A user can navigate, complete paths, edit and recall commands, manage a
  stopped/background job, and run a startup configuration entirely inside a
  graphical terminal.
- Shell parser, line editor, and job-control behavior have host unit tests plus
  PTY-level QEMU tests.
- Syntax errors identify the unexpected construct instead of reporting only a
  generic parse failure.

## Milestone 4: Dynamic desktop and window protocol

**Outcome:** The desktop manages an open-ended session. Applications request
windows at runtime and the user controls them with familiar desktop actions.

### Session and client protocol

- Split session startup from the compositor. A session manager owns autostart,
  application launch, crash handling, logout, restart, and shutdown policy.
- Replace hard-coded `create_window` calls with a versioned client protocol for
  create, configure, title, damage, focus, close, and destroy messages.
- Pass connection endpoints through inherited descriptors at spawn time. Keep
  shared memory for pixel buffers, but validate every control message.
- Define client behavior when the compositor disappears and compositor
  behavior when a client crashes or stops responding.

### Window management

- Support runtime creation of at least 12 windows.
- Add resize borders, minimum/maximum sizes, maximize/restore, minimize, and
  correct close-button press/release behavior.
- Add Alt-Tab, focused-window shortcuts, a task switcher, and keyboard-only
  access to all window actions.
- Add explicit focus, pointer enter/leave/motion/button/wheel, key, repeat,
  configure, and close events.
- Constrain windows to usable screen bounds and preserve their last geometry
  for the session.
- Add occlusion-aware damage clipping so moving one window does not require
  repainting the whole desktop area.

### Desktop shell

- Add an application launcher populated from small metadata files on disk.
- Add a panel showing running applications, focus state, and a clock.
- Add launch shortcuts for Terminal, Files, System Monitor, and Settings.
- Add desktop-level shutdown/reboot/logout actions with confirmation.
- Introduce a session-scoped clipboard service for text.

**Exit criteria**

- Applications can be launched after startup, create more than one window, and
  exit or crash without destabilizing the desktop.
- Windows can be moved, resized, minimized, maximized, closed, and switched
  entirely with either pointer or keyboard.
- Input is delivered only to the correct focused or captured client.

## Milestone 5: GUI toolkit and visual consistency

**Outcome:** Applications share behavior and appearance instead of each
drawing a custom collection of rectangles.

- Turn `user/gui.c` into a small layered library: transport/event loop,
  drawing, text/font resources, layout, widgets, and theme.
- Add clipping, alpha blending, image blitting, lines, borders, and rounded
  corners only where the design uses them.
- Add a retained widget tree with deterministic row/column layout and minimum
  sizes; avoid a large CSS-like system.
- Implement label, button, icon button, text field, checkbox, list, scrollbar,
  menu, dialog, tabs, and status bar.
- Standardize hover, pressed, disabled, focused, selected, and error states.
- Add keyboard focus traversal, activation, escape-to-dismiss, and visible
  focus indicators from the beginning.
- Load theme colors, fonts, icons, and application metadata from regular files
  so visual changes do not require rebuilding the kernel.
- Add a tiny resource compiler or image format only when hand-authored assets
  become a bottleneck.

**Exit criteria**

- Terminal, Files, System Monitor, launcher, and dialogs use the shared event
  loop, theme, font, and widgets.
- Every pointer action has a keyboard path and a visible state transition.
- A new simple application can open a window and present standard controls
  without copying compositor or rendering code.

## Milestone 6: Integrated applications and graphical boot

**Outcome:** ROSE boots into a self-contained desktop capable of common local
tasks, while retaining a reliable rescue shell.

### Files

- Add Back, Forward, Up, refresh, scrolling, selection, and keyboard navigation.
- Add create folder, rename, delete confirmation, copy, move, and properties.
- Open executable files in Terminal and text files in the editor through a
  central application association service.
- Add missing filesystem operations, especially atomic rename, as general VFS
  primitives rather than Files-only behavior.

### System applications

- Add a small text editor with open/save, selection, clipboard, undo, and an
  unsaved-changes dialog.
- Extend System Monitor with per-process information and terminate/kill actions;
  expose this through a structured process-information syscall or a small
  `/proc`-style filesystem.
- Add Settings for appearance, pointer/keyboard preferences, startup apps, and
  the default boot target.
- Add notification and standard open/save dialog services after the core
  toolkit is stable.

### Boot and recovery

- Let `/sbin/init` select a text or graphical target from configuration.
- Make the graphical target the default only after it can start a terminal,
  display a startup failure, and offer logout/shutdown reliably.
- Retain a serial rescue shell and a boot option that bypasses the graphical
  session.
- Have the session manager restart the compositor once after a crash, then fall
  back to a diagnostic text session with a useful error message.

**Exit criteria**

- A normal boot reaches the desktop without manually entering `desktop`.
- A user can launch apps, manage files, edit and save a text file, inspect and
  stop a process, customize basic settings, and shut down from the GUI.
- A broken GUI configuration or crashed compositor still leaves a usable rescue
  path.

## Milestone 7: Broader OS capabilities

These are valuable, but they should not block a convincing local shell and GUI:

- users, groups, ownership, and permission enforcement;
- asynchronous block I/O, a stronger filesystem profile, and crash recovery;
- networking, sockets, DNS, and network applications;
- audio, additional input devices, and hardware beyond QEMU `virt`;
- SMP, kernel worker threads, and larger-scale resource management;
- international text shaping, Unicode input, accessibility APIs, and
  multi-display support.

Treat each as its own project with explicit kernel abstractions and security
boundaries rather than folding it into desktop polish.

## Recommended implementation order

The shortest path to a noticeably better system is a set of vertical slices:

1. **Responsive terminal slice:** monotonic time, sleep, waitable PTY/input,
   ANSI clear/cursor/color support, and shell history/editing.
2. **Dynamic desktop slice:** client control channel, runtime Terminal launch,
   window resize, Alt-Tab, launcher, and lifecycle tests.
3. **Shared UI slice:** one font resource, event loop, button/list/scrollbar
   widgets, then migrate Files and System Monitor.
4. **Integrated session slice:** clipboard, file operations, editor, settings,
   session manager, and graphical boot with rescue fallback.

Do not begin with animation, wallpaper, complex icons, networking, or a full
POSIX shell. Those add breadth, but the event model, terminal semantics, window
protocol, and shared toolkit are what make every later feature feel solid.

## Release checklist for every milestone

- Clean build and all host, shell, platform, and graphics tests pass.
- Keyboard-only and pointer-only paths are exercised where applicable.
- Invalid client messages, stale shared-memory identifiers, and resource
  exhaustion fail without compromising the compositor or kernel.
- Closing or killing an application reclaims its processes, descriptors,
  mappings, and surfaces.
- The desktop remains responsive under rapid input and while another process
  produces continuous output.
- New user-visible behavior is documented in the README and shell `help`.
