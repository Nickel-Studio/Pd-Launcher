#!/usr/bin/env python3
"""Replacement-static-library relink probe for pd-launcher.exe.

Reconstructs the *real recorded link command* from the Ninja build graph
(`LINK_FLAGS` + `LINK_LIBRARIES` + object inputs of the
`CXX_EXECUTABLE_LINKER__ni-launcher_Release` build statement), rewrites only the
paths named by --substitute, and invokes the same MSVC `link.exe`.

It never edits the build graph, tracked sources or the installed Qt prefix.

Run under the trusted developer environment:

    python -B scripts/native/msvc.py -- python -B scripts/native/relink_probe.py \
        --build-dir .snow/legal-relink/build \
        --substitute "D:/.../lib/zlibstatic.lib=<probe>/zlibstatic.lib" \
        --out native/app/pd-launcher-relink-probe.exe

Exit codes: 0 link succeeded, 1 link failed, 2 usage/graph problem.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def logical_lines(text: str) -> list[str]:
    """Join Ninja `$`-continued lines into single logical lines."""
    out: list[str] = []
    pending = ""
    for raw in text.splitlines():
        if pending:
            raw = pending + raw.lstrip() if not pending.endswith("$ ") else pending[:-2] + raw.lstrip()
            pending = ""
        if raw.endswith("$") and not raw.endswith("$$"):
            pending = raw[:-1]
            continue
        out.append(raw)
    if pending:
        out.append(pending)
    return out


def ninja_unescape(value: str) -> str:
    """Undo Ninja path escaping ($:, $$, $<space>, doubled backslashes)."""
    value = value.replace("$$", "\x00").replace("$:", ":").replace("$ ", " ")
    value = value.replace("\\\\", "\\")
    return value.replace("\x00", "$")


def find_target_block(lines: list[str], target: str, rule: str) -> tuple[str, dict[str, str]]:
    needle = f"build {target}: {rule}"
    for index, line in enumerate(lines):
        if line.startswith(needle):
            statement = line
            variables: dict[str, str] = {}
            for follow in lines[index + 1:]:
                if not follow.startswith("  "):
                    break
                stripped = follow.strip()
                if " = " not in stripped:
                    continue
                name, value = stripped.split(" = ", 1)
                variables[name] = value
            return statement, variables
    raise SystemExit(f"target block not found: {needle}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--target", default="native\\app\\pd-launcher.exe")
    parser.add_argument("--rule", default="CXX_EXECUTABLE_LINKER__ni-launcher_Release")
    parser.add_argument("--substitute", action="append", default=[], help="ORIGINAL=PROBE (path prefix/exact)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--implib", default=None)
    parser.add_argument("--pdb", default=None)
    parser.add_argument("--map", default=None)
    parser.add_argument("--rsp", default=None)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    build_dir = Path(args.build_dir).resolve()
    graph = (build_dir / "build.ninja").read_text(encoding="utf-8")
    statement, variables = find_target_block(logical_lines(graph), args.target, args.rule)

    flags = ninja_unescape(variables.get("LINK_FLAGS", ""))
    libraries = ninja_unescape(variables.get("LINK_LIBRARIES", ""))
    link_path = ninja_unescape(variables.get("LINK_PATH", ""))
    objects = ninja_unescape(statement.split(": ", 1)[1])
    # drop "<rule> <inputs...>"; implicit (`|`) and order-only (`||`) inputs are
    # not part of the link line (their objects are already in LINK_LIBRARIES).
    objects = objects.split(" ", 1)[1] if " " in objects else ""
    for separator in (" || ", " | "):
        objects = objects.split(separator, 1)[0]

    substitutions = []
    for pair in args.substitute:
        original, probe = pair.split("=", 1)
        substitutions.append((original, probe))

    def apply(value: str, required: bool = True) -> str:
        for original, probe in substitutions:
            if original in value:
                value = value.replace(original, probe)
            elif required:
                raise SystemExit(f"substitution target not present in link command: {original}")
            else:
                print(f"note: {original} absent from object inputs; substituted in libraries only", flush=True)
        return value

    libraries = apply(libraries)
    objects = apply(objects, required=False)
    if args.map:
        import re
        flags = re.sub(r"/MAP:\S+", "/MAP:" + args.map, flags)

    response = " ".join(part for part in (objects, flags, link_path, libraries) if part).strip()
    rsp_path = Path(args.rsp) if args.rsp else build_dir / "relink-probe.rsp"
    rsp_path.parent.mkdir(parents=True, exist_ok=True)
    rsp_path.write_text(response + "\n", encoding="utf-8")

    link = shutil.which("link.exe")
    if link is None:
        return 2
    command = [link, "/nologo", f"@{rsp_path}", f"/out:{args.out}"]
    if args.implib:
        command.append(f"/implib:{args.implib}")
    if args.pdb:
        command.append(f"/pdb:{args.pdb}")
    print("link:", " ".join(command), flush=True)
    print("rsp:", rsp_path, len(response), "chars", flush=True)
    if args.dry_run:
        return 0
    result = subprocess.run(command, cwd=build_dir)
    return 0 if result.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))