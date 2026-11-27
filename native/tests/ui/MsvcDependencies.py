import json
from pathlib import Path
import shutil
import subprocess
import sys


PREFIX = b"Note: including file: "


def detect_prefix(compiler, source):
    result = subprocess.run(
        [compiler, "/nologo", "/Zs", "/showIncludes", str(source)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False,
    )
    if result.returncode:
        sys.stderr.buffer.write(result.stderr)
        raise RuntimeError("MSVC dependency probe failed")
    marker = str(source.with_name("nickel_include_marker.hpp").resolve()).encode("utf-8")
    line = next((value for value in result.stdout.splitlines() if marker in value), None)
    if line is None:
        raise RuntimeError("MSVC dependency probe did not report its marker header")
    raw_prefix = line[:line.index(marker)]
    if not raw_prefix:
        raise RuntimeError("MSVC dependency probe returned an empty prefix")
    return raw_prefix


def probe(compiler, build_directory):
    directory = Path(build_directory) / "msvc-include-probe"
    if shutil.disk_usage(build_directory).free < 64 * 1024 * 1024:
        raise RuntimeError("Insufficient space for the MSVC dependency probe")
    directory.mkdir(exist_ok=True)
    header = directory / "nickel_include_marker.hpp"
    source = directory / "nickel_include_probe.cpp"
    header.write_text("#pragma once\n", encoding="ascii")
    source.write_text('#include "nickel_include_marker.hpp"\n', encoding="ascii")
    state = directory / "prefix.json"
    state.write_text(json.dumps({"rawPrefixHex": detect_prefix(compiler, source).hex()}), encoding="ascii")
    print(str(state.resolve()))


def compile_with_dependencies(state, command):
    source = Path(state).with_name("nickel_include_probe.cpp")
    # Detached Ninja can use a different output code page than CMake's configure process.
    raw_prefix = detect_prefix(command[0], source)
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    for line in result.stdout.splitlines(keepends=True):
        if line.startswith(raw_prefix):
            line = PREFIX + line[len(raw_prefix):]
        sys.stdout.buffer.write(line)
    sys.stdout.buffer.flush()
    sys.stderr.buffer.write(result.stderr)
    sys.stderr.buffer.flush()
    return result.returncode


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--probe":
        probe(sys.argv[2], sys.argv[3])
        return 0
    if len(sys.argv) > 3 and sys.argv[1] in ("--compile", "--compile-probed"):
        return compile_with_dependencies(sys.argv[2], sys.argv[3:])
    raise RuntimeError("Expected --probe compiler build_directory or --compile-probed state compiler arguments")


if __name__ == "__main__":
    sys.exit(main())
