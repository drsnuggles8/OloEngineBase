#!/usr/bin/env python3
"""Cross-tree link throttle for POSIX hosts. Wraps ONE linker invocation.

The POSIX half of `.claude/skills/run-oloengine/link-semaphore.ps1`, wired by
cmake/LinkSemaphore.cmake. It exists because on Linux **nothing bounds linking at
all** (issue #1313), and that is not a theoretical gap:

    heaviest ld.lld link   11.17 GiB (Debug+ASan) / 10.03 GiB (plain Debug)
    heaviest compile        8.09 GiB              /  4.65 GiB
    runner unit cap        14 GiB  -- one link is 80% of it
    account slice cap      19 GiB  -- shared by BOTH olo-ci runner slots

Measured by .github/workflows/build-memory.yml on the box, 1761 compiles at 99.9%
coverage. The link is the single largest memory consumer in the build, larger than
any translation unit, and it is 10 GiB even with no sanitizer.

Three things that should have bounded it do not:

  * `OLO_LINK_JOBS` (the `olo_link` Ninja job pool) is guarded by
    `if(CMAKE_GENERATOR MATCHES "Ninja")`. Every Linux job configures with no -G and
    gets Unix Makefiles, where the pool silently does not exist.
  * `olo_heavy` is a Ninja pool too, and throttles compiles rather than links.
  * `cmake/LinkSemaphore.cmake` shells out to `pwsh`, which is not installed on the
    box. It says so and fails open -- confirmed in the runner's own log:
        Link semaphore: neither 'pwsh' nor 'powershell' found on PATH -- linking
        WITHOUT cross-tree throttling.

So `make -j2` was free to run that 11.17 GiB link beside a compile (19.26 GiB with
the heaviest one, past the unit cap and at the slice cap), and two concurrent links
across the two runner slots is 22.3 GiB against a 19 GiB slice. In practice the
OloEngine-Tests link lands at the end of the build when little else is left, which
is why this has not been failing constantly -- luck of scheduling, not a guarantee.

WHY flock, AND WHY THE WINDOWS SCRIPT'S REASONING TRANSFERS. Its header explains at
length why it uses N named MUTEXES rather than one counting semaphore: a Windows
semaphore's count is owned by nobody, so a holder that is killed never gives its
permit back, and the throttle silently degrades to nothing. Only a mutex has an
owner, so an abandoned one is handed to the next waiter rather than leaked.

`flock()` has exactly that ownership property, and gets it from the kernel: the lock
belongs to the open file description, so when the holder dies -- for any reason, by
any signal -- the descriptor closes and the lock is released. That is why this is a
handful of lines where the Windows version needs a careful argument, and it is also
why a PID-file scheme is not used here: it would have to answer "is the holder still
alive?", and every wrong answer either steals a permit or leaks one forever.

FAILS OPEN, ALWAYS, for the same reason the Windows script does. If the lock
directory cannot be made, if fcntl is unavailable, if every permit is busy past the
timeout -- the link runs anyway. Unthrottled means "slower, maybe tight on memory".
A stuck throttle means no build on this machine ever finishes again, which is
strictly worse than the problem being solved.

CONFIGURATION COMES FROM THE ENVIRONMENT, not from argv, and that is deliberate:
a linker command line is full of tokens an argument parser would try to interpret
(`-o`, `--start-group`, a bare `--`). Everything in argv after this script's own path
is the command, forwarded verbatim. The Windows script has no param() block for the
same reason.

    OLO_LINK_SEMAPHORE_SLOTS     permits; default 2, <=0 disables the throttle
    OLO_LINK_SEMAPHORE_TIMEOUT   seconds to wait before linking anyway; default 1800
    OLO_LINK_SEMAPHORE_DIR       where the permit files live; default <tmp>/olo-link-semaphore

The permit directory must be SHARED by everything that should contend. Both olo-ci
runner slots run as the same user on one machine, so the default temp directory is
the right scope: per-machine, not per-workspace.
"""

from __future__ import annotations

import math
import os
import subprocess
import sys
import tempfile
import time

try:
    import fcntl  # POSIX only; absent on Windows, which uses the .ps1 wrapper instead.
except ImportError:  # pragma: no cover - exercised on Windows
    fcntl = None

TAG = "[link-semaphore]"
DEFAULT_SLOTS = 2
DEFAULT_TIMEOUT = 1800.0
POLL_SECONDS = 0.25


def note(message: str) -> None:
    """Progress goes to stderr so it cannot be mistaken for linker output."""
    print(f"{TAG} {message}", file=sys.stderr, flush=True)


def env_number(name: str, default: float) -> float:
    """A malformed value must not fail the link — it degrades to the default.

    NaN and infinity are rejected alongside the unparseable ones, and that is not
    tidiness: `float("nan")` parses happily, and a NaN timeout makes every
    `time.monotonic() >= deadline` comparison False, so the acquire loop never reaches
    its deadline and waits forever. `float("inf")` does the same by construction. Either
    one turns the fail-OPEN guarantee — the whole reason a stuck throttle cannot wedge
    the machine — into a hang, from nothing worse than a typo in the environment.
    """
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        value = float(raw)
    except ValueError:
        note(f"{name}={raw!r} is not a number — using {default}")
        return default
    if not math.isfinite(value):
        note(f"{name}={raw!r} is not finite — using {default}")
        return default
    return value


class Permit:
    """One held permit, released when the process exits or this is closed.

    Nothing here tries to release on a crash path: that is the kernel's job. The
    lock lives on the open file description, so it goes away with the process no
    matter how the process goes away.
    """

    def __init__(self) -> None:
        self.fd: int | None = None
        self.index: int | None = None

    def acquire(self, directory: str, slots: int, timeout: float, activity: str = "linking") -> bool:
        # `activity` only words the notes below: heavy-compile-semaphore.py reuses this
        # class for compiles and passes "compiling".
        if fcntl is None:
            note(f"fcntl unavailable (not a POSIX host) — {activity} unthrottled")
            return False
        try:
            os.makedirs(directory, exist_ok=True)
        except OSError as exc:
            note(f"cannot create permit directory {directory!r} ({exc}) — {activity} unthrottled")
            return False

        deadline = time.monotonic() + timeout
        announced_wait = False
        while True:
            for index in range(slots):
                path = os.path.join(directory, f"slot{index}")
                try:
                    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o666)
                except OSError as exc:
                    note(f"cannot open permit {path!r} ({exc}) — {activity} unthrottled")
                    return False
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError:
                    os.close(fd)
                    continue  # Held by someone else; try the next permit.
                except Exception as exc:  # pragma: no cover - defensive
                    os.close(fd)
                    note(f"flock failed ({exc}) — {activity} unthrottled")
                    return False
                self.fd = fd
                self.index = index
                return True

            if time.monotonic() >= deadline:
                note(f"timed out after {timeout:g}s waiting for a permit — {activity} anyway (fail-open)")
                return False
            if not announced_wait:
                note(f"all {slots} permits busy — waiting")
                announced_wait = True
            time.sleep(POLL_SECONDS)

    def close(self) -> None:
        if self.fd is not None:
            try:
                os.close(self.fd)  # Releases the flock with the descriptor.
            except OSError:
                pass
            self.fd = None


def run(command: list[str]) -> int:
    """Run the link and return an exit status that distinguishes every outcome.

    The Windows script needs a sentinel to tell "the linker ran and returned N" from
    "the linker never started", because a failed launch leaves $LASTEXITCODE holding
    whatever it had — possibly 0, reporting success for a link that never happened.
    Python raises instead, so the distinction is structural here; what still needs
    care is not collapsing a real status into a boolean, and not losing a signal.
    """
    try:
        status = subprocess.call(command)
    except FileNotFoundError:
        note(f"failed to launch {command[0]!r}: not found")
        return 1
    except OSError as exc:
        note(f"failed to launch {command[0]!r}: {exc}")
        return 1
    if status < 0:
        # Killed by a signal. Report it the way a shell does rather than as a
        # negative number, which an exit status cannot carry: a link killed by the
        # OOM killer must not look like success or like an ordinary linker error.
        signal_number = -status
        note(f"{command[0]!r} was killed by signal {signal_number}")
        return 128 + signal_number
    return status


def main(argv: list[str]) -> int:
    command = argv[1:]
    if not command:
        note("no command given; this is a CMake linker launcher, not a standalone tool")
        return 2

    slots = int(env_number("OLO_LINK_SEMAPHORE_SLOTS", DEFAULT_SLOTS))
    if slots <= 0:
        # Deliberate opt-out, not a failure: say so, because a silently absent
        # throttle is the state issue #1313 is about.
        note("OLO_LINK_SEMAPHORE_SLOTS <= 0 — throttle disabled, linking unthrottled")
        return run(command)

    directory = os.environ.get("OLO_LINK_SEMAPHORE_DIR") or os.path.join(
        tempfile.gettempdir(), "olo-link-semaphore"
    )
    timeout = env_number("OLO_LINK_SEMAPHORE_TIMEOUT", DEFAULT_TIMEOUT)

    permit = Permit()
    try:
        permit.acquire(directory, slots, timeout)
        return run(command)
    finally:
        permit.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
