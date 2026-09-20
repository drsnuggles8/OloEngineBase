#!/usr/bin/env python3
"""Prove that every uploaded test report is readable, and say which suites they cover.

Issue #1372. When ``GTEST_OUTPUT`` names a DIRECTORY, googletest picks the filename
itself in ``FilePath::GenerateUniqueFileName()``::

    int number = 0;
    do { full_pathname.Set(MakeFileName(directory, base_name, number++, extension)); }
    while (full_pathname.FileOrDirectoryExists());

That is check-then-create with no lock, and the sanitizer shards run
``ctest --parallel 4``. Two processes starting together both picked the same number and
both wrote the file. The result was a report spliced mid-tag::

    <testcase name="..." classname="SkinOcularSurfaceTest" />
    <testct="completed" ... classname="CommandBucketBatchTest" />

The engine-side fix gives every process its own filename before InitGoogleTest resolves
it (``OloEngine/tests/TestXmlOutputPath.h``). This script is the second half: the part
that makes a RECURRENCE loud.

WHY IT IS NEEDED EVEN THOUGH THE BUG IS FIXED. The failure was silent for as long as it
existed. The shards stayed green -- no test failed, because the corruption is in the
report, not the run -- and the #1083 shard-coverage proof counts ctest ENTRIES, not
readable reports, so it could not see it either. The only symptom was
``dorny/test-reporter`` eventually failing to parse, which reads as a tooling problem
rather than as lost results, and which did NOT happen on runs where fewer files were
hit. Three independent checks looked at that run and none of them said "a suite's
results are missing". So the invariant gets its own check, with its own message.

WHAT THIS PROVES: every report file present parses, and every file was named by the
process that wrote it rather than by gtest's racing probe. WHAT IT DOES NOT PROVE: that
the reports are COMPLETE -- a shard that never ran uploads nothing, and that is
``verify_test_shards.py``'s job, which this deliberately does not duplicate.

IT DOES NOT CHECK THAT A SUITE APPEARS IN ONLY ONE FILE, and the first draft of this
script did. That check fired 31 times on a clean local run: the per-case entries
(``gtest_discover_tests`` for OLO_HEAVY_TESTS / OLO_MESH_CACHE_TESTS) give every CASE its
own process, so one suite legitimately spans many files, and a chunked suite does the
same. A guard that cries wolf on correct output gets muted, which is how the original bug
survived in the first place. The filename check replaces it and has no legitimate false
positive: ``OloEngine-Tests.xml`` and ``OloEngine-Tests_<n>.xml`` are names ONLY gtest's
own probe produces.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

# The names gtest's GenerateUniqueFileName() probe produces. Everything this
# repository writes is `OloEngine-Tests_<filter>_<pid>.xml`, which has a second
# underscore and so cannot match.
LEGACY_NAME = re.compile(r"OloEngine-Tests(_\d+)?\.xml")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dir",
        required=True,
        type=pathlib.Path,
        help="directory holding the downloaded report artifacts; searched recursively",
    )
    parser.add_argument("--label", default="tests", help="name used in the messages, e.g. 'ASan (Windows)'")
    parser.add_argument(
        "--require-reports",
        action="store_true",
        help="also fail when no report file was found at all. The sanitizer jobs all pass it: "
        "their dorny/test-reporter step, which would otherwise catch an empty upload, is SKIPPED "
        "on fork PRs, leaving this script as the only thing looking at the reports there",
    )
    args = parser.parse_args()

    if not args.dir.is_dir():
        # Silent-by-default only for a caller that has its own empty-set check;
        # every caller in this repository passes --require-reports, because a run
        # that verified nothing must not be able to look like a run that passed.
        print(f"{args.label}: no report directory at '{args.dir}' -- nothing to verify.")
        return 1 if args.require_reports else 0

    files = sorted(p for p in args.dir.rglob("*.xml") if p.is_file())
    if not files:
        print(f"{args.label}: no .xml report files under '{args.dir}' -- nothing to verify.")
        return 1 if args.require_reports else 0

    broken: list[tuple[pathlib.Path, str]] = []
    suites: set[str] = set()
    cases = 0

    for path in files:
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as exc:
            broken.append((path, str(exc)))
            continue
        for suite in root.iter("testsuite"):
            name = suite.get("name")
            if name:
                suites.add(name)
            try:
                cases += int(suite.get("tests", "0"))
            except ValueError:
                pass

    # A file gtest named ITSELF means the per-process naming did not run for that
    # process, and it went back through the racing probe.
    legacy = [p for p in files if LEGACY_NAME.fullmatch(p.name)]

    print(f"{args.label}: {len(files)} report files, {len(suites)} suites, {cases} cases.")

    if not broken and not legacy:
        return 0

    # Annotations and the summary must not interleave; the summary goes to stderr.
    sys.stdout.flush()

    for path, error in broken:
        rel = path.relative_to(args.dir).as_posix()
        print(f"::error file={rel}::{args.label}: report '{rel}' is not valid XML ({error})")
    for path in legacy:
        rel = path.relative_to(args.dir).as_posix()
        print(
            f"::error file={rel}::{args.label}: report '{rel}' was named by gtest's own probe, so this "
            "process did not get a unique filename and could have raced another"
        )

    print(
        f"\n{args.label}: {len(broken)} unreadable and {len(legacy)} gtest-named report(s).\n"
        "This is issue #1372: two test processes writing the same file. It means SOME SUITES' "
        "RESULTS ARE MISSING from this run -- the shards can be green and still have lost them, "
        "because the corruption is in the report rather than in the run.\n"
        "Check that MakeGTestOutputUniqueToThisProcess() in OloEngine/tests/OloEngineTest.cpp is "
        "still called before InitGoogleTest -- it is what gives each process its own filename, and "
        "it covers both the GTEST_OUTPUT environment variable and a --gtest_output= argument.",
        file=sys.stderr,
    )
    sys.stderr.flush()
    return 1


if __name__ == "__main__":
    sys.exit(main())
