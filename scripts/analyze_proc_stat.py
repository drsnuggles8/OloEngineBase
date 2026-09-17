#!/usr/bin/env python3
"""Rank a build's compiles and links by PEAK RSS, and derive a safe -j width from it.

Reads the CSV records clang's ``-fproc-stat-report=<file>`` appends, which
``cmake/ProcStatReport.cmake`` wires up behind ``OLO_BUILD_INSTRUMENTATION``
(issue #1305). Each record is one driver SUBPROCESS:

    "clang++","CMakeFiles/OloEngine.dir/src/.../Foo.cpp.o",203125,187500,132552
     tool      output file                                  wall   user    peak KiB
                                                            (microseconds)

Why this script exists rather than a `sort -t, -k5`: the ranking on its own does
not answer the question the issue asks. ``--parallel`` and the cgroup caps are
bounded by the SUM of the N heaviest concurrent compiles, not by the mean and not
by a single worst TU, so the derivation table at the bottom is the actual
deliverable. And the records need three kinds of care that a sort does not give:
ccache's preprocessor passes have to be classified out of the compile ranking
without being silently dropped, the file is APPENDED to across builds so an output
can appear more than once, and a warm compiler cache records nothing at all for a
cache hit — which makes a partial run look exactly like a cheap one unless the
coverage is stated.

Usage:
    python scripts/analyze_proc_stat.py build-cached/olo-proc-stat.csv
    python scripts/analyze_proc_stat.py build/olo-proc-stat.csv --top 20 \
        --cap-gib 14 --json report.json --markdown report.md
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import defaultdict

# The record is emitted by clang's Job.cpp with llvm::format, so the shape is fixed:
# two quoted fields then three unsigned integers. Both the separator spacing and the
# backslash escaping in Windows paths have varied across clang versions, so parse
# tolerantly (optional spaces) rather than splitting on "," and trusting the count.
RECORD_RE = re.compile(
    r'^\s*"(?P<tool>[^"]*)"\s*,\s*"(?P<output>[^"]*)"\s*,\s*'
    r"(?P<wall_us>\d+)\s*,\s*(?P<user_us>\d+)\s*,\s*(?P<peak_kib>\d+)\s*$"
)

OBJECT_SUFFIXES = (".o", ".obj")
# A preprocessor or intermediate output, NOT a translation unit's real compile. ccache
# in preprocessor mode runs `-E` into its own tmp dir, and that pass carries the same
# flag, so a single cache MISS appends TWO records. Counted and reported separately —
# folding them into the compile ranking would understate nothing but would inflate the
# record count, and dropping them silently would hide that ccache changed modes.
INTERMEDIATE_SUFFIXES = (".ii", ".i", ".s", ".bc", ".tmp", ".d")
# CMake's object path for target T and source S is CMakeFiles/T.dir/<S>.<o|obj>
# (multi-config generators insert the config: CMakeFiles/T.dir/<Config>/<S>.<o|obj>).
OBJECT_PATH_RE = re.compile(
    r"CMakeFiles[\\/](?P<target>[^\\/]+)\.dir[\\/](?P<rest>.+)$"
)

KIB_PER_GIB = 1024 * 1024


class Record:
    __slots__ = ("tool", "output", "wall_us", "user_us", "peak_kib", "kind")

    def __init__(self, tool: str, output: str, wall_us: int, user_us: int, peak_kib: int):
        self.tool = tool
        # Windows paths arrive with escaped separators; normalise to forward slashes so
        # the same TU compares equal whichever host produced the records.
        self.output = output.replace("\\\\", "/").replace("\\", "/")
        self.wall_us = wall_us
        self.user_us = user_us
        self.peak_kib = peak_kib
        self.kind = classify(self.output)

    @property
    def peak_gib(self) -> float:
        return self.peak_kib / KIB_PER_GIB

    @property
    def wall_s(self) -> float:
        return self.wall_us / 1_000_000


def classify(output: str) -> str:
    """One of 'compile', 'link', 'intermediate'."""
    lowered = output.lower()
    # Order matters: ccache writes its preprocessor output as <hash>.ii inside its own
    # tmp dir, and an intermediate check must win over the extension-less 'link' guess.
    if lowered.endswith(INTERMEDIATE_SUFFIXES) or "/ccache/tmp/" in lowered or "/tmp/cpp_stdout" in lowered:
        return "intermediate"
    if lowered.endswith(OBJECT_SUFFIXES):
        return "compile"
    # clang writes "-" for output on stdout (e.g. a bare -E), which is not a link.
    if output == "-" or output == "":
        return "intermediate"
    return "link"


def describe(output: str) -> str:
    """A readable 'target: source' label for an object path, or the path itself."""
    match = OBJECT_PATH_RE.search(output)
    if not match:
        return output
    target = match.group("target")
    rest = match.group("rest")
    # Strip the object suffix CMake appended to the source name.
    for suffix in OBJECT_SUFFIXES:
        if rest.lower().endswith(suffix):
            rest = rest[: -len(suffix)]
            break
    return f"{target}: {rest}"


def collect_record_files(paths: list[str], name: str) -> list[str]:
    """Expand the given paths into the list of records files to read.

    A directory is searched RECURSIVELY, and that is not a convenience — it is
    required for correctness on the Linux CI generator. The flag's path must stay
    relative (see cmake/ProcStatReport.cmake for the ccache measurement that forces
    it), and clang resolves a relative path against the build tool's working
    directory — which is NOT the same directory for every generator:

        Ninja           runs every command from the top build dir
                        -> ONE build/olo-proc-stat.csv
        Unix Makefiles  `cd`s into each target's own subdirectory build dir first
                        -> build/olo-proc-stat.csv, build/OloEngine/olo-proc-stat.csv,
                           build/OloEngine/tests/olo-proc-stat.csv, ...

    Verified by generating both and reading the emitted compile rule; the Makefiles
    rule is literally `cd <build>/sub && clang++ ... -c ...`. Every Linux job in this
    repo configures with no -G and therefore gets Unix Makefiles, so a reader that
    only opened the top-level file would silently rank a fraction of the build — and
    a fraction that happens to EXCLUDE the engine and test subdirectories, which is
    where the heavy TUs live.
    """
    found: list[str] = []
    for path in paths:
        if os.path.isdir(path):
            for root, _dirs, files in os.walk(path):
                if name in files:
                    found.append(os.path.join(root, name))
        else:
            found.append(path)
    return found


def parse(path: str) -> tuple[list[Record], int]:
    """Returns (records, unparsable_line_count).

    Unparsable lines are counted rather than ignored: several clang processes append
    to this file concurrently, and although each writes a single short line, a torn
    write is the one failure mode that would silently bias a ranking downward. If the
    count is ever non-zero the report says so instead of looking clean.
    """
    records: list[Record] = []
    malformed = 0
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not line.strip():
                continue
            match = RECORD_RE.match(line)
            if not match:
                malformed += 1
                continue
            records.append(
                Record(
                    match.group("tool"),
                    match.group("output"),
                    int(match.group("wall_us")),
                    int(match.group("user_us")),
                    int(match.group("peak_kib")),
                )
            )
    return records, malformed


def collapse(records: list[Record]) -> list[Record]:
    """One entry per output, holding its MAXIMUM observed peak RSS.

    The records file is appended to and never truncated, and the records carry no
    timestamp, so an incremental build on top of an earlier one leaves several
    records for the same object. Max is the right reduction for a memory ceiling —
    and it is also why CI deletes the file before building, so a published ranking
    describes exactly one build.
    """
    best: dict[str, Record] = {}
    for record in records:
        existing = best.get(record.output)
        if existing is None or record.peak_kib > existing.peak_kib:
            best[record.output] = record
    return sorted(best.values(), key=lambda r: r.peak_kib, reverse=True)


def expected_compiles(build_dir: str | None, explicit: int | None) -> tuple[int | None, str]:
    """How many compiles this build PRODUCED, for the coverage line.

    Returns (count, provenance). None when it cannot be established — stated as
    unknown rather than guessed, because the coverage percentage is the one number
    that tells a reader whether the ranking is complete.

    The denominator is the OBJECT FILES sitting in the build tree, deliberately NOT
    ``compile_commands.json``: that file describes every TU in the PROJECT, while a
    CI job builds one target (``--target OloEngine-Tests``), so comparing against it
    would report a large, entirely legitimate shortfall and train a reader to ignore
    the one line that distinguishes a partial ranking from a complete one. Objects on
    disk are what the build actually compiled, whether from the compiler or from the
    cache — which is exactly the comparison that exposes a warm cache.
    """
    if explicit is not None:
        return explicit, "--expected"
    if build_dir is None:
        return None, "no --build-dir given"
    if not os.path.isdir(build_dir):
        return None, f"'{build_dir}' is not a directory"
    count = 0
    for _root, _dirs, files in os.walk(build_dir):
        for name in files:
            if name.lower().endswith(OBJECT_SUFFIXES):
                count += 1
    return count, f"object files under {build_dir}"


def derive_parallel(compiles: list[Record], cap_gib: float, max_lanes: int) -> list[dict]:
    """The worst-case cost of N concurrent compiles, for N = 1..max_lanes.

    The bound is the SUM of the N heaviest TUs, not N x mean: a build scheduler is
    free to start the heavy ones together, and the whole point of the olo_heavy pool
    is that it usually will not. So this is the pessimistic ceiling a cgroup cap has
    to survive, which is the number a cap should be set from.
    """
    rows = []
    running = 0
    for lanes in range(1, max_lanes + 1):
        if lanes <= len(compiles):
            running += compiles[lanes - 1].peak_kib
        worst_gib = running / KIB_PER_GIB
        rows.append(
            {
                "lanes": lanes,
                "worst_case_gib": round(worst_gib, 2),
                "fits": worst_gib <= cap_gib if cap_gib > 0 else None,
            }
        )
    return rows


def render(report: dict, top: int) -> str:
    """The Markdown report — also what lands in the CI job summary."""
    out: list[str] = []
    add = out.append
    totals = report["totals"]

    add("## Build memory: per-invocation peak RSS")
    add("")
    files = report["records_files"]
    if len(files) == 1:
        add(f"Records file: `{files[0]}`")
    else:
        # Say how many rather than listing dozens: on the Makefiles generator there is
        # one per subdirectory, and the count is the useful signal (a count of 1 on a
        # Linux build would mean the gather missed the subdirectories).
        add(f"Records files: **{len(files)}** (one per build subdirectory, Makefiles generator)")
    add("")
    add("| | count | max peak RSS | sum of peaks |")
    add("|---|---:|---:|---:|")
    for kind in ("compile", "link", "intermediate"):
        group = totals[kind]
        add(
            f"| {kind} | {group['count']} | {group['max_gib']:.2f} GiB | {group['sum_gib']:.2f} GiB |"
        )
    add("")

    coverage = report["coverage"]
    if coverage["expected"] is None:
        add(
            f"**Coverage: {coverage['recorded']} compiles recorded; the build's total TU count could "
            f"not be established ({coverage['provenance']}).** A ranking from a build with a warm "
            "compiler cache is PARTIAL by construction — a cache hit runs no compiler and so "
            "records nothing. Treat this as a lower bound unless the cache was cold."
        )
    else:
        add(
            f"**Coverage: {coverage['recorded']} of {coverage['expected']} compiles recorded "
            f"({coverage['percent']:.1f}%, from `{coverage['provenance']}`).**"
        )
        if coverage["percent"] < 95.0:
            add("")
            add(
                "> Under 95% — the missing TUs were compiler-cache hits (a hit runs no compiler, "
                "so it appends no record) or were not built by this target selection. The ranking "
                "is a LOWER BOUND on the build's peak, not a complete census."
            )
    add("")

    if report["malformed_lines"]:
        add(
            f"> **{report['malformed_lines']} unparsable line(s)** in the records file — concurrent "
            "appends can tear. The ranking is missing those invocations."
        )
        add("")

    add(f"### Top {top} compiles by peak RSS")
    add("")
    add("| # | peak RSS | wall | target: source |")
    add("|---:|---:|---:|---|")
    for index, entry in enumerate(report["compiles"][:top], start=1):
        add(
            f"| {index} | **{entry['peak_gib']:.2f} GiB** | {entry['wall_s']:.1f} s | "
            f"`{entry['label']}` |"
        )
    add("")

    if report["links"]:
        add("### Link steps by peak RSS")
        add("")
        add("| peak RSS | wall | tool | output |")
        add("|---:|---:|---|---|")
        for entry in report["links"]:
            add(
                f"| **{entry['peak_gib']:.2f} GiB** | {entry['wall_s']:.1f} s | "
                f"`{entry['tool']}` | `{entry['output']}` |"
            )
        add("")

    derivation = report["derivation"]
    add("### Worst-case concurrent compile cost")
    add("")
    add(
        f"The N heaviest TUs summed — the pessimistic ceiling `--parallel N` has to survive, "
        f"against a cap of **{derivation['cap_gib']:.0f} GiB**."
    )
    add("")
    add("| --parallel N | worst-case peak | fits the cap |")
    add("|---:|---:|:---:|")
    for row in derivation["rows"]:
        verdict = "—" if row["fits"] is None else ("yes" if row["fits"] else "**NO**")
        add(f"| {row['lanes']} | {row['worst_case_gib']:.2f} GiB | {verdict} |")
    add("")
    if derivation["largest_fitting_lanes"] is not None:
        add(
            f"**Largest N whose worst case fits {derivation['cap_gib']:.0f} GiB: "
            f"{derivation['largest_fitting_lanes']}.** Compiles only — a link can run "
            "concurrently with compiles, and the heaviest link measured here is "
            f"{totals['link']['max_gib']:.2f} GiB."
        )
        add("")

    return "\n".join(out) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "records",
        nargs="+",
        help="records files, and/or build trees to search recursively for them. Pass the BUILD "
        "TREE rather than a single file: the Unix Makefiles generator (every Linux CI job) "
        "writes one file per subdirectory — see collect_record_files().",
    )
    parser.add_argument(
        "--name",
        default="olo-proc-stat.csv",
        help="records file name to look for inside a directory argument (default olo-proc-stat.csv; "
        "match OLO_PROC_STAT_FILE if it was overridden)",
    )
    parser.add_argument("--top", type=int, default=20, help="how many compiles to rank (default 20)")
    parser.add_argument(
        "--cap-gib",
        type=float,
        default=14.0,
        help="memory cap to derive a safe -j against, in GiB (default 14 — the olo-ci runner unit's MemoryMax)",
    )
    parser.add_argument("--max-lanes", type=int, default=16, help="highest -j to tabulate (default 16)")
    parser.add_argument(
        "--build-dir",
        help="build tree to count object files in, for the coverage denominator "
        "(default: the records file's own directory, which is where the build tool ran)",
    )
    parser.add_argument(
        "--expected",
        type=int,
        help="override the coverage denominator with an exact TU count",
    )
    parser.add_argument(
        "--fail-under-coverage",
        type=float,
        default=0.0,
        help="exit non-zero when coverage is below this percentage. Use it in CI: an artifact "
        "published from a warm cache is a PARTIAL ranking, and a partial ranking that nobody "
        "notices is how this measurement decayed the first time (issue #1305).",
    )
    parser.add_argument("--json", dest="json_out", help="also write the full report as JSON here")
    parser.add_argument("--markdown", dest="markdown_out", help="also write the Markdown report here")
    args = parser.parse_args(argv)

    missing = [p for p in args.records if not os.path.exists(p)]
    if missing:
        print(f"error: no such path: {', '.join(missing)}", file=sys.stderr)
        return 2

    record_files = collect_record_files(args.records, args.name)
    if not record_files:
        # A loud, actionable failure rather than an empty report: the overwhelmingly
        # likely cause is a build configured without the option, and an empty ranking
        # would read as "nothing used much memory".
        print(
            f"error: found no '{args.name}' under {', '.join(args.records)}.\n"
            "The build was probably configured without -DOLO_BUILD_INSTRUMENTATION=ON, or "
            "ran under a non-clang toolchain (the flag is clang-only — see "
            "cmake/ProcStatReport.cmake).",
            file=sys.stderr,
        )
        return 2

    # With several records files the outputs inside them are each relative to their OWN
    # directory, so they must be re-rooted before being compared or de-duplicated —
    # otherwise two different targets' objects could share a key and one would silently
    # overwrite the other in collapse(). Re-rooting against the common build tree also
    # makes the ranking readable: the label then says which subdirectory a TU came from.
    common_root = os.path.commonpath([os.path.dirname(os.path.abspath(p)) for p in record_files])
    records: list[Record] = []
    malformed = 0
    for path in sorted(record_files):
        file_records, file_malformed = parse(path)
        prefix = os.path.relpath(os.path.dirname(os.path.abspath(path)), common_root).replace(os.sep, "/")
        if prefix != ".":
            for record in file_records:
                record.output = f"{prefix}/{record.output}"
        records.extend(file_records)
        malformed += file_malformed
    if not records:
        print(
            f"error: the {len(record_files)} records file(s) hold no parseable records "
            f"({malformed} unparsable line(s)).",
            file=sys.stderr,
        )
        return 2

    collapsed = collapse(records)
    by_kind: dict[str, list[Record]] = defaultdict(list)
    for record in collapsed:
        by_kind[record.kind].append(record)

    compiles = by_kind["compile"]
    links = by_kind["link"]

    build_dir = args.build_dir or common_root
    expected, provenance = expected_compiles(build_dir, args.expected)
    rows = derive_parallel(compiles, args.cap_gib, args.max_lanes)
    fitting = [row["lanes"] for row in rows if row["fits"]]

    report = {
        "records_files": sorted(os.path.abspath(p) for p in record_files),
        "malformed_lines": malformed,
        "totals": {
            kind: {
                "count": len(by_kind[kind]),
                "max_gib": max((r.peak_gib for r in by_kind[kind]), default=0.0),
                "sum_gib": sum(r.peak_gib for r in by_kind[kind]),
            }
            for kind in ("compile", "link", "intermediate")
        },
        "coverage": {
            "recorded": len(compiles),
            "expected": expected,
            "provenance": provenance,
            "percent": (100.0 * len(compiles) / expected) if expected else 0.0,
        },
        "compiles": [
            {
                "peak_gib": r.peak_gib,
                "peak_kib": r.peak_kib,
                "wall_s": r.wall_s,
                "output": r.output,
                "label": describe(r.output),
                "tool": r.tool,
            }
            for r in compiles
        ],
        "links": [
            {
                "peak_gib": r.peak_gib,
                "peak_kib": r.peak_kib,
                "wall_s": r.wall_s,
                "output": r.output,
                "tool": r.tool,
            }
            for r in links
        ],
        "derivation": {
            "cap_gib": args.cap_gib,
            "rows": rows,
            "largest_fitting_lanes": max(fitting) if fitting else None,
        },
    }

    markdown = render(report, args.top)
    sys.stdout.write(markdown)

    if args.markdown_out:
        with open(args.markdown_out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(markdown)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(report, handle, indent=2)
            handle.write("\n")

    # The coverage gate is the anti-decay mechanism, so it fails LOUDLY on an
    # unestablished denominator too rather than passing on a missing check — a report
    # that cannot state its own completeness is the state this issue exists to end.
    if args.fail_under_coverage > 0.0:
        coverage = report["coverage"]
        if coverage["expected"] is None:
            print(
                f"error: --fail-under-coverage {args.fail_under_coverage} was requested but the "
                f"denominator could not be established ({coverage['provenance']}). Pass "
                "--build-dir or --expected.",
                file=sys.stderr,
            )
            return 1
        if coverage["percent"] < args.fail_under_coverage:
            print(
                f"error: only {coverage['percent']:.1f}% of compiles were recorded "
                f"({coverage['recorded']} of {coverage['expected']}), below the required "
                f"{args.fail_under_coverage:.1f}%. This ranking is a LOWER BOUND, not a census: "
                "a compiler-cache hit runs no compiler and appends no record, so build with the "
                "cache off (-DOLO_ENABLE_COMPILER_CACHE=OFF) for a measurement run.",
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
