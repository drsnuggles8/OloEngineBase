#!/usr/bin/env python3
"""Bound concurrent compiles of the memory-heavy TUs, and record every compile's peak RSS.

A CMake compiler launcher, wired by cmake/HeavyCompileSemaphore.cmake. It does for the
heavy TU set what the `olo_heavy` Ninja pool does, on the trees where that pool does not
exist: CMake older than 4.4 (the pool needs a per-file-set JOB_POOL_COMPILE) or a
generator other than Ninja.

WHY IT EXISTS (issue #1473). gpu-conformance-amd.yml was OOM-killed in "Build tests"
every night from 2026-09-09 on. The runner's CMake is the distro's 3.31.8, so the pool
was never created, and Ninja scheduled the heavy TUs next to each other because they sit
next to each other in the source list. The kernel's OOM report for 2026-09-25 lists what
was resident in the job's own 14 GiB runner unit when it fired:

    cc1plus x 6   SceneSerializer.cpp + five LuaScriptGlue*.cpp parts
    rss_anon      2.08 - 2.41 GiB each, 13.66 GiB together, plus ~0.85 GiB in swap
    constraint    CONSTRAINT_MEMCG on the job's OWN unit, not the shared slice

Two heavy compiles at a time is what the pool allows (OLO_HEAVY_COMPILE_JOBS, default 2).

USAGE, as CMake builds it:

    heavy-compile-semaphore.py --slots=N --manifest=FILE [--rss-log=FILE] -- <compile...>
    heavy-compile-semaphore.py --report=FILE [--top=N] [--cap-gib=G]

The manifest lists one absolute source path per line: the TUs every CMakeLists hands to
olo_bind_heavy_compile_pool(). A compile whose `-c` source is in it takes one of N flock
permits first; every other compile runs at once. Permits live in a per-machine directory,
like the link semaphore's, and the kernel releases a permit when its holder dies.

FAILS OPEN, like scripts/link-semaphore.py and for the same reason: a throttle that can
fail or wedge a build is worse than the memory pressure it bounds. A malformed option, a
missing manifest or a busy-past-the-timeout permit runs the compile anyway, and says so.

THE RSS LOG is the GCC counterpart of clang's -fproc-stat-report, which GCC does not have
(docs/agent-rules/build-memory-per-tu.md). getrusage(RUSAGE_CHILDREN).ru_maxrss after the
compile returns is the peak RSS of the largest process in its tree, which is cc1plus on a
cache miss and ccache itself (tens of MiB) on a hit. `--report` turns the log into a
ranking plus the largest sum of peaks among compiles that overlapped in time, an upper
bound on what the build held at once.
"""

from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import time

try:
    import resource  # POSIX only.
except ImportError:  # pragma: no cover - exercised on Windows
    resource = None

TAG = "[heavy-compile]"
# Longer than a cold heavy queue can last in one build (17 TUs, two at a time, a few
# minutes each), so the fail-open release never fires mid-build: releasing every queued
# heavy compile at once is the OOM this exists to prevent. It is a backstop for a
# permit leaked by something outside this script, not a scheduling knob.
DEFAULT_TIMEOUT = 7200.0
GIB = 1024.0 * 1024.0  # ru_maxrss and the log are in KiB.


def note(message: str) -> None:
    print(f"{TAG} {message}", file=sys.stderr, flush=True)


def load_link_semaphore():
    """One Permit implementation for links and compiles: import the sibling script."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "link-semaphore.py")
    spec = importlib.util.spec_from_file_location("olo_link_semaphore", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.TAG = TAG  # Its notes then read as this tool's, not as a link's.
    return module


def parse_options(argv: list[str]) -> tuple[dict[str, str], list[str]]:
    """Options up to a bare `--`, the compile command after it."""
    options: dict[str, str] = {}
    for index, arg in enumerate(argv):
        if arg == "--":
            return options, argv[index + 1 :]
        if not arg.startswith("--") or "=" not in arg:
            raise ValueError(f"unexpected argument {arg!r}")
        key, value = arg[2:].split("=", 1)
        options[key] = value
    return options, []


def canonical(path: str) -> str:
    """Absolute against the build tool's cwd, symlinks resolved: CMake's spelling and the
    compile line's must meet, or the TU goes unthrottled and the report says so."""
    return os.path.realpath(os.path.abspath(path))


def compiled_source(command: list[str]) -> str | None:
    for index, arg in enumerate(command[:-1]):
        if arg == "-c":
            return canonical(command[index + 1])
    return None


def read_manifest(path: str) -> set[str]:
    with open(path, encoding="utf-8") as manifest:
        return {canonical(line.strip()) for line in manifest if line.strip()}


def append_record(path: str, fields: list[object]) -> None:
    line = ("\t".join(str(f) for f in fields) + "\n").encode("utf-8")
    try:
        # One O_APPEND write per record, so concurrent compiles cannot interleave a line.
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
        try:
            os.write(fd, line)
        finally:
            os.close(fd)
    except OSError as exc:
        note(f"cannot append to {path!r} ({exc}) — record dropped")


def launch(options: dict[str, str], command: list[str]) -> int:
    linksem = load_link_semaphore()
    source = compiled_source(command)

    heavy = False
    manifest_path = options.get("manifest", "")
    if source is not None:
        try:
            heavy = source in read_manifest(manifest_path)
        except OSError as exc:
            note(f"cannot read manifest {manifest_path!r} ({exc}) — compiling unthrottled")

    permit = linksem.Permit()
    queued = time.time()
    try:
        if heavy:
            try:
                slots = int(options.get("slots", "2"))
            except ValueError:
                note(f"--slots={options.get('slots')!r} is not an integer — using 2")
                slots = 2
            directory = os.environ.get("OLO_HEAVY_COMPILE_SEMAPHORE_DIR") or os.path.join(
                tempfile.gettempdir(), "olo-heavy-compile-semaphore"
            )
            timeout = linksem.env_number("OLO_HEAVY_COMPILE_SEMAPHORE_TIMEOUT", DEFAULT_TIMEOUT)
            if slots > 0:
                permit.acquire(directory, slots, timeout, activity="compiling")
        started = time.time()
        status = linksem.run(command)
        ended = time.time()
    finally:
        permit.close()

    peak_kib = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss if resource else 0
    if heavy:
        note(
            f"{os.path.basename(source or '?')}: waited {started - queued:.0f}s, "
            f"ran {ended - started:.0f}s, peak {peak_kib / GIB:.2f} GiB"
        )
    rss_log = options.get("rss-log")
    if rss_log:
        append_record(
            rss_log,
            [f"{started:.3f}", f"{ended:.3f}", f"{started - queued:.1f}", peak_kib, int(heavy), status, source or "?"],
        )
    return status


def short(source: str) -> str:
    """Relative to the working directory when the source is under it, as it is in CI."""
    relative = os.path.relpath(source) if os.path.isabs(source) else source
    return source if relative.startswith("..") else relative


def report(path: str, top: int, cap_gib: float) -> int:
    """Rank the log, and find the largest peak sum among compiles that overlapped."""
    records = []
    with open(path, encoding="utf-8") as log:
        for line in log:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 7:
                continue
            start, end, wait, peak, heavy, status, source = parts
            records.append((float(start), float(end), float(wait), int(peak), heavy == "1", int(status), source))
    if not records:
        print(f"no compile records in {path}")
        return 1

    # Sweep start/end events: at each start, the running set's peak sum and heavy count.
    events = sorted([(r[0], 1, i) for i, r in enumerate(records)] + [(r[1], 0, i) for i, r in enumerate(records)])
    running: set[int] = set()
    worst_sum, worst_set, worst_heavy = 0, [], 0
    for _, kind, index in events:
        if kind == 0:
            running.discard(index)
            continue
        running.add(index)
        total = sum(records[i][3] for i in running)
        worst_heavy = max(worst_heavy, sum(1 for i in running if records[i][4]))
        if total > worst_sum:
            worst_sum, worst_set = total, sorted(running, key=lambda i: -records[i][3])

    heavy = [r for r in records if r[4]]
    print(f"{len(records)} compiles recorded, {len(heavy)} of them in the heavy set")
    if not heavy:
        # Either the build stopped before reaching them, or the manifest's paths and the
        # compile lines' never matched and the semaphore bounded nothing. Say it loudly.
        print("::warning::no recorded compile matched the heavy-TU manifest: either the build "
              "stopped before the heavy TUs, or the semaphore bounded nothing")
    print(f"most heavy compiles running at once: {worst_heavy}")
    print(
        f"largest sum of peak RSS among overlapping compiles: {worst_sum / GIB:.2f} GiB "
        f"(an upper bound on what they held at once; runner unit cap {cap_gib:g} GiB)"
    )
    for i in worst_set:
        print(f"    {records[i][3] / GIB:6.2f} GiB  {os.path.basename(records[i][6])}")
    print(f"\ntop {top} compiles by peak RSS:")
    print("  peak GiB   wall s   wait s  heavy  source")
    for r in sorted(records, key=lambda r: -r[3])[:top]:
        print(f"  {r[3] / GIB:8.2f} {r[1] - r[0]:8.0f} {r[2]:8.0f}  {'yes' if r[4] else '   '}    {short(r[6])}")
    return 0


def main(argv: list[str]) -> int:
    try:
        options, command = parse_options(argv[1:])
    except ValueError as exc:
        # Fail open: run the compile unthrottled rather than drop it. It starts after the
        # `--` if there is one, else after the leading run of this script's own options.
        note(f"{exc} — compiling unthrottled")
        rest = argv[1:]
        if "--" in rest:
            rest = rest[rest.index("--") + 1 :]
        else:
            while rest and rest[0].startswith("--"):
                rest = rest[1:]
        return load_link_semaphore().run(rest)

    if "report" in options:
        return report(options["report"], int(options.get("top", "20")), float(options.get("cap-gib", "14")))
    if not command:
        note("no command given; this is a CMake compiler launcher (or pass --report=FILE)")
        return 2
    return launch(options, command)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
