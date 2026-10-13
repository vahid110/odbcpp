#!/usr/bin/env python3
"""Cheap changed-source GoogleTest control check, not a C++ compiler.

Checks explicit if/else/for/while bodies and readable control lines in changed
code. Reads whole-file context, including nested/multiline statements, but
ignores unrelated historical controls. Comments and literals are not code.
Preprocessor-generated controls and unsupported changed statement forms need
manual review. No installs, builds, compiler flags or CI admission are involved.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import difflib
import json
from pathlib import Path
import re
import subprocess


CONTROL = {"if", "else", "for", "while"}
MACRO = re.compile(r"(?:ASSERT|EXPECT)_[A-Z0-9_]+|ADD_FAILURE(?:_AT)?|FAIL|SUCCEED|GTEST_SKIP|SCOPED_TRACE")
LEX = re.compile(
    r'(?P<comment>//[^\n]*|/\*.*?\*/)'
    r'|(?P<directive>^[ \t]*\#(?:[^\n]*\\\n)*[^\n]*)'
    r'|(?P<literal>(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\]{0,16})\(.*?\)(?P=delimiter)"'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|(?:u8|u|U|L)?\'(?:\\.|[^\'\\])*\')'
    r'|(?P<number>\d[\w\x27.]*)|(?P<word>[A-Za-z_]\w*)|(?P<punct>[^\s])',
    re.MULTILINE | re.DOTALL,
)


@dataclass(frozen=True)
class Token:
    text: str
    kind: str
    start: int
    end: int
    line: int
    last_line: int


@dataclass(frozen=True)
class Finding:
    line: int
    control: str
    reason: str
    manual_review: bool = False


def tokens(source: str) -> list[Token]:
    result = []
    line, previous = 1, 0
    for match in LEX.finditer(source):
        line += source.count("\n", previous, match.start())
        last = line + match.group().count("\n")
        if match.lastgroup != "comment":
            result.append(Token(match.group(), match.lastgroup, match.start(), match.end(), line, last))
        line, previous = last, match.end()
    return result


def changed_code_lines(old: str, new: str, diff: str | None = None) -> set[int]:
    """Map added code and deleted code anchors; comment-only edits stay quiet."""
    hunks, added, deleted = [], set(), []
    old_line = new_line = 0
    lines = diff.splitlines() if diff is not None else difflib.unified_diff(old.splitlines(), new.splitlines(), n=0)
    for line in lines:
        header = re.match(r"@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@", line)
        if header:
            if added or deleted:
                hunks.append((added, deleted))
            added, deleted = set(), []
            old_line, new_line = map(int, header.groups())
        elif line.startswith("+") and not line.startswith("+++"):
            added.add(new_line)
            new_line += 1
        elif line.startswith("-") and not line.startswith("---"):
            deleted.append((old_line, max(1, new_line)))
            old_line += 1
        elif line.startswith(" "):
            old_line += 1
            new_line += 1
    if added or deleted:
        hunks.append((added, deleted))
    old_tokens, new_tokens = tokens(old), tokens(new)

    def comment_lines(source):
        return {line for m in LEX.finditer(source) if m.lastgroup == "comment"
                for line in range(source.count("\n", 0, m.start()) + 1,
                                  source.count("\n", 0, m.end()) + 2)}

    old_comments, new_comments = comment_lines(old), comment_lines(new)

    def line_index(ts):
        result = {}
        for token in ts:
            for line in range(token.line, token.last_line + 1):
                result.setdefault(line, []).append(token)
        return result

    def selected(index, lines):
        by_position = {t.start: t for line in lines for t in index.get(line, [])}
        return [by_position[pos] for pos in sorted(by_position)]

    old_index, new_index = line_index(old_tokens), line_index(new_tokens)
    changed = set()
    for added, deleted in hunks:
        removed = {line for line, _ in deleted}
        before, after = selected(old_index, removed), selected(new_index, added)
        signature = lambda ts: [(t.kind, t.text) for t in ts]
        if before and signature(before) == signature(after):
            # Preserve comment/whitespace-only history, but catch newly joined
            # code lines that lose readable control boundaries.
            if not (removed & old_comments or added & new_comments) and len(added) < len(removed):
                changed.update(added)
            continue
        changed.update(line for t in after for line in range(t.line, t.last_line + 1) if line in added)
        code_removed = {line for t in before for line in range(t.line, t.last_line + 1)}
        changed.update(anchor for line, anchor in deleted if line in code_removed)
    return changed


class Statements:
    """Only balanced groups and the four selected controls; no C++ AST claim."""

    def __init__(self, source: str):
        self.all_tokens = tokens(source)
        self.code = [t for t in self.all_tokens if t.kind != "directive"]
        self.pairs = {}
        stack = []
        for i, token in enumerate(self.code):
            if token.kind == "punct" and token.text in "([{":
                stack.append(i)
            elif token.kind == "punct" and token.text in ")]}":
                if stack and self.code[stack[-1]].text == {")": "(", "]": "[", "}": "{"}[token.text]:
                    self.pairs[stack.pop()] = i

    def body(self, i: int) -> int:
        kind = self.code[i].text
        j = i + 1
        if kind == "else":
            return j
        if kind == "if" and j < len(self.code) and self.code[j].text == "constexpr":
            j += 1
        if j not in self.pairs or self.code[j].text != "(":
            raise ValueError("unsupported or unbalanced control header")
        return self.pairs[j] + 1

    def end(self, i: int) -> int:
        if i >= len(self.code):
            raise ValueError("missing control body")
        text = self.code[i].text
        if text == "{":
            if i not in self.pairs:
                raise ValueError("unbalanced compound body")
            return self.pairs[i] + 1
        if text in CONTROL:
            end = self.end(self.body(i))
            if text == "if" and end < len(self.code) and self.code[end].text == "else":
                end = self.end(end + 1)
            return end
        if text in {"do", "switch", "try", "catch", "["}:
            raise ValueError("unsupported statement prefix; inspect its exact scope")
        j = i
        while j < len(self.code):
            if self.code[j].text == ";":
                return j + 1
            if self.code[j].text == "}":
                break
            if self.code[j].text in "([{":
                if j not in self.pairs:
                    break
                j = self.pairs[j]
            j += 1
        raise ValueError("unsupported or unterminated statement")

    def contains_macro(self, begin: int, end: int) -> bool:
        return any(t.kind == "word" and MACRO.fullmatch(t.text) and
                   i + 1 < end and self.code[i + 1].text == "("
                   for i, t in enumerate(self.code[begin:end], begin))

    def recovery_end(self, begin: int) -> int:
        """Bound an unsupported statement for reporting, never accept it."""
        j = begin
        while j < len(self.code):
            if self.code[j].text == ";":
                return j + 1
            if j in self.pairs:
                if self.code[j].text == "{":
                    return self.pairs[j] + 1
                j = self.pairs[j]
            j += 1
        return len(self.code)

    def changed(self, begin: int, end: int, lines: set[int]) -> bool:
        return any(any(line in lines for line in range(t.line, t.last_line + 1))
                   for t in self.code[begin:end])


def inspect(source: str, changed: set[int]) -> tuple[list[Finding], list[dict]]:
    statements = Statements(source)
    code, findings, inventory = statements.code, [], []
    for token in statements.all_tokens:
        if token.kind == "directive" and any(line in changed for line in range(token.line, token.last_line + 1)):
            inner = tokens(token.text.lstrip()[1:])
            if any(t.kind == "word" and MACRO.fullmatch(t.text) and
                   i + 1 < len(inner) and inner[i + 1].text == "("
                   for i, t in enumerate(inner)):
                findings.append(Finding(token.line, "preprocessor", "changed assertion macro definition needs manual review", True))
    for i, token in enumerate(code):
        if token.kind != "word" or token.text not in CONTROL:
            continue
        try:
            begin = statements.body(i)
            end = statements.end(begin)
        except ValueError as error:
            # No guessed pass for changed syntax that this small checker cannot parse.
            end = statements.recovery_end(i + 1)
            if statements.changed(i, end, changed) and (
                    statements.contains_macro(i, end) or token.line in changed):
                findings.append(Finding(token.line, token.text, str(error), True))
            continue
        if not statements.contains_macro(begin, end) or not statements.changed(i, end, changed):
            continue
        braced = code[begin].text == "{"
        inventory.append({"line": token.line, "control": token.text, "body_line": code[begin].line,
                          "last_line": code[end - 1].last_line, "braced": braced})
        if not braced:
            findings.append(Finding(token.line, token.text, "GoogleTest-containing body requires explicit braces"))
        j, prefix = i - 1, []
        while j >= 0 and code[j].last_line == token.line:
            prefix.append(code[j].text)
            j -= 1
        if prefix and not (token.text == "else" and prefix == ["}"]):
            findings.append(Finding(token.line, token.text, "start each control statement on a readable line"))
        if any(t.kind == "word" and t.text in CONTROL and t.line == token.line
               for t in code[i + 1:end]):
            findings.append(Finding(token.line, token.text, "separate control statements onto readable lines"))
        if braced and code[begin].line == code[end - 1].line:
            findings.append(Finding(token.line, token.text, "put the assertion-containing compound body on separate lines"))
    return findings, inventory


def check_change(old: str, new: str, diff: str | None = None) -> tuple[list[Finding], list[dict]]:
    return inspect(new, changed_code_lines(old, new, diff))


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "--literal-pathspecs", "-C", str(root), *args])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="explicit review/authoring baseline Git ref")
    parser.add_argument("--head", help="committed candidate ref; omit for working source including untracked C++")
    parser.add_argument("--json", action="store_true", help="include changed control inventory")
    parser.add_argument("paths", nargs="*", help="optional literal source paths")
    args = parser.parse_args()
    root = Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())
    refs = [args.base] + ([args.head] if args.head else [])
    paths = set(p.decode() for p in git(root, "diff", "--name-only", "--no-renames", "-z", *refs,
                                       "--", *args.paths).split(b"\0") if p)
    if not args.head:
        paths.update(p.decode() for p in git(root, "ls-files", "--others", "--exclude-standard", "-z",
                                            "--", *args.paths).split(b"\0") if p)
    result = []
    for path in sorted(paths):
        if Path(path).suffix not in {".cpp", ".cc", ".cxx", ".h", ".hpp"}:
            continue
        try:
            old = git(root, "show", args.base + ":" + path).decode()
        except subprocess.CalledProcessError:
            old = ""  # New file; all of its controls are in scope.
        if args.head:
            try:
                new = git(root, "show", args.head + ":" + path).decode()
            except subprocess.CalledProcessError:
                continue  # Deleted file.
        else:
            file = root / path
            if not file.exists():
                continue
            new = file.read_text()
        diff = git(root, "diff", "--unified=0", "--no-renames", *refs, "--", path).decode()
        findings, controls = check_change(old, new, diff if old else None)
        result.append({"file": path, "findings": [asdict(f) for f in findings], "controls": controls})
    count = sum(len(r["findings"]) for r in result)
    if args.json:
        print(json.dumps({"files": result, "findings": count,
                          "scope": "changed source only; not compiler/GCC qualification"}, indent=2))
    else:
        for row in result:
            for finding in row["findings"]:
                print(f"{row['file']}:{finding['line']}: {finding['control']}: {finding['reason']}")
        print(f"Changed GoogleTest control check: {count} findings in {len(result)} files (not compiler proof).")
    return 1 if count else 0


if __name__ == "__main__":
    raise SystemExit(main())
