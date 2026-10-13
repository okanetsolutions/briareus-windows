#!/usr/bin/env python3
"""Reject new structure violations against a Git merge base."""

import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


PRESENTATION_LIMIT = 300
CONDITION_LINES = 3
CONDITION_OPERANDS = 5
SOURCE_SUFFIXES = {".c", ".h", ".cpp"}
COMMENT_PATTERN = r'//(?:\\\r?\n|[^\n])*|/\*[\s\S]*?\*/'
LITERAL_PATTERN = r'"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\''
COMMENTS_AND_LITERALS = re.compile(f'(?P<comment>{COMMENT_PATTERN})|{LITERAL_PATTERN}')
TOKEN = re.compile(
    r'(?P<directive>^[ \t]*\#(?:/\*[\s\S]*?\*/|\\\r?\n|[^\n])*)'
    f'|(?P<comment>{COMMENT_PATTERN})'
    f'|(?P<literal>{LITERAL_PATTERN})'
    r'|(?P<space>\s+)'
    r'|(?P<word>[A-Za-z_][A-Za-z_0-9]*|[0-9](?:[A-Za-z_0-9.]|(?<=[eEpP])[+-])*'
    r'|>>=|<<=|\.\.\.|&&|\|\||\+\+|--|->|<<|>>|<=|>=|==|!=|[+*/%&|^!-]=|[^\s])',
    re.MULTILINE,
)
API_CALLS = {"api_call", "api_discovery", "api_catalog", "api_stream", "api_transcribe", "api_upload"}
UI_HEADERS = {"commctrl.h", "commdlg.h", "d2d1.h", "dwrite.h", "dwmapi.h", "uxtheme.h"}


@dataclass(frozen=True)
class Token:
    value: str
    line: int


@dataclass(frozen=True)
class Finding:
    rule: str
    signature: str
    line: int
    message: str


def tokens(source, include_directives=False):
    result = []
    line = 1
    for match in TOKEN.finditer(source):
        if match.lastgroup in {"literal", "word"}:
            result.append(Token(match.group(), line))
        elif include_directives and match.lastgroup == "directive":
            result.append(Token("#", line))
            result.extend(tokens(match.group().lstrip()[1:]))
        line += match.group().count("\n")
    return result


def token_values(source):
    return tuple(token.value for token in tokens(source, include_directives=True))


def pairs(items):
    stack = []
    result = {}
    for index, token in enumerate(items):
        if token.value in {"(", "[", "{"}:
            stack.append(index)
        elif token.value in {")", "]", "}"} and stack:
            opening = stack.pop()
            if items[opening].value == {")": "(", "]": "[", "}": "{"}[token.value]:
                result[opening] = index
    return result


def operand_count(items, matching, start, end):
    count = 1
    index = start
    while index < end:
        value = items[index].value
        is_call = value == "(" and index > start and re.fullmatch(r"[A-Za-z_]\w*", items[index - 1].value)
        if value in {"[", "{"} or is_call:
            index = matching.get(index, index) + 1
            continue
        if value in {"&&", "||"}:
            count += 1
        index += 1
    return count


def condition_findings(items):
    matching = pairs(items)
    for index in range(len(items) - 2):
        if items[index].value != "if" or items[index + 1].value != "(":
            continue
        start = index + 2
        end = matching.get(index + 1, start)
        if start >= end:
            continue
        signature = " ".join(token.value for token in items[start:end])
        lines = items[end - 1].line - items[start].line + 1
        count = operand_count(items, matching, start, end)
        if lines > CONDITION_LINES:
            yield Finding("condition-lines", f"{lines}: {signature}", items[start].line,
                          f"if condition spans {lines} lines (max {CONDITION_LINES}); extract a named predicate or early returns")
        if count > CONDITION_OPERANDS:
            yield Finding("condition-operands", signature, items[start].line,
                          f"if condition has {count} logical operands (max {CONDITION_OPERANDS}); split the decision")


def findings(path, source, app_headers):
    items = tokens(source)
    result = list(condition_findings(items))
    without_comments = COMMENTS_AND_LITERALS.sub(
        lambda match: "\n" * match.group().count("\n") if match.lastgroup == "comment" else match.group(), source
    )
    for match in re.finditer(r'^[ \t]*#\s*include\s*[<"]([^">]+)[">]', without_comments, re.MULTILINE):
        header = match[1].replace("\\", "/")
        name = PurePosixPath(header).name.lower()
        if path.startswith("core/") and (name in app_headers or name in UI_HEADERS or "app" in header.split("/")):
            result.append(Finding("core-ui", header, without_comments[:match.start()].count("\n") + 1,
                                  f"core includes UI header {header}; move UI ownership into app"))
    if path.startswith("app/"):
        for index, token in enumerate(items[:-1]):
            if items[index + 1].value != "(":
                continue
            if token.value in API_CALLS and path != "app/store.c":
                result.append(Finding("direct-api", token.value, token.line,
                                      f"{token.value} bypasses store; use a store request helper"))
            if token.value.startswith("WinHttp") and path != "app/ws.c":
                result.append(Finding("direct-http", token.value, token.line,
                                      "WinHTTP belongs in core transport or app/ws.c, not screen/app helpers"))
    return result


def source_suppressions(source):
    count = 0
    for match in TOKEN.finditer(source):
        if match.lastgroup == "comment":
            count += len(re.findall(r"\bcppcheck-suppress(?:-\w+)?\b", match.group()))
        elif match.lastgroup == "directive":
            if re.search(r"#\s*pragma\s+(?:(?:GCC|clang)\s+diagnostic\s+ignored|warning\s*\(\s*(?:disable\s*:|push\s*,\s*0))", match.group()):
                count += 1
    return count


def production_source(path):
    return path.startswith(("core/", "app/")) and PurePosixPath(path).suffix in SOURCE_SUFFIXES


def presentation(path):
    return path.startswith(("app/screen_", "app/project_")) and path.endswith(".c") or path == "app/forge_site.c"


def check(before, after):
    errors = []
    app_headers = {PurePosixPath(path).name.lower() for path in before.keys() | after.keys()
                   if path.startswith("app/") and path.endswith(".h")}
    changed = {path for path in before.keys() | after.keys() if before.get(path) != after.get(path)}
    core_changes = []
    for path in sorted(changed):
        old, new = before.get(path, ""), after.get(path, "")
        if presentation(path) and len(new.splitlines()) > max(PRESENTATION_LIMIT, len(old.splitlines())):
            errors.append(f"{path}:1: presentation file grew to {len(new.splitlines())} lines; extract cohesive logic (max 300, existing debt may not grow)")
        if not production_source(path):
            continue
        previous = Counter((item.rule, item.signature) for item in findings(path, old, app_headers))
        for item in findings(path, new, app_headers):
            key = item.rule, item.signature
            if previous[key]:
                previous[key] -= 1
            else:
                errors.append(f"{path}:{item.line}: {item.message}")
        if source_suppressions(new) > source_suppressions(old):
            errors.append(f"{path}:1: warning suppressions increased; fix the cause instead of suppressing it")
        if path.startswith("core/") and token_values(old) != token_values(new):
            core_changes.append(path)
    test_changes = any(
        re.fullmatch(r"tests/core_.+tests\.c", path) and path in after
        and token_values(before.get(path, "")) != token_values(after[path])
        for path in changed
    )
    if core_changes and not test_changes:
        errors.append("core test gate: add or update a meaningful tests/core_*tests.c suite for " + ", ".join(core_changes))
    if "Makefile" in changed:
        old, new = (re.sub(r"(?m)#.*$", "", snapshot.get("Makefile", "")) for snapshot in (before, after))
        suppressions = lambda text: Counter(re.findall(r"--suppress(?:=|\s+)([^\s]+)", text))
        if suppressions(new) - suppressions(old):
            errors.append("Makefile:1: cppcheck suppressions widened; fix the finding")
        floor = lambda text: re.search(r"^COVERAGE_MIN\s*[?:+]?=\s*(\d+)\s*$", text, re.MULTILINE)
        old_floor, new_floor = floor(old), floor(new)
        if old_floor and (not new_floor or int(new_floor[1]) < int(old_floor[1])):
            errors.append("Makefile:1: COVERAGE_MIN was removed or lowered; preserve the coverage floor")
    return errors


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout.decode("utf-8")


def relevant(path):
    return path == "Makefile" or production_source(path) or bool(re.fullmatch(r"tests/core_.+tests\.c", path))


def snapshots(root, base):
    git(root, "rev-parse", "--verify", base + "^{commit}")
    ancestor = git(root, "merge-base", "HEAD", base).strip()
    before = {path: git(root, "show", f"{ancestor}:{path}")
              for path in git(root, "ls-tree", "-r", "--name-only", "-z", ancestor).split("\0") if relevant(path)}
    paths = set(git(root, "ls-files", "--cached", "--others", "--exclude-standard", "-z").split("\0"))
    after = {path: (root / path).read_text(encoding="utf-8") for path in paths
             if relevant(path) and (root / path).is_file()}
    return ancestor, before, after


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", nargs="?", default="origin/main")
    args = parser.parse_args()
    try:
        root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel").strip())
        ancestor, before, after = snapshots(root, args.base)
        errors = check(before, after)
    except (subprocess.CalledProcessError, OSError, UnicodeError) as error:
        detail = error.stderr.decode("utf-8", errors="replace") if isinstance(error, subprocess.CalledProcessError) else str(error)
        print(f"Structure check could not run: {detail.strip()}", file=sys.stderr)
        return 2
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Structure checks passed against {args.base} (merge base {ancestor[:12]}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
