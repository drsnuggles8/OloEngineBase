"""Report, and optionally require, execution of the L7 Vulkan ray-query device suites (#1294).

Every ray-query device test skips cleanly when the device cannot trace rays, so a green run says
nothing about whether any of them executed. This reads the gtest XML and writes, per suite, how
many cases executed, passed, failed and skipped, with each skip's reason, to the job summary.

Two modes:

* report (default): Windows.yml, where hosted runners have no ray-query device and skipping is
  expected. Fails only when the filter matched no case at all.
* --require: vulkan-software.yml (lavapipe). Every case the suites define must execute, apart
  from those named with --allow-skip (opt-in experiments). The defined set comes from
  --expected-list (the binary's --gtest_list_tests output), so a case a --gtest_filter exclusion
  dropped is reported missing rather than silently absent from the XML.

Test failures are not this script's verdict; the test step that produced the XML already fails
the job for those. This gate is about cases that did not run.
"""
import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET

_SUITE_LINE = re.compile(r'^([A-Za-z_][A-Za-z0-9_/]*)\.(\s+#.*)?$')
_CASE_LINE = re.compile(r'^  ([A-Za-z_][A-Za-z0-9_/]*)(\s+#.*)?$')

# Lines of the capability report (vulkan-software.yml writes it before the suites run) that name
# the device, its driver, and whether the real gate enabled ray query.
_CAPABILITY_LINES = (
    re.compile(r'^### Device \d+: '),
    re.compile(r'^- Driver: '),
    re.compile(r'^Optional \(never part of the verdict\): '),
    re.compile(r'^`VulkanDevice::Init` '),
)


def parse_test_list(text, suites):
    """Cases per suite from --gtest_list_tests output, ignoring anything else the binary printed."""
    cases = {suite: [] for suite in suites}
    current = None
    for line in text.splitlines():
        suite_match = _SUITE_LINE.match(line)
        if suite_match:
            current = suite_match.group(1) if suite_match.group(1) in cases else None
            continue
        case_match = _CASE_LINE.match(line)
        if case_match and current is not None:
            cases[current].append(case_match.group(1))
        elif not case_match:
            current = None
    return cases


def parse_report(root, suites):
    """{suite: {case: (status, reason)}}, status one of passed / failed / skipped / notrun."""
    results = {suite: {} for suite in suites}
    for case in root.iter('testcase'):
        suite = case.get('classname')
        if suite not in results:
            continue
        skipped = list(case.iter('skipped'))
        failures = list(case.iter('failure')) + list(case.iter('error'))
        reason = ''
        if case.get('status') == 'notrun':
            status = 'notrun'
        elif skipped or case.get('result') == 'skipped':
            status = 'skipped'
            for node in skipped:
                # gtest writes "file:line" and the message on separate lines.
                reason = ' '.join((node.get('message') or node.text or '').split()).replace('`', "'")
                if reason:
                    break
            reason = reason or 'No skip reason supplied.'
        elif failures:
            status = 'failed'
        else:
            status = 'passed'
        results[suite][case.get('name')] = (status, reason)
    return results


def evaluate(results, expected, allowed_skips):
    """Rows for the summary plus the list of required cases that did not execute."""
    rows = []
    missing_execution = []
    for suite, cases in results.items():
        defined = expected.get(suite) if expected is not None else None
        names = list(defined) if defined is not None else list(cases)
        names += [name for name in cases if name not in names]
        counts = {'passed': 0, 'failed': 0, 'skipped': 0, 'missing': 0}
        notes = []
        for name in names:
            status, reason = cases.get(name, ('missing', ''))
            if status == 'notrun':
                status = 'missing'
            counts[status] += 1
            full = suite + '.' + name
            if status == 'skipped':
                allowed = full in allowed_skips
                notes.append((full, 'skipped (allowed opt-in)' if allowed else 'skipped', reason))
                if not allowed:
                    missing_execution.append(full + ' skipped: ' + reason)
            elif status == 'missing':
                notes.append((full, 'did not run', 'not in the XML: filtered out, or the run died first'))
                missing_execution.append(full + ' did not run')
        if not names:
            missing_execution.append(suite + ' defines no case in this run')
        rows.append((suite, len(names), counts, notes))
    return rows, missing_execution


def capability_lines(path):
    if not path:
        return []
    try:
        with open(path, encoding='utf-8') as handle:
            text = handle.read()
    except OSError as error:
        return ['(capability report unreadable: {})'.format(error)]
    return [line for line in text.splitlines() if any(p.match(line) for p in _CAPABILITY_LINES)]


def render(title, rows, missing_execution, require, device_lines):
    out = ['## ' + title, '']
    sha = os.environ.get('GITHUB_SHA')
    if sha:
        out.append('- Commit: `{}`'.format(sha))
    for line in device_lines:
        out.append('- ' + line.lstrip('#- ').strip())
    out.append('- Mode: {}'.format('**required**: every listed case must execute' if require
                                   else 'report only: skips are expected on this runner'))
    out += ['', '| Suite | Cases | Executed | Passed | Failed | Skipped | Did not run |',
            '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for suite, total, counts, _ in rows:
        executed = counts['passed'] + counts['failed']
        out.append('| `{}` | {} | **{}** | {} | {} | {} | {} |'.format(
            suite, total, executed, counts['passed'], counts['failed'], counts['skipped'], counts['missing']))
    notes = [note for _, _, _, suite_notes in rows for note in suite_notes]
    if notes:
        out += ['', 'Cases that did not execute:', '']
        for name, state, reason in notes:
            out.append('- `{}`: {}. `{}`'.format(name, state, reason))
    if require:
        out.append('')
        if missing_execution:
            out.append('**FAILED: {} required case(s) did not execute.**'.format(len(missing_execution)))
        else:
            out.append('Every required case executed.')
    out.append('')
    return '\n'.join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--report', required=True, help='gtest XML from the suite run')
    parser.add_argument('--suite', action='append', required=True, help='a suite to account for')
    parser.add_argument('--expected-list', help='--gtest_list_tests output naming every defined case')
    parser.add_argument('--allow-skip', action='append', default=[], help='Suite.Case that may skip')
    parser.add_argument('--capability-report', help='--olo-vulkan-capability-report output to quote')
    parser.add_argument('--require', action='store_true', help='fail unless every listed case executed')
    parser.add_argument('--title', default='Vulkan ray-query device-suite coverage')
    args = parser.parse_args(argv)

    try:
        root = ET.parse(args.report).getroot()
    except (OSError, ET.ParseError) as error:
        print('::error::Could not read Vulkan ray-query device-suite report {}: {}'.format(args.report, error))
        return 1

    expected = None
    if args.expected_list:
        try:
            with open(args.expected_list, encoding='utf-8', errors='replace') as handle:
                expected = parse_test_list(handle.read(), args.suite)
        except OSError as error:
            print('::error::Could not read the expected test list {}: {}'.format(args.expected_list, error))
            return 1

    results = parse_report(root, args.suite)
    rows, missing_execution = evaluate(results, expected, set(args.allow_skip))
    summary = render(args.title, rows, missing_execution, args.require, capability_lines(args.capability_report))

    summary_path = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary_path:
        with open(summary_path, 'a', encoding='utf-8') as output:
            output.write(summary)
    print(summary)

    if sum(total for _, total, _, _ in rows) == 0:
        print('::error::Vulkan ray-query device-suite filter matched no test cases')
        return 1
    if args.require and missing_execution:
        for entry in missing_execution:
            print('::error::Required ray-query case did not execute: ' + entry)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
