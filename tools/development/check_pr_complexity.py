#!/usr/bin/env python3
"""Pull-request code complexity (Lizard) and duplication (jscpd) gates.

Enforces:
1. Cyclomatic Complexity Number (CCN) <= 25 and NLOC <= 100 for newly introduced
   or modified functions in changed files.
2. Code duplication <= 50 tokens for newly introduced or modified code blocks.

Only flags changed code in incoming PRs; untouched legacy code stays quiet.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any

DEFAULT_CCN_THRESHOLD = 25
DEFAULT_NLOC_THRESHOLD = 100
DEFAULT_MIN_TOKENS = 50
PINNED_JSCPD_VERSION = "jscpd@5.4.3"

SUPPORTED_EXTENSIONS = {
    ".cpp",
    ".cc",
    ".cxx",
    ".h",
    ".hpp",
    ".py",
    ".cs",
}

# Strictly match comments preceding or on the function definition line
PRECEDING_COMMENT_PATTERN = re.compile(
    r"^\s*(?://|/\*+|#)\s*(?:NOLINT\s*\(\s*complexity\s*\)|lizard\s*:\s*ignore)",
    re.IGNORECASE,
)
SIGNATURE_COMMENT_PATTERN = re.compile(
    r"(?://|/\*+|#)\s*(?:NOLINT\s*\(\s*complexity\s*\)|lizard\s*:\s*ignore)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Finding:
    file: str
    line: int
    metric: str
    metric_value: int
    threshold: int
    reason: str


def is_supported_source_file(path: str) -> bool:
    """Return True if path has an analyzed source extension."""
    suffix = Path(path).suffix.lower()
    return suffix in SUPPORTED_EXTENSIONS


def parse_changed_lines(diff: str) -> set[int]:
    """Extract 1-based line numbers added or modified in the new file from a unified diff."""
    changed = set()
    current_line = 0
    for line in diff.splitlines():
        if line.startswith("@@"):
            match = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", line)
            if match:
                old_count = int(match.group(2)) if match.group(2) else 1
                new_start = int(match.group(3))
                new_count = int(match.group(4)) if match.group(4) else 1
                current_line = new_start
                if old_count > 0 and new_count == 0:
                    # Pure deletion hunk; attribute deletion anchor to enclosing line
                    changed.add(max(1, new_start))
        elif line.startswith("+") and not line.startswith("+++"):
            changed.add(current_line)
            current_line += 1
        elif line.startswith("-") and not line.startswith("---"):
            pass
        elif line.startswith(" "):
            current_line += 1
    return changed


def has_exemption(code_lines: list[str], start_line: int) -> bool:
    """Check if the function definition line or the non-blank line immediately preceding it has an exemption."""
    # Check signature line (1-based index)
    if 1 <= start_line <= len(code_lines):
        if SIGNATURE_COMMENT_PATTERN.search(code_lines[start_line - 1]):
            return True

    # Check non-blank lines directly preceding start_line
    line_idx = start_line - 2
    while line_idx >= 0 and line_idx >= start_line - 4:
        stripped = code_lines[line_idx].strip()
        if not stripped:
            line_idx -= 1
            continue
        if PRECEDING_COMMENT_PATTERN.search(code_lines[line_idx]):
            return True
        # If preceding non-blank line is not an exemption comment, do not scan further
        break

    return False


def check_source_complexity(
    file_path: str,
    code: str,
    changed_lines: set[int],
    ccn_limit: int = DEFAULT_CCN_THRESHOLD,
    nloc_limit: int = DEFAULT_NLOC_THRESHOLD,
) -> list[Finding]:
    """Run Lizard complexity analysis on source code and flag changed functions exceeding limits."""
    try:
        import lizard
    except ImportError:
        sys.stderr.write(
            "ERROR: 'lizard' Python package is not installed.\n"
            "Install it via: python3 -m pip install lizard==1.26.0\n"
        )
        raise

    findings: list[Finding] = []
    file_info = lizard.analyze_file.analyze_source_code(file_path, code)
    code_lines = code.splitlines()

    for func in file_info.function_list:
        func_lines = set(range(func.start_line, func.end_line + 1))
        if not (func_lines & changed_lines):
            continue

        if has_exemption(code_lines, func.start_line):
            continue

        if func.cyclomatic_complexity > ccn_limit:
            findings.append(
                Finding(
                    file=file_path,
                    line=func.start_line,
                    metric="CCN",
                    metric_value=func.cyclomatic_complexity,
                    threshold=ccn_limit,
                    reason=(
                        f"Function '{func.name}' has Cyclomatic Complexity Number (CCN) "
                        f"{func.cyclomatic_complexity} > {ccn_limit}"
                    ),
                )
            )

        if func.nloc > nloc_limit:
            findings.append(
                Finding(
                    file=file_path,
                    line=func.start_line,
                    metric="NLOC",
                    metric_value=func.nloc,
                    threshold=nloc_limit,
                    reason=(
                        f"Function '{func.name}' has NLOC "
                        f"{func.nloc} > {nloc_limit}"
                    ),
                )
            )

    return findings


def normalize_repo_path(raw_name: str, root: Path) -> str:
    """Normalize file path relative to repo root."""
    path_obj = Path(raw_name)
    if path_obj.is_absolute():
        try:
            return str(path_obj.relative_to(root))
        except ValueError:
            return str(path_obj)
    norm = os.path.relpath(root / path_obj, start=str(root))
    return norm[2:] if norm.startswith("./") else norm


def evaluate_duplication_report(
    report: dict[str, Any],
    changed_by_file: dict[str, set[int]],
    min_tokens: int = DEFAULT_MIN_TOKENS,
    root: Path | None = None,
) -> list[Finding]:
    """Evaluate jscpd JSON report and flag duplicate blocks that intersect changed lines."""
    findings: list[Finding] = []
    seen = set()
    repo_root = root or Path.cwd()

    for dup in report.get("duplicates", []):
        tokens = dup.get("tokens", 0)
        if tokens < min_tokens:
            continue

        first = dup.get("firstFile", {})
        second = dup.get("secondFile", {})

        for current, other in ((first, second), (second, first)):
            curr_name = current.get("name", "")
            norm_name = normalize_repo_path(curr_name, repo_root)

            if norm_name not in changed_by_file:
                continue

            curr_start = current.get("start", 0)
            curr_end = current.get("end", 0)
            dup_range = set(range(curr_start, curr_end + 1))

            if dup_range & changed_by_file[norm_name]:
                other_name = normalize_repo_path(other.get("name", ""), repo_root)
                other_start = other.get("start", 0)
                other_end = other.get("end", 0)
                key = (norm_name, curr_start, curr_end, other_name, other_start)
                if key in seen:
                    continue
                seen.add(key)

                findings.append(
                    Finding(
                        file=norm_name,
                        line=curr_start,
                        metric="DUPLICATION",
                        metric_value=tokens,
                        threshold=min_tokens,
                        reason=(
                            f"Duplicate code block of {tokens} tokens ({dup.get('lines', 0)} lines) "
                            f"duplicates {other_name}:{other_start}-{other_end}"
                        ),
                    )
                )

    return findings


def run_jscpd(
    root: Path,
    min_tokens: int = DEFAULT_MIN_TOKENS,
    timeout_seconds: int = 45,
) -> dict[str, Any]:
    """Execute jscpd via pinned npx or local binary and return parsed JSON report, failing closed on errors."""
    jscpd_bin = shutil.which("jscpd")
    npx_bin = shutil.which("npx")

    if not jscpd_bin and not npx_bin:
        raise RuntimeError(
            "Neither 'jscpd' nor 'npx' was found on PATH. "
            "Install Node.js or run with --skip-duplication if intentionally omitting."
        )

    with tempfile.TemporaryDirectory() as tmpdir:
        output_dir = Path(tmpdir) / "jscpd-output"
        cmd = [jscpd_bin] if jscpd_bin else [npx_bin, "--yes", PINNED_JSCPD_VERSION]

        cmd.extend([
            str(root),
            "-k", str(min_tokens),
            "--ignore", "**/.git/**,build,build-*/**,build/**,**/.codex/**,**/.gemini/**,**/worktrees/**,**/node_modules/**,**/tests/fixtures/**",
            "-r", "json",
            "--output", str(output_dir),
            "--silent",
            "--no-tips",
        ])

        try:
            proc = subprocess.run(
                cmd,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=str(root),
                timeout=timeout_seconds,
                text=True,
            )
        except subprocess.TimeoutExpired as e:
            raise RuntimeError(f"jscpd execution timed out after {timeout_seconds} seconds") from e
        except OSError as e:
            raise RuntimeError(f"Failed to execute jscpd command ({cmd[0]}): {e}") from e

        report_file = output_dir / "jscpd-report.json"
        if not report_file.exists():
            raise RuntimeError(
                f"jscpd did not generate a report file (exit code {proc.returncode}): "
                f"{proc.stderr.strip() or proc.stdout.strip() or 'unknown error'}"
            )

        try:
            return json.loads(report_file.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError) as e:
            raise RuntimeError(f"Failed to parse jscpd report JSON: {e}") from e


def git_cmd(root: Path, *args: str) -> str:
    """Run git command in root directory and return string output."""
    return subprocess.check_output(
        ["git", "--literal-pathspecs", "-C", str(root), *args],
        text=True,
    )


def resolve_base_ref(root: Path, base_arg: str | None) -> str:
    """Determine effective base Git ref for comparisons."""
    if base_arg:
        return base_arg

    for candidate in ("origin/main", "main", "HEAD~1"):
        try:
            git_cmd(root, "rev-parse", "--verify", candidate)
            return candidate
        except subprocess.CalledProcessError:
            continue

    raise RuntimeError("Could not resolve a default Git base ref; please provide --base explicitly.")


def file_exists_at_ref(root: Path, ref: str, path_str: str) -> bool:
    """Check if file existed at the given Git ref."""
    try:
        subprocess.check_output(
            ["git", "--literal-pathspecs", "-C", str(root), "cat-file", "-e", f"{ref}:{path_str}"],
            stderr=subprocess.DEVNULL,
        )
        return True
    except subprocess.CalledProcessError:
        return False


def collect_changed_files(
    root: Path,
    base_ref: str,
    head_ref: str | None,
    paths: list[str],
) -> tuple[dict[str, set[int]], list[str]]:
    """Determine changed source files and changed line numbers."""
    refs = [base_ref] + ([head_ref] if head_ref else [])

    diff_args = ["diff", "--name-only", "--no-renames", "-z", *refs]
    if paths:
        diff_args.extend(["--", *paths])

    raw_paths = git_cmd(root, *diff_args).split("\0")
    changed_paths = {p for p in raw_paths if p}

    if not head_ref:
        ls_args = ["ls-files", "--others", "--exclude-standard", "-z"]
        if paths:
            ls_args.extend(["--", *paths])
        raw_untracked = git_cmd(root, *ls_args).split("\0")
        changed_paths.update(p for p in raw_untracked if p)

    changed_by_file: dict[str, set[int]] = {}
    source_files: list[str] = []

    for path_str in sorted(changed_paths):
        if not is_supported_source_file(path_str):
            continue

        target_path = root / path_str
        if not target_path.exists():
            # File was deleted in this change
            continue

        source_files.append(path_str)
        try:
            file_diff = git_cmd(root, "diff", "-U0", "--no-renames", *refs, "--", path_str)
            changed_lines = parse_changed_lines(file_diff)
        except subprocess.CalledProcessError:
            changed_lines = set()

        # Check if file was newly created in this change
        if not file_exists_at_ref(root, base_ref, path_str):
            try:
                num_lines = len(target_path.read_text(encoding="utf-8", errors="replace").splitlines())
                changed_lines = set(range(1, num_lines + 1))
            except OSError:
                changed_lines = set()

        changed_by_file[path_str] = changed_lines

    return changed_by_file, source_files


def run_complexity_checks(
    root: Path,
    source_files: list[str],
    changed_by_file: dict[str, set[int]],
    ccn_limit: int,
    nloc_limit: int,
) -> list[Finding]:
    """Execute Lizard complexity checks across all changed files."""
    findings: list[Finding] = []
    for path_str in source_files:
        target_file = root / path_str
        if not target_file.exists():
            continue

        try:
            code = target_file.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        file_findings = check_source_complexity(
            path_str,
            code,
            changed_by_file[path_str],
            ccn_limit=ccn_limit,
            nloc_limit=nloc_limit,
        )
        findings.extend(file_findings)
    return findings


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--base",
        help="Base Git ref to compare against (default: auto-detected origin/main or HEAD~1)",
    )
    parser.add_argument(
        "--head",
        help="Committed head ref; omit to compare working directory / staged files",
    )
    parser.add_argument(
        "--ccn",
        type=int,
        default=DEFAULT_CCN_THRESHOLD,
        help=f"Cyclomatic Complexity Number (CCN) limit (default: {DEFAULT_CCN_THRESHOLD})",
    )
    parser.add_argument(
        "--nloc",
        type=int,
        default=DEFAULT_NLOC_THRESHOLD,
        help=f"Lines of code (NLOC) limit per function (default: {DEFAULT_NLOC_THRESHOLD})",
    )
    parser.add_argument(
        "--min-tokens",
        type=int,
        default=DEFAULT_MIN_TOKENS,
        help=f"Minimum tokens for duplication check (default: {DEFAULT_MIN_TOKENS})",
    )
    parser.add_argument(
        "--skip-complexity",
        action="store_true",
        help="Skip Lizard cyclomatic complexity checks",
    )
    parser.add_argument(
        "--skip-duplication",
        action="store_true",
        help="Skip jscpd duplicate code checks",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output findings as JSON",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        help="Optional literal paths to restrict analysis",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    try:
        root = Path(git_cmd(Path.cwd(), "rev-parse", "--show-toplevel").strip())
    except subprocess.CalledProcessError:
        root = Path.cwd()

    base_ref = resolve_base_ref(root, args.base)
    changed_by_file, source_files = collect_changed_files(root, base_ref, args.head, args.paths)

    all_findings: list[Finding] = []

    # 1. Lizard complexity
    if not args.skip_complexity:
        findings = run_complexity_checks(root, source_files, changed_by_file, args.ccn, args.nloc)
        all_findings.extend(findings)

    # 2. jscpd duplication
    if not args.skip_duplication and changed_by_file:
        try:
            jscpd_report = run_jscpd(root, min_tokens=args.min_tokens)
            dup_findings = evaluate_duplication_report(
                jscpd_report,
                changed_by_file,
                min_tokens=args.min_tokens,
                root=root,
            )
            all_findings.extend(dup_findings)
        except RuntimeError as e:
            all_findings.append(
                Finding(
                    file="jscpd",
                    line=1,
                    metric="ERROR",
                    metric_value=0,
                    threshold=0,
                    reason=f"Duplication gate execution error: {e}",
                )
            )

    if args.json:
        payload = {
            "findings": [asdict(f) for f in all_findings],
            "total_findings": len(all_findings),
            "files_checked": len(source_files),
            "base": base_ref,
            "head": args.head or "working_tree",
        }
        print(json.dumps(payload, indent=2))
    else:
        for f in all_findings:
            print(f"{f.file}:{f.line}: [{f.metric}] {f.reason}")

        if all_findings:
            print(
                f"\nPR Complexity & Duplication Gate: FAILED with {len(all_findings)} finding(s) "
                f"across {len(source_files)} checked file(s)."
            )
        else:
            print(
                f"PR Complexity & Duplication Gate: PASSED ({len(source_files)} checked file(s), 0 findings)."
            )

    return 1 if all_findings else 0


if __name__ == "__main__":
    sys.exit(main())
