#!/usr/bin/env python3
"""Summarise CTest/GoogleTest JUnit files as exact counts with names.

Prints, per file, how many cases passed, failed, were skipped and were
disabled, with the name (and skip message) of every case that did not pass,
in Markdown for a PR's evidence section. It reports; it does not decide
acceptance. A file whose declared totals (on each <testsuite> and on the root
<testsuites>) disagree with its cases is an error, so a truncated or
hand-edited file is never summarised as complete. A CTest "notrun" case is a
skip only when CTest skipped it on purpose (SKIP_RETURN_CODE or
SKIP_REGULAR_EXPRESSION); any other "notrun" (missing executable, failed
fixture, missing files) is reported as failed, as ctest itself does.

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
# CTest NOT_RUN messages that mean an intended skip.
CTEST_SKIPS = ('SKIP_RETURN_CODE=', 'SKIP_REGULAR_EXPRESSION_MATCHED')


class SummaryError(ValueError):
    """The file cannot be summarised as a complete result."""


def _int_attr(element, name):
    value = element.get(name)
    if value is None:
        return None
    if not (value.isascii() and value.isdigit()):
        raise SummaryError(f'{name}="{value}" is not a count')
    return int(value)


def _declared_failure(case):
    """A failure the file's failures=/errors= totals count."""
    return (case.find('failure') is not None or case.find('error') is not None
            or case.get('status') == 'fail')


def classify(case):
    """Return (category, detail) for one <testcase>."""
    status, result = case.get('status'), case.get('result')
    if _declared_failure(case):
        return 'failed', None
    if status == 'disabled' or result == 'suppressed':
        return 'disabled', None
    skipped = case.find('skipped')
    message = skipped.get('message') if skipped is not None else None
    if status == 'notrun':
        if message and message.startswith(CTEST_SKIPS):
            return 'skipped', message
        return 'failed', message or status
    if skipped is not None or result == 'skipped':
        return 'skipped', message or result
    if status not in (None, 'run'):
        raise SummaryError(f'unknown status "{status}" for {case.get("name")}')
    if result not in (None, 'completed'):
        raise SummaryError(f'unknown result "{result}" for {case.get("name")}')
    return 'passed', None


def summarise(path):
    root = parse_xml_file(path)
    suites = [root] if root.tag == 'testsuite' else list(root.iter('testsuite'))
    if root.tag not in ('testsuite', 'testsuites') or not suites:
        raise SummaryError('not a JUnit file')
    result = {category: [] for category in CATEGORIES}
    seen = declared_failures = 0
    for suite in suites:
        failed = 0
        cases = suite.findall('testcase')
        seen += len(cases)
        for case in cases:
            name = case.get('name')
            if not name:
                raise SummaryError('a testcase has no name')
            classname = case.get('classname')
            if classname and classname != name:
                name = f'{classname}.{name}'
            category, detail = classify(case)
            failed += _declared_failure(case)
            result[category].append((name, detail))
        declared_failures += failed
        # The suite's own totals must match its cases (a truncated file fails).
        declared = _int_attr(suite, 'tests')
        if declared is not None and declared != len(cases):
            raise SummaryError(f'suite declares {declared} tests but has {len(cases)}')
        _check_failures(suite, failed)
    if sum(1 for _ in root.iter('testcase')) != seen:
        raise SummaryError('a testcase is outside a testsuite')
    if root.tag == 'testsuites':
        declared = _int_attr(root, 'tests')
        if declared is not None and declared != seen:
            raise SummaryError(f'file declares {declared} tests but has {seen}')
        _check_failures(root, declared_failures)
    if not any(result.values()):
        raise SummaryError('no test cases')
    return result


def _check_failures(element, failed):
    if element.get('failures') is None and element.get('errors') is None:
        return
    declared = sum(_int_attr(element, key) or 0 for key in ('failures', 'errors'))
    if declared != failed:
        raise SummaryError(f'{element.tag} declares {declared} failures but has {failed}')


def _code(text):
    """One line of untrusted text as an inert Markdown code span.

    Control characters are dropped, whitespace is collapsed and backticks are
    replaced, so the text cannot end the span, hide later lines (<!--),
    mention a user (@name) or send terminal escapes.
    """
    text = ''.join(ch if ch.isprintable() else ' ' for ch in text)
    text = ' '.join(text.split()).replace('`', "'")
    if len(text) > MAX_DETAIL:
        text = text[:MAX_DETAIL - 3] + '...'
    return f'`{text}`'


def render(label, result):
    total = sum(len(rows) for rows in result.values())
    counts = ', '.join(f'{len(result[c])} {c}' for c in CATEGORIES)
    lines = [f'**{label}**: {total} tests: {counts}']
    for category in ('failed', 'skipped', 'disabled'):
        for name, detail in result[category]:
            # One line per case: messages can span lines (GoogleTest skips).
            suffix = f' ({_code(detail)})' if detail and detail.strip() else ''
            lines.append(f'- {category}: {_code(name)}{suffix}')
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
            # On stdout too, so a saved summary cannot silently omit a file.
            message = f'**{label}**: cannot summarise {_code(file_name)}: {_code(str(error))}'
            print(message)
            print(message, file=sys.stderr)
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
