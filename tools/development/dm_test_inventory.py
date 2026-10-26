#!/usr/bin/env python3
"""Classify every GoogleTest case by what it needs from the driver.

Phase 0 of docs/plans/driver-manager-test-layer.md.  This is a lexical source
inventory, not a C++ parser and not test evidence.  For each TEST/TEST_F/TEST_P
it collects the case body plus everything the body can reach inside its own
file and the included tests/ support headers (fixture classes and their bases,
file-local helpers and types, transitively), then classifies what that text
names:

  public-only      calls ODBC SQL* entry points and names no driver/SDK
                   internals; could run through a Driver Manager as written
                   once its connection setup is a DRIVER=/DSN= string.
  fake-backend     calls ODBC SQL* entry points; the only internals are an
                   injected fake backend/transport and the glue that registers
                   a connection over it.  Becomes public-only once Phase 2 makes
                   a fake reachable through a connection-string keyword.
  needs-internals  calls ODBC SQL* entry points and also names driver/SDK
                   internals (HandleRegistry::get_handle_as, *TestAccess,
                   add_diagnostic, internal classes, ...).
  no-odbc-entry    never calls an ODBC SQL* entry point: SDK, backend,
                   transport, auth or crypto component tests.  Also "needs
                   internals"; listed apart because no manager can reach them.

Internal names come from the project headers (core/, odbc/, sdk/, product/):
declared types and aliases, namespace-scope functions (only when called), and
any rs::/odbcpp:: qualified name outside odbcpp::test.  Names a test file or
its support headers declare themselves shadow project names.  The per-case CSV
keeps the matched names as evidence so a reviewer can audit a classification,
and says whether they appear in the case body or only in reached helpers.
TEST_P rows are definitions; instantiate_macros counts the
INSTANTIATE_TEST_SUITE_P uses for the suite, not the runtime parameter count.
"""
from __future__ import annotations

import argparse
import collections
import csv
import io
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

TEST_MACROS = ("TEST", "TEST_F", "TEST_P", "TYPED_TEST", "TYPED_TEST_P")
PRODUCT_HEADER_ROOTS = ("core", "odbc", "sdk", "product")
TEST_ROOTS = ("tests/unit", "tests/integration", "tests/driver_manager",
              "tests/security", "tests/windows")
CLASSES = ("public-only", "fake-backend", "needs-internals", "no-odbc-entry")

# Internals that only build a fake-backed connection.  Anything else in
# reachable text makes the case need internals.
FAKE_GLUE = frozenset({
    "HandleRegistry", "instance", "register_handle", "ODBCConnection", "ODBCEnvironment",
    "IBackendProvider", "IDatabaseConnection", "ITransport",
    "SqlDialectProfile", "PgCatalogProfile", "DatabaseType", "BackendProfile",
})
# Bases that make a file-local type a fake backend or transport.
FAKE_BASES = frozenset({
    "IBackendProvider", "IDatabaseConnection", "IStatementDescription",
    "ITransactionSession", "ICatalogExecution", "ICatalogQueries",
    "ISqlDialect", "ITransport", "GenericDatabaseConnection",
    "IStreamingResult", "IResultStream",
})
FAKE_NAME = re.compile(r"^(Fake|Scripted|Mock|Stub|Fault|Recording)|"
                       r"(Fake|Mock|Stub|Wire|Provider|Backend|Transport)$")
# Identifiers that are never evidence of internals even if a project header
# declares them: ODBC names, keywords and common std/gtest words.
NEVER_INTERNAL = frozenset("""
    std testing size data value values begin end get set reset count type name
    string string_view vector optional array span map pair tuple unique_ptr
    shared_ptr make_shared make_unique move forward function chrono error
    result status state code text message length format operator this
    true false nullptr auto const static inline return main detail test
    """.split())
QUALIFIED = re.compile(r"\b(?:rs|odbcpp)(?:::\w+)+")
IDENT = re.compile(r"[A-Za-z_]\w*")
SQL_CALL = re.compile(r"(?<![\w.>:])(?:::)?(SQL[A-Z]\w*)\s*\(")


def strip_source(text: str) -> str:
    """Blank comments, literal contents and preprocessor lines; keep newlines."""
    out: list[str] = []
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if line_start and c in " \t":
            out.append(c)
            i += 1
            continue
        if line_start and c == "#":
            j = i
            while j < n and text[j] != "\n":
                j = j + 2 if text.startswith("\\\n", j) else j + 1
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
            continue
        line_start = c == "\n"
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif (m := re.match(r'(?:u8|[uUL])?R"([^(\s]{0,16})\(', text[i:i + 24])) and \
                (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            close = ")" + m.group(1) + '"'
            j = text.find(close, i + m.end())
            j = n if j < 0 else j + len(close)
            out.append('""' + re.sub(r"[^\n]", " ", text[i + 2:j]))
            i = j
        elif c in "\"'":
            if c == "'" and i > 0 and text[i - 1].isalnum():
                out.append(c)  # digit separator such as 1'000
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * max(0, j - i - 2) + c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_brace(text: str, open_index: int) -> int:
    depth = 0
    for k in range(open_index, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return k
    return len(text) - 1


@dataclass
class Definition:
    kind: str  # type | function | variable
    name: str
    text: str
    bases: tuple[str, ...] = ()


@dataclass
class Case:
    macro: str
    suite: str
    name: str
    line: int
    text: str


@dataclass
class ParsedFile:
    definitions: dict[str, list[Definition]] = field(default_factory=dict)
    cases: list[Case] = field(default_factory=list)
    instantiations: collections.Counter = field(default_factory=collections.Counter)


TYPE_HEAD = re.compile(r"\b(?:class|struct|union|enum(?:\s+class|\s+struct)?)\s+"
                       r"(?:\[\[[^\]]*\]\]\s*|alignas\([^)]*\)\s*)*(\w+)\s*(?:final\s*)?(:[^{]*)?$")
TEST_HEAD = re.compile(r"\b(" + "|".join(TEST_MACROS) + r")\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*$")
INSTANTIATE = re.compile(r"\b(?:INSTANTIATE_TEST_SUITE_P|INSTANTIATE_TYPED_TEST_SUITE_P)"
                         r"\s*\(\s*\w*\s*,\s*(\w+)")


def _definition_name(head: str) -> Definition | None:
    head = head.strip()
    if not head:
        return None
    if m := TYPE_HEAD.search(head):
        bases = tuple(IDENT.findall(re.sub(r"\b(public|private|protected|virtual)\b", "",
                                           m.group(2) or "")))
        return Definition("type", m.group(1), "", bases)
    if "=" in head and not re.search(r"\boperator\b", head):
        left = head.split("=", 1)[0]
        names = IDENT.findall(left)
        return Definition("variable", names[-1], "") if names else None
    if "(" in head:
        depth = 0
        start = None
        for k, ch in enumerate(head):
            if ch == "(":
                if depth == 0 and start is None:
                    start = k
                depth += 1
            elif ch == ")":
                depth -= 1
        if start is None:
            return None
        names = IDENT.findall(head[:start])
        if not names or names[-1] in ("if", "for", "while", "switch", "catch", "return"):
            return None
        if names[-1] == "operator" or "operator" in names:
            return Definition("function", "operator", "")
        return Definition("function", names[-1], "")
    names = IDENT.findall(head)  # brace-initialized variable: T name{...}
    if len(names) >= 2:
        return Definition("variable", names[-1], "")
    return None


def parse(stripped: str) -> ParsedFile:
    """Collect namespace-scope definitions and test cases from stripped text."""
    parsed = ParsedFile()
    for m in INSTANTIATE.finditer(stripped):
        parsed.instantiations[m.group(1)] += 1
    i, n = 0, len(stripped)
    boundary = 0
    while i < n:
        c = stripped[i]
        if c in ";}":
            boundary = i + 1
        elif c == "{":
            head = stripped[boundary:i]
            if re.search(r"\bnamespace\b[\w:\s]*$", head) or re.search(r'extern\s*""\s*$', head):
                boundary = i + 1  # transparent scope
            else:
                end = match_brace(stripped, i)
                body = stripped[boundary:end + 1]
                if m := TEST_HEAD.search(head):
                    line = stripped.count("\n", 0, boundary + head.find(m.group(1))) + 1
                    parsed.cases.append(Case(m.group(1), m.group(2), m.group(3), line, body))
                elif d := _definition_name(head):
                    d.text = body
                    parsed.definitions.setdefault(d.name, []).append(d)
                i = end
                boundary = end + 1
        i += 1
    return parsed


def header_symbols(repo: Path) -> tuple[set[str], set[str], set[str]]:
    """Project type names anywhere, free functions at namespace scope, namespaces."""
    types: set[str] = set()
    functions: set[str] = set()
    namespaces: set[str] = set()
    for root in PRODUCT_HEADER_ROOTS:
        for path in sorted((repo / root).rglob("*.h")):
            text = strip_source(path.read_text(encoding="utf-8", errors="replace"))
            types.update(m.group(1) or m.group(2) for m in re.finditer(
                r"\b(?:class|struct|union|enum(?:\s+class)?)\s+(\w+)\s*(?:final\s*)?[:{;]|"
                r"\busing\s+(\w+)\s*=", text))
            functions.update(namespace_scope_functions(text))
            namespaces.update(re.findall(r"\bnamespace\s+([\w:]+)", text))
    def keep(name: str) -> bool:
        return (bool(name) and not name.startswith("SQL") and name not in NEVER_INTERNAL
                and len(name) > 2 and not re.fullmatch(r"[A-Z0-9_]+", name))
    namespaces = {part for name in namespaces for part in name.split("::")}
    return ({t for t in types if keep(t)} - namespaces,
            {f for f in functions if keep(f)} - namespaces, namespaces)


def namespace_scope_functions(stripped: str) -> set[str]:
    names: set[str] = set()
    i, n, boundary = 0, len(stripped), 0
    while i < n:
        c = stripped[i]
        if c in ";{":
            head = stripped[boundary:i]
            if c == "{" and (re.search(r"\bnamespace\b[\w:\s]*$", head) or
                             re.search(r'extern\s*""\s*$', head)):
                boundary = i + 1
                i += 1
                continue
            if not re.search(r"\b(class|struct|union|enum|using|typedef|return)\b|=", head):
                if d := _definition_name(head):
                    if d.kind == "function" and d.name != "operator":
                        names.add(d.name)
            if c == "{":
                i = match_brace(stripped, i)
            boundary = i + 1
        elif c == "}":
            boundary = i + 1
        i += 1
    return names


LOCAL_DECL = re.compile(r"\b(?:class|struct|union|enum(?:\s+class)?)\s+(\w+)\s*(?:final\s*)?[:{;]|"
                        r"\busing\s+(\w+)\s*=|\b(?:template\s*<[^>]*?\b(?:class|typename))\s+(\w+)|"
                        r"<\s*(?:class|typename)\s+(\w+)")


def local_names(stripped: str) -> set[str]:
    return {next(g for g in m.groups() if g) for m in LOCAL_DECL.finditer(stripped)}


def local_includes(text: str, path: Path, repo: Path) -> list[Path]:
    found = []
    for m in re.finditer(r'^\s*#\s*include\s+"([^"]+)"', text, re.M):
        for base in (path.parent, repo):
            candidate = (base / m.group(1)).resolve()
            if candidate.is_file() and repo / "tests" in candidate.parents:
                found.append(candidate)
                break
    return found


def is_fake(definition: Definition) -> bool:
    return definition.kind == "type" and (
        bool(FAKE_NAME.search(definition.name)) and definition.name.endswith(
            ("Provider", "Backend", "Transport", "Wire", "Session", "Dialect", "Connection"))
        or any(b in FAKE_BASES for b in definition.bases))


def fake_side(definitions: dict[str, list[Definition]]) -> set[str]:
    """Fakes plus the local types they name: their state and observations."""
    side = {name for name, defs in definitions.items() if any(is_fake(d) for d in defs)}
    pending = list(side)
    while pending:
        for d in definitions[pending.pop()]:
            for ident in set(IDENT.findall(d.text)):
                if ident not in side and any(x.kind == "type" for x in definitions.get(ident, ())):
                    side.add(ident)
                    pending.append(ident)
    return side


@dataclass
class Symbols:
    types: set[str]
    functions: set[str]
    namespaces: set[str]


@dataclass
class Classification:
    klass: str
    sql_calls: list[str]
    internals: list[str]
    fakes: list[str]
    fake_state: bool
    internals_in: str
    connect: str


def internal_names(text: str, symbols: Symbols, shadow: set[str]) -> set[str]:
    names: set[str] = set()
    for m in QUALIFIED.finditer(text):
        parts = m.group(0).split("::")
        if parts[:2] == ["odbcpp", "test"]:
            continue
        known = [x for x in parts if x in symbols.types or x in symbols.functions]
        names.update(known or [x for x in parts[-1:] if x not in symbols.namespaces])
    for m in re.finditer(r"(?<![\w.])(?<!->)(?<!::)([A-Za-z_]\w*)(\s*\()?", text):
        ident = m.group(1)
        if ident in shadow:
            continue
        if ident in symbols.types or (m.group(2) and ident in symbols.functions):
            names.add(ident)
    names.update(re.findall(r"\w*TestAccess\b", text))
    # Glue is registration of a fake-backed connection only: any other
    # registry member, or any other use of ODBCConnection, is an internal.
    for member in re.findall(r"\bHandleRegistry\s*::\s*instance\s*\(\s*\)\s*\.\s*(\w+)", text):
        if member != "register_handle":
            names.add(f"HandleRegistry::{member}")
    construction = r"make_(?:unique|shared)\s*<\s*(?:(?:::)?rs::odbc::)?ODBCConnection\s*>"
    if len(re.findall(r"\bODBCConnection\b", text)) > len(re.findall(construction, text)):
        names.add("ODBCConnection(non-construction)")
    return {x for x in names if not x.startswith("SQL") and x not in NEVER_INTERNAL
            and x not in symbols.namespaces and not re.fullmatch(r"[A-Z0-9_]+", x)}


def classify(case: Case, definitions: dict[str, list[Definition]], symbols: Symbols,
             shadow: set[str], fake_types: set[str]) -> Classification:
    seen_defs: set[int] = set()
    texts = [case.text]
    fakes: set[str] = set()
    pending = [case.text]
    if case.macro in ("TEST_F", "TEST_P", "TYPED_TEST", "TYPED_TEST_P"):
        pending.append(case.suite)
    while pending:
        chunk = pending.pop()
        for ident in set(IDENT.findall(chunk)):
            if ident in fake_types:
                fakes.add(ident)
                continue
            for d in definitions.get(ident, ()):
                if id(d) not in seen_defs:
                    seen_defs.add(id(d))
                    texts.append(d.text)
                    pending.append(d.text)
    reach = "\n".join(texts)
    sql_calls = sorted(set(SQL_CALL.findall(reach)))
    names = internal_names(reach, symbols, shadow)
    # Variables of fake-side type (seen, observations, wire, ...) named in the
    # case body mean the case configures or asserts on fake-backend state.
    fake_vars = set()
    for t in fakes:
        fake_vars.update(re.findall(r"\b" + t + r"\b[^;(){}=]*?>?\s*\b(\w+)\s*(?:=|\{|;)", reach))
    body_idents = set(IDENT.findall(case.text))
    fake_state = bool(body_idents & (fakes | fake_vars))
    # Internals named only by fixtures/helpers are cheaper to remove than
    # internals the case body itself calls or asserts on.
    beyond_glue = names - FAKE_GLUE
    if not beyond_glue:
        internals_in = ""
    elif internal_names(case.text, symbols, shadow) - FAKE_GLUE:
        internals_in = "body"
    else:
        internals_in = "helpers"
    if not sql_calls:
        klass = "no-odbc-entry"
    elif not names and not fakes:
        klass = "public-only"
    elif fakes and names <= FAKE_GLUE:
        klass = "fake-backend"
    else:
        klass = "needs-internals"
    if any(re.fullmatch(r"SQLDriverConnect[AW]?", x) for x in sql_calls):
        connect = "SQLDriverConnect"
    elif any(re.fullmatch(r"SQLConnect[AW]?", x) for x in sql_calls):
        connect = "SQLConnect"
    else:
        connect = "none"
    if re.search(r"\bHandleRegistry\s*::\s*instance\s*\(\s*\)\s*\.\s*register_handle\b", reach):
        connect += "+registered-handle"
    return Classification(klass, sql_calls, sorted(beyond_glue),
                          sorted(fakes), fake_state, internals_in, connect)


def inventory(repo: Path) -> list[dict[str, str]]:
    symbols = Symbols(*header_symbols(repo))
    rows = []
    for root in TEST_ROOTS:
        for path in sorted((repo / root).rglob("*.cpp")):
            raw = path.read_text(encoding="utf-8", errors="replace")
            stripped = strip_source(raw)
            parsed = parse(stripped)
            if not parsed.cases:
                continue
            definitions = {k: list(v) for k, v in parsed.definitions.items()}
            shadow = local_names(stripped) | set(definitions)
            for header in local_includes(raw, path, repo):
                header_text = strip_source(header.read_text(encoding="utf-8", errors="replace"))
                extra = parse(header_text)
                shadow |= local_names(header_text) | set(extra.definitions)
                for name, defs in extra.definitions.items():
                    definitions.setdefault(name, []).extend(defs)
            fake_types = fake_side(definitions)
            rel = path.relative_to(repo).as_posix()
            for case in parsed.cases:
                result = classify(case, definitions, symbols, shadow, fake_types)
                rows.append({
                    "file": rel, "line": str(case.line), "macro": case.macro,
                    "suite": case.suite, "case": case.name,
                    "instantiate_macros": str(parsed.instantiations.get(case.suite, 0))
                    if case.macro == "TEST_P" else "",
                    "class": result.klass, "connect": result.connect,
                    "fakes": " ".join(result.fakes),
                    "fake_state": "yes" if result.fake_state else "",
                    "internals_in": result.internals_in,
                    "internals": " ".join(result.internals[:8]),
                    "sql_calls": str(len(result.sql_calls)),
                })
    return rows


def write_csv(rows: list[dict[str, str]]) -> str:
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=list(rows[0]), lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    return buffer.getvalue()


def counts_table(groups: dict[str, collections.Counter], label: str) -> list[str]:
    lines = [f"| {label} | cases | " + " | ".join(CLASSES) + " |",
             "|---|---:|" + "---:|" * len(CLASSES)]
    for key in sorted(groups):
        counter = groups[key]
        lines.append(f"| `{key}` | {sum(counter.values())} | " +
                     " | ".join(str(counter.get(c, 0)) for c in CLASSES) + " |")
    return lines


def summary(rows: list[dict[str, str]]) -> str:
    total = collections.Counter(r["class"] for r in rows)
    by_dir: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    by_file: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    by_suite: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    for r in rows:
        by_dir[r["file"].rsplit("/", 1)[0]][r["class"]] += 1
        by_file[r["file"]][r["class"]] += 1
        by_suite[f'{r["file"].rsplit("/", 1)[1]}:{r["suite"]}'][r["class"]] += 1
    out = ["# Generated by tools/development/dm_test_inventory.py; do not edit.", "",
           f"Total GoogleTest case definitions: {len(rows)} "
           "(TEST_P counted once per definition).", ""]
    out += counts_table({"all": total}, "scope") + [""]
    split = collections.Counter(
        (r["class"], r["internals_in"] or "-", r["fake_state"] or "-") for r in rows
        if r["class"] in ("fake-backend", "needs-internals"))
    out += ["Where the internals are (`helpers` = only fixtures/helpers the case reaches) and",
            "whether the case body configures or reads fake-backend state:", "",
            "| class | internals in | fake state in body | cases |", "|---|---|---|---:|"]
    out += [f"| {k[0]} | {k[1]} | {k[2]} | {v} |" for k, v in sorted(split.items())] + [""]
    out += ["## By directory", ""] + counts_table(by_dir, "directory") + [""]
    out += ["## By file", ""] + counts_table(by_file, "file") + [""]
    out += ["## By suite", ""] + counts_table(by_suite, "file:suite") + [""]
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--csv", type=Path, help="write the per-case CSV here")
    parser.add_argument("--summary", type=Path, help="write the count tables here")
    parser.add_argument("--check", action="store_true",
                        help="fail if --csv/--summary differ from a fresh run")
    args = parser.parse_args(argv)
    rows = inventory(args.repo.resolve())
    outputs = {args.csv: write_csv(rows), args.summary: summary(rows)}
    stale = []
    for path, content in outputs.items():
        if path is None:
            continue
        if args.check:
            if not path.is_file() or path.read_text(encoding="utf-8") != content:
                stale.append(str(path))
        else:
            path.write_text(content, encoding="utf-8")
    if stale:
        print("stale inventory: " + ", ".join(stale), file=sys.stderr)
        return 1
    if args.csv is None and args.summary is None:
        sys.stdout.write(summary(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
