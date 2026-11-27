#!/usr/bin/env python3
"""Offline static verification of the Apple stop-token shim wiring.

Answers, from CMake/shim source text alone (no compiler, no network):

  Q1  Is the Apple forced include effective for every Apple target?
      -> the ``add_compile_options`` statement must precede the first
         target-creating statement (``add_subdirectory``/``add_library``/
         ``add_executable``) reachable at top level, in both the production
         root project and the standalone macOS probe project.
  Q2  Is the flag form correct?
      -> GNU ``-include`` (never MSVC ``/FI``), wrapped in
         ``$<$<COMPILE_LANGUAGE:CXX>:...>`` and ``SHELL:``, inside an
         ``if(APPLE)`` scope, with an absolute path kept whole by shell-style
         quoting (``separate_arguments(UNIX_COMMAND)`` semantics). The quoting
         requirement is strict for both the root statement and the standalone
         probe project.
  Q3  Are the layer-2 preconditions kept?
      -> no precompiled headers / unity builds; the only ``-include`` flags in
         the tree are the two Apple shim statements (no ordering ambiguity);
         ``stop_compat.hpp`` itself must include ``<version>`` before testing
         the libc++ gate and include ``<stop_token>`` after lifting it.
  Q4  ``-Werror`` target inventory.
      -> list every ``-Werror`` (outside MSVC branches) and ``/WX`` (MSVC
         branches) target; assert the probe project uses none and that every
         non-MSVC ``-Werror`` file belongs to the audited root target graph
         (hence inherits the directory-scoped forced include by construction).
  Q5  ``NICKEL_FORCE_PORTABLE_CANCELLATION`` must not pollute production.
      -> no directory-scope definition, no ``option()``/cache entry, no
         definition inside ``stop_compat.hpp``; the only allowed site is a
         PRIVATE ``target_compile_definitions`` under ``native/tests/`` or
         ``tests/``; the macOS probe must not define it.

Enumeration scope: every CMake file is discovered through ``git ls-files`` (the
index), so untracked and ignored trees (``artifacts/``, ``.snow/``,
``evidence-*/``, local build directories) are excluded by construction: a
historical source snapshot that the repository never checks out cannot
contaminate the audit. A root without git metadata (staged delivery tree)
falls back to a filesystem walk that skips the same class of local-only
directories.

Exit status: 0 when every hard check passes; 1 when any hard check fails
(``--check`` makes this an explicit CI gate). ``--root`` points at the
repository (or staged delivery tree) to audit; default is the repository
containing this script.

Usage:
  python -B scripts/native/apple_shim_verify.py [--check] [--root DIR]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

FORCED_INCLUDE_MARKER = "stop_compat.hpp"
SHIM_HEADER = Path("native/core/common/stop_compat.hpp")
CANCELLATION_HEADER = Path("native/core/common/cancellation.hpp")
PROBE_CMAKE = Path("tests/native-macos/CMakeLists.txt")
ROOT_CMAKE = Path("CMakeLists.txt")
TEST_SWITCH = "NICKEL_FORCE_PORTABLE_CANCELLATION"

# Files allowed to place a non-MSVC ``-Werror`` on a target. Every one of them
# is reached through ``add_subdirectory`` from the root project, after the
# directory-scope forced include, so the shim applies to their TUs by
# construction. Kept as file-level (not target-level) so target-name refactors
# stay allowed while a new file fails the audit until reviewed.
EXPECTED_WERROR_FILES = {
    # Root target graph: reached through add_subdirectory after the
    # directory-scope forced include, so the shim applies to their TUs by
    # construction.
    "native/core/accounts/CMakeLists.txt",
    "native/app/CMakeLists.txt",
    "native/tests/identity/CMakeLists.txt",
    "native/tests/production_entry/production_tests.cmake",
    "native/tests/trusted_export/CMakeLists.txt",
    "tests/native-production-smoke/CMakeLists.txt",
    # Standalone host harnesses: NOT add_subdirectory'd by the root project and
    # not configured by the macOS delivery (Windows/Linux evidence only). If
    # these are ever configured on Apple they must join the forced-include
    # scope first; treat a new configure path as an audit event.
    "tests/native-cleanup/CMakeLists.txt",
}

CMAKE_FILE_NAMES = ("CMakeLists.txt",)
CMAKE_SUFFIXES = (".cmake",)
# Only used by the non-git fallback; the git path never needs directory pruning.
SKIP_DIRECTORIES = {
    ".git", ".snow", ".tools", ".build", ".deps", ".temp", "__pycache__",
    "build", "builds", "node_modules", "artifacts",
}
SKIP_DIRECTORY_PREFIXES = ("evidence-", "build-", "objects-")


def is_cmake_file(path: Path) -> bool:
    return path.name in CMAKE_FILE_NAMES or path.suffix in CMAKE_SUFFIXES


def git_tracked_cmake_files(root: Path) -> list[Path] | None:
    """CMake files recorded in the git index under ``root``.

    ``None`` means the root is not a git work tree (or git is unavailable), so
    the caller must use the filesystem fallback. Untracked and ignored trees
    never appear here: ``git ls-files`` reports index entries only.
    """
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached", "--", "."],
            capture_output=True, timeout=180, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    files: list[Path] = []
    for entry in completed.stdout.decode("utf-8", "surrogateescape").split("\0"):
        if not entry:
            continue
        relative = Path(entry)
        if not is_cmake_file(relative):
            continue
        candidate = root / relative
        if candidate.is_file():
            files.append(candidate)
    return sorted(files)


def filesystem_cmake_files(root: Path) -> list[Path]:
    """Fallback enumeration for roots without git metadata (delivery trees)."""
    files: list[Path] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        parts = path.relative_to(root).as_posix().split("/")
        if any(part in SKIP_DIRECTORIES for part in parts):
            continue
        if any(part.startswith(SKIP_DIRECTORY_PREFIXES) for part in parts):
            continue
        if is_cmake_file(path):
            files.append(path)
    return files


def list_cmake_files(root: Path) -> tuple[list[Path], str]:
    """Return (files, mode) where mode is ``git-tracked`` or ``filesystem-fallback``."""
    tracked = git_tracked_cmake_files(root)
    if tracked is not None:
        return tracked, "git-tracked"
    return filesystem_cmake_files(root), "filesystem-fallback"


def strip_comments(text: str) -> list[str]:
    """Drop ``#`` comments while respecting double-quoted CMake strings."""
    out: list[str] = []
    for line in text.splitlines():
        result: list[str] = []
        in_string = False
        index = 0
        while index < len(line):
            if line[index] == "\\" and in_string and index + 1 < len(line):
                result.append(line[index : index + 2])
                index += 2
                continue
            ch = line[index]
            if ch == '"':
                in_string = not in_string
                result.append(ch)
            elif ch == "#" and not in_string:
                break
            else:
                result.append(ch)
            index += 1
        out.append("".join(result))
    return out


def comment_free_lines(path: Path) -> list[str]:
    return strip_comments(path.read_text(encoding="utf-8", errors="replace"))


def raw_lines_of(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def cpp_comment_free_lines(text: str) -> list[str]:
    """Drop ``//`` comments (C++ headers) while keeping preprocessor lines intact."""
    out: list[str] = []
    for line in text.splitlines():
        result: list[str] = []
        in_string = False
        index = 0
        while index < len(line):
            if line[index] == "\\" and in_string and index + 1 < len(line):
                result.append(line[index : index + 2])
                index += 2
                continue
            ch = line[index]
            if ch == '"':
                in_string = not in_string
                result.append(ch)
                index += 1
                continue
            if ch == "/" and index + 1 < len(line) and line[index + 1] == "/" and not in_string:
                break
            result.append(ch)
            index += 1
        out.append("".join(result))
    return out


def cpp_free_lines(path: Path) -> list[str]:
    return cpp_comment_free_lines(path.read_text(encoding="utf-8", errors="replace"))


def enclosing_branches(lines: list[str], index: int) -> list[tuple[str, bool]]:
    """Return [(condition, in_else)] for if/elseif/else frames active at a line."""
    frames: list[tuple[str, bool]] = []
    for line in lines[:index]:
        stripped = line.strip()
        match = re.match(r"^(if|elseif)\s*\((.*)\)\s*$", stripped)
        if match:
            frames.append((match.group(2), False))
            continue
        if re.match(r"^else\s*\(\s*\)\s*$", stripped):
            if frames:
                condition, _ = frames[-1]
                frames[-1] = (condition, True)
            continue
        if re.match(r"^endif\s*\(\s*\)\s*$", stripped):
            if frames:
                frames.pop()
    return frames


def branch_platform(lines: list[str], index: int) -> str:
    """Classify a line as 'msvc', 'non-msvc' or 'neutral' by branch context."""
    tag = "neutral"
    for condition, in_else in enclosing_branches(lines, index):
        positive = re.search(r"(?<![A-Za-z_])MSVC(?![A-Za-z_])", condition) is not None
        negative = re.search(r"\bNOT\b[^()]*\bMSVC\b", condition) is not None
        if in_else:
            if positive:
                tag = "non-msvc"
            elif negative:
                tag = "msvc"
        else:
            if positive:
                tag = "msvc"
            elif negative:
                tag = "non-msvc"
    return tag


def top_level_indices(lines: list[str], kinds: tuple[str, ...]) -> list[int]:
    """Indices of unindented statements of the given kinds, skipping function bodies."""
    result: list[int] = []
    function_depth = 0
    for index, line in enumerate(lines):
        stripped = line.strip()
        if function_depth == 0 and re.match(r"^(function|macro)\s*\(", stripped):
            function_depth = 1
            continue
        if function_depth > 0:
            if re.match(r"^(endfunction|endmacro)\s*\(", stripped):
                function_depth -= 1
            continue
        if line.startswith((" ", "\t")):
            continue
        if any(re.match(rf"^{kind}\s*\(", stripped) for kind in kinds):
            result.append(index)
    return result


class Finding:
    def __init__(self, name: str, ok: bool, detail: str) -> None:
        self.name = name
        self.ok = ok
        self.detail = detail

    def render(self) -> str:
        return f"[{'PASS' if self.ok else 'FAIL'}] {self.name}: {self.detail}"


class Auditor:
    def __init__(self, root: Path) -> None:
        self.root = root.resolve()
        self.findings: list[Finding] = []
        self.werror_non_msvc: list[str] = []
        self.werror_msvc: list[str] = []

    def record(self, name: str, ok: bool, detail: str) -> None:
        self.findings.append(Finding(name, ok, detail))

    # ------------------------------------------------------------- Q1 + Q2 --
    def find_forced_include(self, path: Path) -> tuple[int, str] | None:
        for index, line in enumerate(comment_free_lines(path)):
            if "-include" in line and FORCED_INCLUDE_MARKER in line:
                return index + 1, line.strip()
        return None

    def first_target_statement(self, path: Path) -> tuple[int, str] | None:
        lines = comment_free_lines(path)
        for index in top_level_indices(lines, ("add_subdirectory", "add_library", "add_executable")):
            return index + 1, lines[index].strip()
        return None

    def check_forced_include_order(self, label: str, cmake_path: Path) -> None:
        include = self.find_forced_include(cmake_path)
        if include is None:
            self.record(f"{label}: forced include present", False,
                        f"no -include {FORCED_INCLUDE_MARKER} in {cmake_path}")
            return
        include_line, include_text = include
        first = self.first_target_statement(cmake_path)
        if first is None:
            self.record(f"{label}: forced include precedes targets", False,
                        "no target-creating statements found")
            return
        first_line, first_text = first
        self.record(
            f"{label}: forced include precedes first target",
            include_line < first_line,
            f"include@{include_line} vs first target @{first_line}: {first_text[:80]}",
        )
        self.check_flag_form(label, cmake_path, include_line, include_text)

    def check_flag_form(self, label: str, cmake_path: Path, include_line: int,
                        include_text: str) -> None:
        lines = comment_free_lines(cmake_path)
        raw = raw_lines_of(cmake_path)
        frames = enclosing_branches(lines, include_line - 1)
        inside_apple = any(re.search(r"(?<![A-Za-z_])APPLE(?![A-Za-z_])", cond) for cond, _ in frames)
        self.record(f"{label}: forced include inside if(APPLE)", inside_apple, f"branches={frames}")

        problems: list[str] = []
        if "/FI" in include_text:
            problems.append("MSVC /FI used instead of GNU -include")
        if not re.match(r"^\s*add_compile_options\s*\(", include_text):
            problems.append("not add_compile_options (would lose directory scope)")
        if "SHELL:" not in include_text:
            problems.append("missing SHELL: prefix (flag would be mangled/de-duplicated)")
        if "$<$<COMPILE_LANGUAGE:CXX>:" not in include_text:
            problems.append("missing $<COMPILE_LANGUAGE:CXX> guard (would hit C/RC TUs)")
        if not re.search(r"-include \\\"", raw[include_line - 1]):
            problems.append("path not shell-quoted inside SHELL: (a path with spaces would split the flag)")
        self.record(f"{label}: GNU -include form, SHELL:, CXX-guarded, shell-quoted absolute path",
                    not problems,
                    ("; ".join(problems) or "ok") + (f"; path expr={'absolute' if '${CMAKE_' in include_text else 'variable'}"))

    # ---------------------------------------------------------------- Q3 --
    def check_no_other_forced_includes(self, cmake_files: list[Path]) -> None:
        offenders: list[str] = []
        include_pattern = re.compile(r"(?<![\w-])-include\s")
        fi_pattern = re.compile(r"(?<![\w-])/FI\b")
        for path in cmake_files:
            rel = path.relative_to(self.root).as_posix()
            for index, line in enumerate(comment_free_lines(path)):
                if fi_pattern.search(line):
                    offenders.append(f"{rel}:{index + 1}: /FI")
                elif include_pattern.search(line) and FORCED_INCLUDE_MARKER not in line:
                    offenders.append(f"{rel}:{index + 1}: {line.strip()[:70]}")
        self.record(
            "Q3b: no competing forced includes outside the two Apple shim statements",
            not offenders,
            "; ".join(offenders) or "only root+probe statements present",
        )

    def check_no_pch_unity(self, cmake_files: list[Path]) -> None:
        offenders: list[str] = []
        for path in cmake_files:
            rel = path.relative_to(self.root).as_posix()
            for index, line in enumerate(comment_free_lines(path)):
                if "target_precompile_headers" in line or "cotire" in line:
                    offenders.append(f"{rel}:{index + 1} (PCH)")
                elif re.search(r"UNITY_BUILD[^)]*\bON\b", line) or re.search(r"\bCMAKE_UNITY_BUILD\b[^)]*\bON\b", line):
                    offenders.append(f"{rel}:{index + 1} (unity)")
        self.record("Q3c: no PCH/unity-build statements anywhere", not offenders,
                    "; ".join(offenders) or "clean")
        has_disable = any("CMAKE_DISABLE_PRECOMPILE_HEADERS ON" in line
                          for line in comment_free_lines(self.root / ROOT_CMAKE))
        self.record("Q3c: root disables precompiled headers", has_disable, ROOT_CMAKE.as_posix())

    def check_shim_header_shape(self) -> None:
        lines = cpp_free_lines(self.root / SHIM_HEADER)
        version_line = next((i for i, l in enumerate(lines) if "#include <version>" in l), None)
        gate_line = next((i for i, l in enumerate(lines)
                          if "_LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN" in l and "#if" in l), None)
        undef_line = next((i for i, l in enumerate(lines)
                           if "#undef _LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN" in l), None)
        stop_token_line = next((i for i, l in enumerate(lines) if "#include <stop_token>" in l), None)
        problems: list[str] = []
        if None in (version_line, gate_line, undef_line, stop_token_line):
            problems.append("missing version/gate/undef/<stop_token> sequence")
        else:
            if not version_line < gate_line:
                problems.append("<version> must precede the gate test so libc++ __config defines the macro")
            if not undef_line < stop_token_line:
                problems.append("<stop_token> must be parsed after the gate macro is undefined")
            if stop_token_line < gate_line:
                problems.append("<stop_token> included before the gate logic")
        if any(TEST_SWITCH in line and "#define" in line for line in lines):
            problems.append("shim header defines the test-only switch")
        for macro in ("NICKEL_STOP_TOKEN", "NICKEL_STOP_SOURCE"):
            if not any(f"#define {macro}" in line for line in lines):
                problems.append(f"missing routing macro {macro}")
        self.record("Q3a: stop_compat.hpp layer-2 sequence (version -> gate -> undef -> <stop_token>)",
                    not problems, "; ".join(problems) or "ok")
        cancellation = self.root / CANCELLATION_HEADER
        if cancellation.exists():
            text = cancellation.read_text(encoding="utf-8", errors="replace")
            ok = "NICKEL_STOP_TOKEN" in text and "NICKEL_STOP_SOURCE" in text
            self.record("Q3a: cancellation.hpp routes through NICKEL_STOP_* macros", ok, "cancellation.hpp")

    # ---------------------------------------------------------------- Q4 --
    def check_werror_inventory(self, cmake_files: list[Path]) -> None:
        for path in cmake_files:
            rel = path.relative_to(self.root).as_posix()
            lines = comment_free_lines(path)
            for index, line in enumerate(lines):
                match = re.search(r"target_compile_options\s*\(\s*([^\s)]+)", line)
                if not match:
                    continue
                target = match.group(1)
                platform = branch_platform(lines, index)
                if re.search(r"(?<![\w-])-Werror(?![\w-])", line) and platform != "msvc":
                    self.werror_non_msvc.append(f"{rel}:{index + 1} target={target}")
                if "/WX" in line and platform != "non-msvc":
                    self.werror_msvc.append(f"{rel}:{index + 1} target={target}")
        outside = [entry for entry in self.werror_non_msvc
                   if entry.split(":")[0] not in EXPECTED_WERROR_FILES]
        self.record(
            f"Q4: non-MSVC -Werror inventory within audited allowlist ({len(self.werror_non_msvc)} entries)",
            not outside,
            "; ".join(outside) or "all within " + ", ".join(sorted(EXPECTED_WERROR_FILES)),
        )
        probe_has_werror = any(re.search(r"(?<![\w-])-Werror(?![\w-])|/WX", line)
                               for line in comment_free_lines(self.root / PROBE_CMAKE))
        self.record("Q4: macOS probe project uses no -Werror//WX", not probe_has_werror,
                    PROBE_CMAKE.as_posix())

    # ---------------------------------------------------------------- Q5 --
    def check_test_switch_isolation(self, cmake_files: list[Path]) -> None:
        definition_sites: list[str] = []
        bad_sites: list[str] = []
        for path in cmake_files:
            rel = path.relative_to(self.root).as_posix()
            for index, line in enumerate(comment_free_lines(path)):
                if TEST_SWITCH not in line:
                    continue
                if re.match(r"^\s*target_compile_definitions\s*\(", line):
                    if "PRIVATE" in line:
                        definition_sites.append(f"{rel}:{index + 1}")
                    else:
                        bad_sites.append(f"{rel}:{index + 1} (not PRIVATE)")
                elif re.match(r"^\s*option\s*\(", line) or "CACHE" in line:
                    bad_sites.append(f"{rel}:{index + 1} (cacheable)")
                elif re.match(r"^\s*add_compile_definitions\s*\(", line):
                    bad_sites.append(f"{rel}:{index + 1} (directory scope)")
                elif not line.lstrip().startswith("#"):
                    bad_sites.append(f"{rel}:{index + 1} (unexpected statement)")
        under_tests = [site for site in definition_sites
                       if site.startswith(("native/tests/", "tests/"))]
        ok = not bad_sites and definition_sites and len(under_tests) == len(definition_sites)
        self.record(
            "Q5: test-only switch defined only via PRIVATE target_compile_definitions under tests",
            ok,
            f"sites={definition_sites} problems={bad_sites or 'none'}",
        )
        probe_defined = any(TEST_SWITCH in line for line in comment_free_lines(self.root / PROBE_CMAKE))
        self.record("Q5: macOS probe build does not define the portable fallback switch",
                    not probe_defined, PROBE_CMAKE.as_posix())

    # ------------------------------------------------------------- driver --
    def run(self) -> None:
        root_cmake = self.root / ROOT_CMAKE
        if not root_cmake.exists():
            self.record("root CMakeLists.txt exists", False, str(root_cmake))
            return
        self.record("root CMakeLists.txt exists", True, str(root_cmake))
        self.check_forced_include_order("Q1/Q2 root", root_cmake)
        probe = self.root / PROBE_CMAKE
        if probe.exists():
            self.check_forced_include_order("Q1/Q2 probe", probe)
        else:
            self.record("probe project present", False, f"missing {PROBE_CMAKE}")
        cmake_files, enumeration = list_cmake_files(self.root)
        self.record(
            f"enumeration: {len(cmake_files)} CMake files audited ({enumeration})",
            True,
            "git ls-files index: untracked/ignored trees (artifacts/, .snow/, evidence-*/) excluded"
            if enumeration == "git-tracked"
            else "root is not a git work tree: directory-name fallback applied, "
                 "ignored trees cannot be proven excluded",
        )
        self.check_no_other_forced_includes(cmake_files)
        self.check_no_pch_unity(cmake_files)
        self.check_shim_header_shape()
        self.check_werror_inventory(cmake_files)
        self.check_test_switch_isolation(cmake_files)

    def report(self) -> str:
        lines = [finding.render() for finding in self.findings]
        failed = [finding.name for finding in self.findings if not finding.ok]
        lines.append(f"-Werror (non-MSVC) inventory: {self.werror_non_msvc or ['none']}")
        lines.append(f"/WX (MSVC) inventory: {self.werror_msvc or ['none']}")
        lines.append("RESULT: " + ("PASS" if not failed else f"FAIL ({len(failed)}): {failed}"))
        return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=None, help="repository root to audit")
    parser.add_argument("--check", action="store_true", help="exit non-zero when any hard check fails")
    args = parser.parse_args(argv)
    root = args.root or Path(__file__).resolve().parents[2]
    auditor = Auditor(root)
    auditor.run()
    print(auditor.report())
    if args.check and any(not finding.ok for finding in auditor.findings):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
