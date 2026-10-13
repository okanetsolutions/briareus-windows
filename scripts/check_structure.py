#!/usr/bin/env python3
"""Reject new structure violations against a Git merge base."""

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import sys


PRESENTATION_LIMIT = 300
CONDITION_LINES = 3
CONDITION_OPERANDS = 5
SOURCE_SUFFIXES = {".c", ".h", ".cpp"}
BLOCK_COMMENT_PATTERN = r'/\*(?:[^*]|\*(?!/))*\*/'
COMMENT_PATTERN = rf'//(?:\\\r?\n|[^\n])*|{BLOCK_COMMENT_PATTERN}'
LITERAL_PATTERN = r'"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\''
COMMENTS_AND_LITERALS = re.compile(f'(?P<comment>{COMMENT_PATTERN})|{LITERAL_PATTERN}')
TOKEN = re.compile(
    rf'(?P<directive>^[ \t]*(?:{BLOCK_COMMENT_PATTERN}\s*)*(?P<hash>\#)(?:{BLOCK_COMMENT_PATTERN}|\\\r?\n|[^\n])*)'
    f'|(?P<comment>{COMMENT_PATTERN})'
    f'|(?P<literal>{LITERAL_PATTERN})'
    r'|(?P<space>[^\S\n]+|\n)'
    r'|(?P<word>[A-Za-z_][A-Za-z_0-9]*|[0-9](?:[A-Za-z_0-9.]|(?<=[eEpP])[+-])*'
    r'|>>=|<<=|\.\.\.|&&|\|\||\+\+|--|->|<<|>>|<=|>=|==|!=|[+*/%&|^!-]=|[^\s])',
    re.MULTILINE,
)
API_CALLS = {"api_call", "api_discovery", "api_catalog", "api_stream", "api_transcribe", "api_upload"}
UI_HEADERS = {"commctrl.h", "commdlg.h", "d2d1.h", "dwrite.h", "dwmapi.h", "uxtheme.h"}
BUILTIN_TYPES = {"void", "char", "short", "int", "long", "signed", "unsigned", "float", "double",
                 "_Bool", "_Complex", "_Imaginary", "bool", "wchar_t", "char16_t", "char32_t"}


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
    size: int = 0


def tokens(source, include_directives=False):
    result = []
    line = 1
    for match in TOKEN.finditer(source):
        if match.lastgroup in {"literal", "word"}:
            result.append(Token(match.group(), line))
        elif include_directives and match.lastgroup == "directive":
            result.append(Token("#", line))
            logical = re.sub(r"\\\r?\n", "", source[match.start("hash"):match.end()])
            logical = COMMENTS_AND_LITERALS.sub(
                lambda item: " " if item.lastgroup == "comment" else item.group(), logical
            )
            result.extend(tokens(logical.lstrip()[1:]))
            if re.match(r"#\s*define\s+[A-Za-z_]\w*\(", logical):
                result.append(Token("<function-like-macro>", line))
            result.append(Token("\n", line))
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


def call_callees(items, matching):
    closing = {end: start for start, end in matching.items()}
    result = {}
    for index, token in enumerate(items):
        if token.value != "(" or index == 0:
            continue
        start, end = index - 1, index
        if items[start].value == ")":
            start = closing.get(start, start)
        while start < end:
            if items[start].value == "(" and matching.get(start) == end - 1:
                start, end = start + 1, end - 1
            elif items[start].value == "*":
                start += 1
            else:
                break
        if (end == start + 1 and items[start].value not in BUILTIN_TYPES
                and re.fullmatch(r"[A-Za-z_]\w*", items[start].value)):
            result[index] = items[start]
    return result


def operand_count(items, matching, callees, start, end):
    count = 1
    index = start
    while index < end:
        value = items[index].value
        is_call = index in callees and index > start
        if value in {"[", "{"} or is_call:
            index = matching.get(index, index) + 1
            continue
        if value in {"&&", "||"}:
            count += 1
        index += 1
    return count


def condition_findings(items):
    matching = pairs(items)
    callees = call_callees(items, matching)
    for index in range(len(items) - 2):
        if items[index].value != "if" or items[index + 1].value != "(":
            continue
        start = index + 2
        end = matching.get(index + 1, start)
        if start >= end:
            continue
        signature = " ".join(token.value for token in items[start:end])
        lines = items[end - 1].line - items[start].line + 1
        count = operand_count(items, matching, callees, start, end)
        if lines > CONDITION_LINES:
            yield Finding("condition-lines", signature, items[start].line,
                          f"if condition spans {lines} lines (max {CONDITION_LINES}); extract a named predicate or early returns", lines)
        if count > CONDITION_OPERANDS:
            yield Finding("condition-operands", signature, items[start].line,
                          f"if condition has {count} logical operands (max {CONDITION_OPERANDS}); split the decision")


def findings(path, source, app_headers, snapshot):
    items = tokens(source)
    result = list(condition_findings(items))
    for directive in TOKEN.finditer(source):
        if directive.lastgroup != "directive":
            continue
        logical = re.sub(r"\\\r?\n", "", source[directive.start("hash"):directive.end()])
        logical = COMMENTS_AND_LITERALS.sub(
            lambda match: " " if match.lastgroup == "comment" else match.group(), logical
        )
        match = re.match(r'^#\s*include\s*([<"])([^">]+)[">]', logical)
        if not match:
            continue
        header = match[2].replace("\\", "/")
        name = PurePosixPath(header).name.lower()
        local = posixpath.normpath(str(PurePosixPath(path).parent / header)) if match[1] == '"' else None
        if path.startswith("core/") and (name in app_headers and local not in snapshot
                                        or name in UI_HEADERS or "app" in header.split("/")):
            result.append(Finding("core-ui", header, source[:directive.start("hash")].count("\n") + 1,
                                  f"core includes UI header {header}; move UI ownership into app"))
    if path.startswith("app/"):
        for token in call_callees(items, pairs(items)).values():
            if token.value in API_CALLS and path != "app/store.c":
                result.append(Finding("direct-api", token.value, token.line,
                                      f"{token.value} bypasses store; use a store request helper"))
            if token.value.startswith("WinHttp") and path != "app/ws.c":
                result.append(Finding("direct-http", token.value, token.line,
                                      "WinHTTP belongs in core transport or app/ws.c, not screen/app helpers"))
    return result


def source_suppressions(source):
    count = 0
    for match in COMMENTS_AND_LITERALS.finditer(source):
        if match.lastgroup == "comment":
            count += len(re.findall(r"\bcppcheck-suppress(?:-\w+)?\b", match.group()))
    items = tokens(source, include_directives=True)
    code = " ".join(token.value for token in items
                    if not token.value.startswith(('"', "'")))
    for index in range(len(items) - 3):
        if items[index].value != "_Pragma" or items[index + 1].value != "(":
            continue
        operand = index + 2
        if items[operand].value == "L":
            operand += 1
        literal = items[operand].value
        if literal.startswith('"') and operand + 1 < len(items) and items[operand + 1].value == ")":
            decoded = re.sub(r'\\(["\\])', r'\1', literal[1:-1])
            code += " # pragma " + " ".join(token.value for token in tokens(decoded)
                                             if not token.value.startswith(('"', "'")))
    count += len(re.findall(
        r"(?:#\s*pragma\s+|\b__pragma\s*\(\s*)"
        r"(?:(?:GCC|clang)\s+diagnostic\s+ignored|warning\s*\(\s*(?:(?:disable|suppress)\s*:|push\s*,\s*[0-3]\b))",
        code,
    ))
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
        previous = defaultdict(list)
        for item in findings(path, old, app_headers, before):
            previous[item.rule, item.signature].append(item.size)
        for item in findings(path, new, app_headers, after):
            key = item.rule, item.signature
            allowance = next((size for size in sorted(previous[key]) if size >= item.size), None)
            if allowance is not None:
                previous[key].remove(allowance)
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
        floor = lambda text: [float(value) for value in re.findall(
            r"^[ \t]*(?:[^:=\n]+:{1,2}[ \t]*)?(?:(?:override|export|private)[ \t]+)*"
            r"COVERAGE_MIN[ \t]*(?:::?=|[?+]?=)[ \t]*([+-]?(?:\d+(?:\.\d*)?|\.\d+))[ \t]*$",
            text, re.MULTILINE,
        )]
        old_floor, new_floor = (floor(re.sub(r"(?m)#.*$", "", re.sub(
            r"[ \t]*\\\r?\n[ \t]*", " ", snapshot.get("Makefile", "")
        ))) for snapshot in (before, after))
        if old_floor and (not new_floor or min(new_floor) < min(old_floor)):
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
