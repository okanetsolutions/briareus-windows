#!/usr/bin/env python3
"""Extract explicitly selected presentation responsibilities without changing function bodies."""

import argparse
from collections import defaultdict
from dataclasses import dataclass
import json
from pathlib import Path
import re
import subprocess

from check_structure import TOKEN


@dataclass
class Unit:
    text: str
    kind: str
    name: str = ""
    signature: str = ""


def lex(source):
    return [m for m in TOKEN.finditer(source) if m.lastgroup in {"word", "literal"}]


def identifiers(source):
    return {m.group() for m in lex(source) if m.lastgroup == "word"}


def rename(source, names):
    previous = ""

    def replacement(match):
        nonlocal previous
        value = match.group()
        if match.lastgroup != "word":
            return value
        result = names.get(value, value) if previous not in {".", "->"} else value
        previous = value
        return result

    return TOKEN.sub(replacement, source)


def units(source):
    """Read top-level C declarations, treating function bodies as indivisible units."""
    directives = re.findall(r"^\s*#.*$", source, re.MULTILINE)
    if any(re.match(r"\s*#\s*(if|else|endif)", d) for d in directives):
        raise ValueError("conditional source requires an explicit manual extraction")
    source = re.sub(r"^[ \t]*#.*\n?", "", source, flags=re.MULTILINE)
    items = lex(source)
    result, start, previous = [], 0, 0
    depth = {"(": 0, "[": 0, "{": 0}
    closing = {")": "(", "]": "[", "}": "{"}
    body = None
    for i, item in enumerate(items):
        value = item.group()
        if value == "{" and not any(depth.values()):
            head = [m.group() for m in items[start:i]]
            if head and head[-1] == ")" and "=" not in head and head[0] != "typedef":
                body = i
        if value in depth:
            depth[value] += 1
        elif value in closing:
            depth[closing[value]] -= 1
        ended_function = body is not None and value == "}" and not any(depth.values())
        if not ended_function and (value != ";" or any(depth.values())):
            continue
        text = source[previous:item.end()].strip() + "\n"
        values = [m.group() for m in items[start:i + 1]]
        if ended_function:
            signature = source[items[start].start():items[body].start()].strip()
            name = re.search(r"([A-Za-z_]\w*)\s*\(", signature)[1]
            result.append(Unit(text, "function", name, signature))
        elif values[0] == "typedef" or values[0] == "enum" or (values[0] == "struct" and values[-2] == "}"):
            name = values[-2] if values[0] == "typedef" else ""
            result.append(Unit(text, "type", name))
        elif "(" in values and "=" not in values and "{" not in values:
            result.append(Unit(text, "prototype"))
        else:
            head = text.split("=", 1)[0].rstrip(";\n ")
            match = re.search(r"([A-Za-z_]\w*)\s*((?:\[[^\]]*\]\s*)*)$", head)
            if not match:
                raise ValueError(f"unsupported declaration: {text[:100]}")
            result.append(Unit(text, "data", match[1]))
        previous, start, body = item.end(), i + 1, None
    if start != len(items):
        raise ValueError("incomplete top-level declaration")
    return directives, result


def extern_data(unit, alias):
    definition = re.sub(r"\bstatic\s+", "", unit.text, count=1)
    declaration = definition.split("=", 1)[0].strip().rstrip(";")
    extra = ""
    anonymous = re.search(r"\bstruct\s*(\{.*?\})", declaration, re.DOTALL)
    if anonymous:
        type_name = alias + "_type"
        extra = "typedef struct " + anonymous[1] + " " + type_name + ";\n"
        definition = definition.replace(anonymous[0], type_name, 1)
        declaration = declaration.replace(anonymous[0], type_name, 1)
    if "[]" in declaration:
        # Complete array types are required by sizeof in another component.
        initializer = unit.text.split("=", 1)[1].strip().rstrip(";")
        levels, elements = 0, 0
        for item in lex(initializer):
            value = item.group()
            if value == "{" and levels == 0:
                levels = 1
            elif value in {"{", "(", "["}:
                levels += 1
            elif value in {"}", ")", "]"}:
                levels -= 1
            elif levels == 1 and value == ",":
                elements += 1
        last = initializer.rstrip().rstrip("}").rstrip()
        size = elements if last.endswith(",") else elements + 1
        declaration = declaration.replace("[]", f"[{size}]", 1)
    # Leading comments belong to the definition, never an extern declaration.
    declaration = re.sub(r"^(?:\s|//[^\n]*\n|/\*.*?\*/)*", "", declaration, flags=re.DOTALL)
    return extra + "extern " + declaration + ";\n", definition


def extract(path, source, components):
    directives, declarations = units(source)
    stem = Path(path).stem
    assignments = {name: suffix for suffix, names in components.items() for name in names}
    if len(assignments) != sum(map(len, components.values())):
        raise ValueError(f"{path}: duplicate function assignment")
    functions = {u.name: u for u in declarations if u.kind == "function"}
    if assignments.keys() - functions.keys():
        raise ValueError(f"{path}: unknown functions {assignments.keys() - functions.keys()}")
    groups = {name: assignments.get(name, "") for name in functions}
    consumers = defaultdict(set)
    for name, unit in functions.items():
        for symbol in identifiers(unit.text):
            consumers[symbol].add(groups[name])
    data = {u.name: u for u in declarations if u.kind == "data"}
    for name in data:
        uses = consumers[name]
        groups[name] = next(iter(uses)) if len(uses) == 1 else ""
    # Initializers also use callbacks and tables, so include them in linkage decisions.
    for name, unit in data.items():
        for symbol in identifiers(unit.text):
            consumers[symbol].add(groups[name])
    # Drawing payloads used by just one component remain private to that component.
    type_groups = {}
    types = [u for u in declarations if u.kind == "type"]
    for unit in types:
        uses = consumers[unit.name]
        type_groups[id(unit)] = next(iter(uses)) if unit.name and len(uses) == 1 else None
    # A type referenced by a shared declaration must itself be shared.
    for _ in types:
        shared = set().union(*(identifiers(u.text) for u in types if type_groups[id(u)] is None))
        shared.update(set().union(*(identifiers(u.signature) for u in functions.values()
                                   if consumers[u.name] - {groups[u.name]} or not u.signature.startswith("static "))))
        shared.update(set().union(*(identifiers(u.text.split("=", 1)[0]) for u in data.values()
                                   if consumers[u.name] - {groups[u.name]})))
        for unit in types:
            if unit.name in shared:
                type_groups[id(unit)] = None
    names = {}
    cross = set()
    for name, unit in {**functions, **data}.items():
        if consumers[name] - {groups[name]}:
            cross.add(name)
            if "static" in identifiers(unit.signature if unit.kind == "function" else unit.text.split("=", 1)[0]):
                names[name] = stem + "_" + name
    header_name = stem + "_internal.h"
    guard = "BRIAREUS_" + header_name.replace(".", "_").upper()
    header = [f"#ifndef {guard}\n#define {guard}\n", "\n".join(directives) + "\n\n"]
    for unit in declarations:
        if unit.kind == "type" and type_groups[id(unit)] is None:
            header.append(unit.text + "\n")
    for name, unit in functions.items():
        if name in cross or "static" not in identifiers(unit.signature):
            header.append(rename(re.sub(r"^static\s+", "", unit.signature), names) + ";\n")
    definitions = {}
    for name, unit in data.items():
        if name in cross:
            declaration, definition = extern_data(unit, names.get(name, name))
            header.append(rename(declaration, names))
            definitions[name] = definition
    header.append("\n#endif\n")
    output = {str(Path(path).with_name(header_name)): "".join(header)}
    for suffix in ["", *components]:
        filename = stem + ("_" + suffix if suffix else "") + ".c"
        parts = [f'#include "{header_name}"\n\n']
        for unit in types:
            if type_groups[id(unit)] == suffix:
                parts.append(unit.text + "\n")
        for name, unit in functions.items():
            if groups[name] == suffix and name not in cross and "static" in identifiers(unit.signature):
                parts.append(rename(unit.signature, names) + ";\n")
        parts.append("\n")
        for unit in declarations:
            if unit.kind not in {"function", "data"} or groups[unit.name] != suffix:
                continue
            text = definitions.get(unit.name, unit.text)
            if unit.kind == "function" and unit.name in cross:
                text = re.sub(r"\bstatic\s+", "", text, count=1)
            parts.append(rename(text, names) + "\n")
        content = "".join(parts)
        if len(content.splitlines()) > 300:
            raise ValueError(f"{filename}: {len(content.splitlines())} lines; select a cohesive smaller component")
        output[str(Path(path).with_name(filename))] = content
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    outputs = {}
    for path, components in manifest["sources"].items():
        source = subprocess.run(["git", "show", f'{manifest["base"]}:{path}'], check=True, capture_output=True, text=True).stdout
        if args.list:
            for unit in units(source)[1]:
                if unit.kind == "function":
                    print(path, unit.name, len(unit.text.splitlines()))
            continue
        outputs.update(extract(path, source, components))
    if args.list:
        return
    if args.check:
        differences = [path for path, content in outputs.items() if not Path(path).exists() or Path(path).read_text() != content]
        if differences:
            raise SystemExit("Extraction differs: " + ", ".join(differences))
        print(f"Verified {len(outputs)} extracted files against the pinned source.")
    else:
        for path, content in outputs.items():
            Path(path).write_text(content)
        print(f"Extracted {len(outputs)} files from {len(manifest['sources'])} presentation owners.")


if __name__ == "__main__":
    main()
