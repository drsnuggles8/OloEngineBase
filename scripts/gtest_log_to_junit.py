#!/usr/bin/env python3
"""Rebuild a JUnit report from a googletest console log when the binary wrote none.

Issue #1473. googletest writes ``--gtest_output=xml`` once, at the end of the run. A
process that never reaches the end -- killed by a step timeout, by the OOM killer, or
by a crash -- leaves no report at all, and the only thing the next step can say is
"no XML". The 2026-09-27 AMD nightly ran 6820 tests for 60 minutes and reported
nothing, including which test it was in when the timeout fired.

The console log has everything a report needs: ``[ RUN      ]`` opens a test, and
``[       OK ]`` / ``[  FAILED  ]`` / ``[  SKIPPED ]`` close it with its duration. This
script turns that log into a JUnit file with

* one ``<testcase>`` per finished test, with its result, time and failure text;
* one failed ``<testcase>`` for the test that was RUNNING when the process ended,
  whose message says why the run ended (``--reason``);
* a ``olo_partial`` property on the root, so a consumer can tell this report from a
  complete one. The workflow's assert step fails the job on it: a partial report is
  evidence of what ran, never a pass.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

# gtest's console markers. Optional leading timestamp: GitHub's downloaded job logs
# prefix every line with one, and this script should read those too.
_PREFIX = r"^(?:\S+Z )?"
RUN = re.compile(_PREFIX + r"\[ RUN      \] (\S+)")
CLOSE = re.compile(_PREFIX + r"\[( {7}OK |  FAILED  |  SKIPPED )\] (\S+?)(?:,.*)? \((\d+) ms\)")


def parse(lines: list[str]) -> tuple[list[dict], dict | None]:
    finished: list[dict] = []
    current: dict | None = None
    for line in lines:
        line = line.rstrip("\n")
        if m := RUN.match(line):
            current = {"name": m.group(1), "output": []}
            continue
        if current is None:
            continue
        if m := CLOSE.match(line):
            if m.group(2) != current["name"]:
                continue
            current["status"] = m.group(1).strip()
            current["ms"] = int(m.group(3))
            finished.append(current)
            current = None
            continue
        current["output"].append(line)
    return finished, current


def failure_text(output: list[str]) -> str:
    # Keep the assertion blocks, not the engine's log lines between them.
    keep: list[str] = []
    for i, line in enumerate(output):
        if "Failure" in line or line.startswith(("Expected", "Value of", "  Actual", "Which is")):
            keep.extend(output[i : i + 6])
    return "\n".join(keep)[:8000] or "failed (no assertion text in the log)"


def build(finished: list[dict], running: dict | None, reason: str) -> ET.Element:
    root = ET.Element("testsuites", name="AllTests")
    props = ET.SubElement(root, "properties")
    ET.SubElement(props, "property", name="olo_partial", value="true")
    ET.SubElement(props, "property", name="olo_partial_reason", value=reason)
    ET.SubElement(
        props, "property", name="olo_running_test", value=running["name"] if running else "(between tests)"
    )

    suites: dict[str, ET.Element] = {}

    def case(name: str) -> ET.Element:
        suite_name, _, test_name = name.partition(".")
        suite = suites.get(suite_name)
        if suite is None:
            suite = suites[suite_name] = ET.SubElement(root, "testsuite", name=suite_name)
        return ET.SubElement(suite, "testcase", name=test_name, classname=suite_name)

    failures = 0
    for test in finished:
        tc = case(test["name"])
        tc.set("time", f"{test['ms'] / 1000.0:.3f}")
        if test["status"] == "FAILED":
            failures += 1
            ET.SubElement(tc, "failure", message="failed").text = failure_text(test["output"])
        elif test["status"] == "SKIPPED":
            ET.SubElement(tc, "skipped", message="\n".join(test["output"])[-2000:])
    if running is not None:
        failures += 1
        tc = case(running["name"])
        ET.SubElement(tc, "failure", message=f"{reason} while this test was running").text = (
            f"The process ended ({reason}) during this test; it has no result. "
            f"{len(finished)} tests finished before it."
        )

    total = len(finished) + (1 if running else 0)
    root.set("tests", str(total))
    root.set("failures", str(failures))
    for suite in suites.values():
        suite.set("tests", str(len(suite)))
    return root


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--log", required=True, type=pathlib.Path)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--reason", required=True)
    args = ap.parse_args()

    lines = args.log.read_text(encoding="utf-8", errors="replace").splitlines()
    finished, running = parse(lines)
    ET.ElementTree(build(finished, running, args.reason)).write(args.out, encoding="utf-8", xml_declaration=True)
    running_name = running["name"] if running else "(between tests)"
    print(f"partial report: {len(finished)} finished test(s); running at the end: {running_name}; reason: {args.reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
