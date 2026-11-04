#!/usr/bin/env python3
"""Summarise CTest/GoogleTest JUnit files as exact counts with names.

Prints, per file, how many cases passed, failed, were skipped and were
disabled, with the name (and skip message) of every case that did not pass,
in Markdown for a PR's evidence section. It reports; it does not decide
acceptance. A file whose own totals disagree with its cases is an error, so a
truncated or hand-edited file is never summarised as complete.

Usage: python3 -I -B tools/development/junit_summary.py LABEL=FILE [LABEL=FILE ...]
"""
import importlib.util
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

# Load only the bounded, DTD-free parser in this source tree.
_spec = importlib.util.spec_from_file_location(
    'odbcpp_xml_evidence', Path(__file__).resolve().parents[1] / 'odbcpp_xml_evidence.py')
_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_module)
parse_xml_file = _module.parse_xml_file

CATEGORIES = ('passed', 'failed', 'skipped', 'disabled')
MAX_DETAIL = 160


class SummaryError(ValueError):
    """The file cannot be summarised as a complete result."""


def _int_attr(element, name):
    value = element.get(name)
    if value is None:
        return None
    if not value.isdigit():
        raise SummaryError(f'{name}="{value}" is not a count')
    return int(value)


def classify(case):
    """Return (category, detail) for one <testcase>."""
    status = case.get('status')
    if case.find('failure') is not None or case.find('error') is not None or status == 'fail':
        return 'failed', None
    if status == 'disabled' or case.get('result') == 'suppressed':
        return 'disabled', None
    skipped = case.find('skipped')
    if skipped is not None or status == 'notrun' or case.get('result') == 'skipped':
        return 'skipped', (skipped.get('message') if skipped is not None else None) or status
    if status not in (None, 'run'):
        raise SummaryError(f'unknown status "{status}" for {case.get("name")}')
    return 'passed', None


def summarise(path):
    root = parse_xml_file(path)
    suites = [root] if root.tag == 'testsuite' else list(root.iter('testsuite'))
    if root.tag not in ('testsuite', 'testsuites') or not suites:
        raise SummaryError('not a JUnit file')
    result = {category: [] for category in CATEGORIES}
    for suite in suites:
        counts = {category: 0 for category in CATEGORIES}
        cases = suite.findall('testcase')
        for case in cases:
            name = case.get('name')
            if not name:
                raise SummaryError('a testcase has no name')
            classname = case.get('classname')
            if classname and classname != name:
                name = f'{classname}.{name}'
            category, detail = classify(case)
            counts[category] += 1
            result[category].append((name, detail))
        # The suite's own totals must match its cases (a truncated file fails).
        declared = _int_attr(suite, 'tests')
        if declared is not None and declared != len(cases):
            raise SummaryError(f'suite declares {declared} tests but has {len(cases)}')
        failures = sum(_int_attr(suite, key) or 0 for key in ('failures', 'errors'))
        if (suite.get('failures') is not None) and failures != counts['failed']:
            raise SummaryError(f'suite declares {failures} failures but has {counts["failed"]}')
    if not any(result.values()):
        raise SummaryError('no test cases')
    return result


def render(label, result):
    total = sum(len(rows) for rows in result.values())
    counts = ', '.join(f'{len(result[c])} {c}' for c in CATEGORIES)
    lines = [f'**{label}**: {total} tests: {counts}']
    for category in ('failed', 'skipped', 'disabled'):
        for name, detail in result[category]:
            # One line per case: messages can span lines (GoogleTest skips).
            detail = ' '.join((detail or '').split())
            if len(detail) > MAX_DETAIL:
                detail = detail[:MAX_DETAIL - 3] + '...'
            suffix = f' ({detail})' if detail else ''
            lines.append(f'- {category}: `{name}`{suffix}')
    return '\n'.join(lines)


def main(argv):
    if not argv:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    status = 0
    for argument in argv:
        label, sep, file_name = argument.partition('=')
        if not sep or not label or not file_name:
            print(f'error: expected LABEL=FILE, got {argument!r}', file=sys.stderr)
            return 2
        try:
            print(render(label, summarise(Path(file_name))))
        except (OSError, ET.ParseError, SummaryError) as error:
            print(f'**{label}**: cannot summarise {file_name}: {error}', file=sys.stderr)
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
