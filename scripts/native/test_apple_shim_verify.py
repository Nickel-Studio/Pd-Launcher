#!/usr/bin/env python3
"""Host-side tests for scripts/native/apple_shim_verify.py (no compiler needed).

Run:  python -B scripts/native/test_apple_shim_verify.py

Covers:
  1. the real repository tree passes every audit check (17 findings);
  2. synthetic git fixture trees that violate each invariant are rejected
     (include order, unquoted root path, unquoted probe path, /FI form,
     directory-scope test-switch pollution) so the checker actually has teeth;
  3. enumeration scope: an untracked/ignored ``artifacts/`` snapshot with its own
     violating CMakeLists is ignored, while the same file tracked by git fails;
  4. ``--check`` exits 0 on the good tree and non-zero on each broken tree.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "apple_shim_verify.py"
REPO_ROOT = SCRIPT.resolve().parents[2]

GOOD_SHIM = """#pragma once
#include <version>
#if defined(_LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN) && !defined(_LIBCPP_ENABLE_EXPERIMENTAL)
#define _LIBCPP_ENABLE_EXPERIMENTAL
#undef _LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN
#endif
#if __has_include(<stop_token>)
#include <stop_token>
#endif
#define NICKEL_STOP_TOKEN ::std::stop_token
#define NICKEL_STOP_SOURCE ::std::stop_source
"""

GOOD_ROOT_CMAKE = """cmake_minimum_required(VERSION 3.25)
project(Fake LANGUAGES CXX)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()
if(APPLE)
    add_compile_options("SHELL:$<$<COMPILE_LANGUAGE:CXX>:-include \\"${CMAKE_CURRENT_SOURCE_DIR}/native/core/common/stop_compat.hpp\\">")
endif()
include(extra/deps.cmake)
add_subdirectory(native/core/common)
add_subdirectory(native/app)
"""

GOOD_EXTRA_DEPS = """# wrapper only, no targets
set(FAKE_DEP TRUE)
"""

GOOD_DOMAIN_CMAKE = """add_library(nickel_app STATIC app.cpp)
if(MSVC)
    target_compile_options(nickel_app PRIVATE /W4 /WX)
else()
    target_compile_options(nickel_app PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
"""

GOOD_PROBE_CMAKE = """cmake_minimum_required(VERSION 3.25)
project(Probe LANGUAGES CXX)
set(NICKEL_PROBE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "source root")
get_filename_component(NICKEL_PROBE_ROOT "${NICKEL_PROBE_ROOT}" ABSOLUTE)
if(APPLE)
    add_compile_options("SHELL:$<$<COMPILE_LANGUAGE:CXX>:-include \\"${NICKEL_PROBE_ROOT}/native/core/common/stop_compat.hpp\\">")
endif()
add_subdirectory(native/core/common)
add_executable(probe_tests probe.cpp)
"""

# Historic predecessor source snapshot: carries its own CMakeLists with -Werror.
# In the real workspace this lives under the ignored artifacts/ directory.
HISTORIC_CMAKE = """cmake_minimum_required(VERSION 3.25)
project(Historic LANGUAGES CXX)
add_library(historic STATIC historic.cpp)
target_compile_options(historic PRIVATE -Wall -Wextra -Werror)
"""

FIXTURE_GITIGNORE = """/artifacts/
/evidence-*/
/.snow/
/build/
"""


def git(target: Path, *args: str) -> subprocess.CompletedProcess[str]:
    command = ["git", "-C", str(target),
               "-c", "user.email=fixture@example.invalid",
               "-c", "user.name=fixture",
               "-c", "commit.gpgsign=false", *args]
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def sync_repo(target: Path) -> None:
    """Stage everything so new files enter the index (ignored files stay out)."""
    completed = git(target, "add", "-A")
    assert completed.returncode == 0, f"git add failed: {completed.stderr}"


def init_repo(target: Path) -> None:
    completed = git(target, "init", "-q")
    assert completed.returncode == 0, f"git init failed: {completed.stderr}"
    sync_repo(target)
    completed = git(target, "commit", "-q", "-m", "fixture")
    assert completed.returncode == 0, f"git commit failed: {completed.stderr}"


def build_tree(target: Path, git_init: bool = True) -> None:
    (target / "native/core/common").mkdir(parents=True, exist_ok=True)
    (target / "native/app").mkdir(parents=True, exist_ok=True)
    (target / "extra").mkdir(parents=True, exist_ok=True)
    (target / "tests/native-macos").mkdir(parents=True, exist_ok=True)
    (target / "native/tests").mkdir(parents=True, exist_ok=True)
    (target / "native/core/common/stop_compat.hpp").write_text(GOOD_SHIM, encoding="utf-8")
    (target / "native/core/common/cancellation.hpp").write_text(
        "#pragma once\n#define NICKEL_STOP_TOKEN fake\n#define NICKEL_STOP_SOURCE fake\n", encoding="utf-8")
    (target / "native/tests/fallback.cmake").write_text(
        "target_compile_definitions(nickel_fallback_tests PRIVATE NICKEL_FORCE_PORTABLE_CANCELLATION)\n",
        encoding="utf-8")
    (target / "CMakeLists.txt").write_text(GOOD_ROOT_CMAKE, encoding="utf-8")
    (target / "extra/deps.cmake").write_text(GOOD_EXTRA_DEPS, encoding="utf-8")
    (target / "native/app/CMakeLists.txt").write_text(GOOD_DOMAIN_CMAKE, encoding="utf-8")
    (target / "tests/native-macos/CMakeLists.txt").write_text(GOOD_PROBE_CMAKE, encoding="utf-8")
    (target / ".gitignore").write_text(FIXTURE_GITIGNORE, encoding="utf-8")
    if git_init:
        init_repo(target)


def run_auditor(root: Path, check: bool = False) -> tuple[int, str]:
    command = [sys.executable, "-B", str(SCRIPT)]
    if check:
        command.append("--check")
    command += ["--root", str(root)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
    return completed.returncode, completed.stdout + completed.stderr


def find_finding(report: str, needle: str) -> str:
    for line in report.splitlines():
        if needle in line:
            return line
    raise AssertionError(f"finding not found: {needle}\n{report}")


def assert_passing(report: str, needle: str) -> None:
    line = find_finding(report, needle)
    assert "[PASS]" in line, f"expected PASS: {line}"


def assert_failing(report: str, needle: str) -> None:
    line = find_finding(report, needle)
    assert "[FAIL]" in line, f"expected FAIL: {line}"


def finding_count(report: str) -> int:
    return sum(1 for line in report.splitlines()
               if line.startswith("[PASS]") or line.startswith("[FAIL]"))


def main() -> int:
    # 1. Real repository tree must pass everything.
    code, report = run_auditor(REPO_ROOT, check=True)
    assert code == 0, f"repository audit failed ({code}):\n{report}"
    assert "RESULT: PASS" in report, report
    assert finding_count(report) == 17, f"expected 17 findings:\n{report}"
    for needle in (
        "Q1/Q2 root: forced include precedes first target",
        "Q1/Q2 probe: forced include precedes first target",
        "Q1/Q2 root: GNU -include form, SHELL:, CXX-guarded, shell-quoted absolute path",
        "Q1/Q2 probe: GNU -include form, SHELL:, CXX-guarded, shell-quoted absolute path",
        "enumeration: ",
        "Q3a: stop_compat.hpp layer-2 sequence",
        "Q4: macOS probe project uses no -Werror//WX",
        "Q5: macOS probe build does not define the portable fallback switch",
    ):
        assert_passing(report, needle)
    assert "git-tracked" in find_finding(report, "enumeration: "), report
    assert "recommendation, informational" not in report, \
        "probe quoting must be a hard assertion, not a recommendation"
    print("test_repository_tree: OK")

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)

        # 2a. Synthetic good git fixture.
        good = base / "good"
        build_tree(good)
        code, report = run_auditor(good, check=True)
        assert code == 0, f"good fixture failed:\n{report}"
        assert "RESULT: PASS" in report, report
        assert finding_count(report) == 17, f"expected 17 findings:\n{report}"
        assert_passing(report, "Q1/Q2 probe: GNU -include form")
        assert_passing(report, "enumeration: ")
        assert "git-tracked" in find_finding(report, "enumeration: "), report
        print("test_good_fixture: OK")

        # 2b. Untracked + ignored artifacts/ snapshot must be invisible.
        ignored = base / "ignored_artifacts"
        build_tree(ignored)
        predecessor = ignored / "artifacts/pd-canonical-20261121T1726Z/predecessor-source"
        predecessor.mkdir(parents=True, exist_ok=True)
        (predecessor / "CMakeLists.txt").write_text(HISTORIC_CMAKE, encoding="utf-8")
        sync_repo(ignored)
        tracked = git(ignored, "ls-files").stdout
        assert "predecessor-source" not in tracked, f"fixture setup leaked into the index:\n{tracked}"
        code, report = run_auditor(ignored, check=True)
        assert code == 0, f"ignored artifacts fixture must pass:\n{report}"
        assert "RESULT: PASS" in report, report
        assert "predecessor-source" not in report, report
        assert_passing(report, "Q4: non-MSVC -Werror inventory")
        print("test_ignored_artifacts: OK")

        # 2c. The same violating CMakeLists, now tracked, must fail.
        tracked_bad = base / "tracked_violation"
        build_tree(tracked_bad)
        violation = tracked_bad / "tools/historic/CMakeLists.txt"
        violation.parent.mkdir(parents=True, exist_ok=True)
        violation.write_text(HISTORIC_CMAKE, encoding="utf-8")
        sync_repo(tracked_bad)
        tracked = git(tracked_bad, "ls-files").stdout
        assert "tools/historic/CMakeLists.txt" in tracked, tracked
        code, report = run_auditor(tracked_bad, check=True)
        assert code != 0, "tracked violating CMakeLists must fail --check"
        assert_failing(report, "Q4: non-MSVC -Werror inventory")
        assert "tools/historic/CMakeLists.txt" in report, report
        print("test_tracked_violation: OK")

        # 2d. Non-git fallback still skips artifacts-like local trees.
        fallback = base / "fallback_no_git"
        build_tree(fallback, git_init=False)
        fallback_predecessor = fallback / "artifacts/predecessor-source"
        fallback_predecessor.mkdir(parents=True, exist_ok=True)
        (fallback_predecessor / "CMakeLists.txt").write_text(HISTORIC_CMAKE, encoding="utf-8")
        code, report = run_auditor(fallback, check=True)
        assert code == 0, f"fallback fixture must pass:\n{report}"
        assert "filesystem-fallback" in find_finding(report, "enumeration: "), report
        assert "predecessor-source" not in report, report
        print("test_filesystem_fallback: OK")

        # 2e. Mutation: forced include placed after the first add_subdirectory.
        late = base / "late"
        build_tree(late)
        text = (late / "CMakeLists.txt").read_text(encoding="utf-8")
        include_block = [line for line in text.splitlines() if "-include" in line][0]
        text = text.replace(include_block + "\n", "")
        text = text.replace("add_subdirectory(native/core/common)\n",
                            "add_subdirectory(native/core/common)\n" + include_block + "\n")
        (late / "CMakeLists.txt").write_text(text, encoding="utf-8")
        code, report = run_auditor(late, check=True)
        assert code != 0, "late-include fixture must fail --check"
        assert_failing(report, "Q1/Q2 root: forced include precedes first target")
        print("test_late_include: OK")

        # 2f. Mutation: unquoted path inside the root SHELL: statement.
        unquoted = base / "unquoted"
        build_tree(unquoted)
        text = (unquoted / "CMakeLists.txt").read_text(encoding="utf-8")
        text = text.replace('\\"', '"')
        (unquoted / "CMakeLists.txt").write_text(text, encoding="utf-8")
        code, report = run_auditor(unquoted, check=True)
        assert code != 0, "unquoted fixture must fail --check"
        assert_failing(report, "Q1/Q2 root: GNU -include form")
        print("test_unquoted_path: OK")

        # 2g. Mutation: unquoted path inside the probe SHELL: statement.
        probe_unquoted = base / "probe_unquoted"
        build_tree(probe_unquoted)
        probe_path = probe_unquoted / "tests/native-macos/CMakeLists.txt"
        text = probe_path.read_text(encoding="utf-8")
        assert '\\"' in text, text
        probe_path.write_text(text.replace('\\"', '"'), encoding="utf-8")
        code, report = run_auditor(probe_unquoted, check=True)
        assert code != 0, "unquoted probe fixture must fail --check"
        assert_failing(report, "Q1/Q2 probe: GNU -include form")
        print("test_unquoted_probe_path: OK")

        # 2h. Mutation: MSVC /FI instead of GNU -include.
        msvc_style = base / "msvc_style"
        build_tree(msvc_style)
        text = (msvc_style / "CMakeLists.txt").read_text(encoding="utf-8")
        text = text.replace("-include", "/FI")
        (msvc_style / "CMakeLists.txt").write_text(text, encoding="utf-8")
        code, report = run_auditor(msvc_style, check=True)
        assert code != 0, "/FI fixture must fail --check"
        assert_failing(report, "Q1/Q2 root: forced include present")
        print("test_fi_flag: OK")

        # 2i. Mutation: production directory-scope definition of the test switch.
        polluted = base / "polluted"
        build_tree(polluted)
        text = (polluted / "CMakeLists.txt").read_text(encoding="utf-8")
        text += "add_compile_definitions(NICKEL_FORCE_PORTABLE_CANCELLATION)\n"
        (polluted / "CMakeLists.txt").write_text(text, encoding="utf-8")
        code, report = run_auditor(polluted, check=True)
        assert code != 0, "polluted fixture must fail --check"
        assert_failing(report, "Q5: test-only switch")
        print("test_switch_pollution: OK")

    print("ALL apple_shim_verify HOST TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
