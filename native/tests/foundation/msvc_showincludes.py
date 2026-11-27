import argparse
import ctypes
import locale
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler")
    parser.add_argument("build_directory", type=Path)
    args = parser.parse_args()
    probe = args.build_directory / "foundation-showincludes-probe"
    probe.mkdir(exist_ok=True)
    header = probe / "foundation_showincludes_marker.hpp"
    source = probe / "foundation_showincludes_probe.cpp"
    header.write_text("#pragma once\n", encoding="ascii")
    source.write_text('#include "foundation_showincludes_marker.hpp"\n', encoding="ascii")
    result = subprocess.run(
        [args.compiler, "/nologo", "/Zs", "/showIncludes", str(source)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False,
    )
    if result.returncode:
        raise RuntimeError("MSVC include-prefix probe did not compile")
    marker = str(header.resolve()).encode("utf-8")
    line = next((value for value in result.stdout.splitlines() if marker in value), None)
    if line is None:
        raise RuntimeError("MSVC include-prefix probe did not report its header")
    raw = line[:line.index(marker)]
    try:
        prefix = raw.decode("utf-8")
    except UnicodeDecodeError:
        code_page = ctypes.windll.kernel32.GetConsoleOutputCP() if sys.platform == "win32" else 0
        prefix = raw.decode("cp" + str(code_page) if code_page else locale.getpreferredencoding(False))
    if not prefix or "\n" in prefix or "\r" in prefix:
        raise RuntimeError("MSVC include-prefix probe returned an invalid prefix")
    sys.stdout.reconfigure(encoding="utf-8")
    print(prefix)


if __name__ == "__main__":
    main()
