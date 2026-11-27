#!/usr/bin/env python3
"""Source-bound LP64 lambda contract checks; never product acceptance."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCES = (
    "native/core/minecraft/services_native.cpp",
    "native/core/minecraft/services.hpp",
    "native/core/archive/archive.hpp",
)
UNTYPED = "std::min(maximumBytes, 64ULL * 1024 * 1024)"
TYPED = "std::min<std::uint64_t>(maximumBytes, 64ULL * 1024 * 1024)"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=ROOT, timeout=30)


def archive_block(source: str) -> str:
    matches = list(re.finditer(
        r"^    result\.archiveRead = \[operation\]\([^\n]*\) \{\n.*?^    \};\n",
        source, re.MULTILINE | re.DOTALL,
    ))
    if len(matches) != 1:
        raise ValueError("Expected exactly one complete archiveRead lambda")
    block = matches[0].group()
    for left, right in (("{", "}"), ("(", ")"), ("[", "]")):
        if block.count(left) != block.count(right):
            raise ValueError("Unbalanced archiveRead lambda")
    if block.count("check();") != 2:
        raise ValueError("The before/after cancellation contract has changed")
    return block


def candidate_block(block: str) -> tuple[str, bool]:
    if block.count(TYPED) == 1 and UNTYPED not in block:
        return block, True
    if block.count(UNTYPED) != 1 or TYPED in block:
        raise ValueError("The reviewed min expression has changed")
    return block.replace(UNTYPED, TYPED), False


def declaration(source: str, pattern: str) -> str:
    matches = list(re.finditer(pattern, source, re.MULTILINE))
    if len(matches) != 1:
        raise ValueError("The reviewed public declaration has changed")
    return matches[0].group()


def bind_source(commit: str, expected: str) -> tuple[dict, dict[str, str]]:
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("An explicit full source commit is required")
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("An explicit committed source SHA256 is required")
    actual = git("rev-parse", "HEAD").decode().strip()
    if actual != commit:
        raise ValueError("HEAD differs from the explicit source commit")
    records, contents = {}, {}
    runner_name = Path(__file__).resolve().relative_to(ROOT).as_posix()
    for name in (*SOURCES, runner_name):
        raw = (ROOT / name).read_bytes()
        committed = git("show", commit + ":" + name)
        if raw.replace(b"\r\n", b"\n") != committed:
            raise ValueError("Working source differs from its commit: " + name)
        records[name] = {
            "gitBlob": git("rev-parse", commit + ":" + name).decode().strip(),
            "committedBytes": len(committed), "committedSha256": sha256(committed),
            "workingBytes": len(raw), "workingSha256": sha256(raw),
        }
        contents[name] = committed.decode("utf-8")
    if records[SOURCES[0]]["committedSha256"] != expected:
        raise ValueError("Production input differs from the independently pinned SHA256")
    return {
        "sourceCommit": actual, "sourceTree": git("rev-parse", "HEAD^{tree}").decode().strip(),
        "files": records, "runnerSha256": sha256(Path(__file__).read_bytes()),
    }, contents


def fixture(block: str, sources: dict[str, str]) -> str:
    service_field = declaration(sources[SOURCES[1]],
        r"    std::function<std::string\(const std::filesystem::path&, std::string_view entry,\n"
        r"                              std::uint64_t maximumBytes, const CancelCheck&\)> archiveRead;")
    archive_method = declaration(sources[SOURCES[2]],
        r"    static std::vector<std::byte> read_entry\([^;]+;")
    # The lambda and public declarations are real; archive IO is deliberately a fixture.
    return r'''#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
using CancelCheck = std::function<void()>;
struct Cancellation {};
namespace archive {
struct Limits {};
inline std::uint64_t observed = 0;
inline unsigned calls = 0;
class Archive {
public:
''' + archive_method + r'''
};
std::vector<std::byte> Archive::read_entry(const std::filesystem::path&, std::string_view,
    std::uint64_t max_bytes, const Cancellation&, const Limits&) {
    observed = max_bytes; ++calls;
    return {std::byte{'o'}, std::byte{'k'}};
}
}
archive::Limits archive_limits() { return {}; }
struct Operation { Cancellation cancellation; };
struct Services {
''' + service_field + r'''
};
constexpr std::uint64_t quota = 64ULL * 1024 * 1024;
template<class U> constexpr U capped(U value) {
    return std::min<U>(value, 64ULL * 1024 * 1024);
}
static_assert(capped<unsigned long>(0) == 0);
static_assert(capped<unsigned long long>(0) == 0);
static_assert(capped(std::numeric_limits<unsigned long>::max()) == quota);
static_assert(capped(std::numeric_limits<unsigned long long>::max()) == quota);
static_assert(std::is_same_v<decltype(capped(0UL)), unsigned long>);
static_assert(std::is_same_v<decltype(capped(0ULL)), unsigned long long>);
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    auto operation = std::make_shared<Operation>();
    Services result;
''' + block + r'''
    constexpr std::array<std::uint64_t, 10> cases = {
        0, 1, 64 * 1024, 1024 * 1024, 16 * 1024 * 1024,
        quota - 1, quota, quota + 1, (1ULL << 32) + 1,
        std::numeric_limits<std::uint64_t>::max()
    };
    for (const auto maximum : cases) {
        unsigned checks = 0;
        const auto before = archive::calls;
        const auto bytes = result.archiveRead({}, "fixture", maximum, [&] { ++checks; });
        require(bytes == "ok", "Returned bytes changed");
        require(checks == 2, "Cancellation checks changed");
        require(archive::calls == before + 1, "Archive call count changed");
        const auto expected = maximum < quota ? maximum : quota;
        require(archive::observed == expected, "Quota clamping or uint64 preservation changed");
    }
    const auto before = archive::calls;
    unsigned checks = 0;
    bool cancelled = false;
    try { result.archiveRead({}, "fixture", quota, [&] {
        ++checks; throw std::runtime_error("cancel before");
    }); } catch (const std::runtime_error&) { cancelled = true; }
    require(cancelled && checks == 1 && archive::calls == before, "Pre-read cancellation changed");
    checks = 0; cancelled = false;
    try { result.archiveRead({}, "fixture", quota, [&] {
        if (++checks == 2) throw std::runtime_error("cancel after");
    }); } catch (const std::runtime_error&) { cancelled = true; }
    require(cancelled && checks == 2 && archive::calls == before + 1, "Post-read cancellation changed");
    std::cout << std::boolalpha << "{\"contractCases\":12,\"sizeofLong\":" << sizeof(long)
        << ",\"sizeofLongLong\":" << sizeof(long long)
        << ",\"uint64IsUnsignedLong\":" << std::is_same_v<std::uint64_t, unsigned long>
        << ",\"uint64IsUnsignedLongLong\":" << std::is_same_v<std::uint64_t, unsigned long long>
        << ",\"fixturePassed\":true}\n";
}
'''


def write_new(path: Path, data: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(data)


def command(argv: list[str], output: Path, label: str) -> dict:
    env = dict(os.environ, TEMP=str(output), TMP=str(output), TMPDIR=str(output))
    completed = subprocess.run(argv, cwd=ROOT, env=env, capture_output=True, timeout=90)
    record = {"argv": argv, "returnCode": completed.returncode}
    for kind, data in (("stdout", completed.stdout), ("stderr", completed.stderr)):
        name = label + "." + kind + ".log"
        write_new(output / name, data)
        record[kind] = {"path": name, "bytes": len(data), "sha256": sha256(data)}
    return record


def run(args: argparse.Namespace) -> int:
    before, sources = bind_source(args.source_commit, args.expected_source_sha256)
    production = archive_block(sources[SOURCES[0]])
    candidate, already_fixed = candidate_block(production)
    if not already_fixed:
        raise ValueError("The committed production lambda is not fixed; a candidate is not a regression pass")
    negative = candidate.replace(TYPED, UNTYPED)
    output = (ROOT / args.output_directory).absolute()
    scratch = ROOT / "tests/native-portability/scratch"
    if ".." in Path(args.output_directory).parts or not output.is_relative_to(scratch):
        raise ValueError("Outputs must be inside this worktree's portability scratch")
    if output.exists() or any(p.is_symlink() or p.is_junction() for p in [output, *output.parents] if p != ROOT):
        raise ValueError("Preserve existing outputs and refuse redirected output paths")
    if shutil.disk_usage(ROOT).free < 268435456 + 8 * 1024 * 1024:
        raise ValueError("Insufficient physical reserve for the bounded contract check")
    output.mkdir(parents=True)
    report = {
        "scope": "exact fixed source lambda / typed-min compiler contract; synthetic archive IO",
        "sourceBinding": before, "productionBlockSha256": sha256(production.encode()),
        "untypedNegativeBlockSha256": sha256(negative.encode()), "productionAlreadyFixed": already_fixed,
        "productionFileModifiedByRunner": False, "productBuildExecuted": False,
        "productAccepted": False, "hostSystem": platform.system(), "hostMachine": platform.machine(),
        "contractGatePassed": False, "commands": [],
    }
    write_new(output / "source-binding-before.json", json.dumps(before, indent=2).encode() + b"\n")
    try:
        compiler = shutil.which(args.compiler)
        if not compiler:
            raise ValueError("Required compiler is unavailable; do not mark this skipped/pass")
        report["commands"].append(command([compiler, "--version"], output, "compiler-version"))
        production_file, negative_file = output / "production-lambda.cpp", output / "untyped-negative.cpp"
        write_new(production_file, fixture(production, sources).encode("ascii"))
        write_new(negative_file, fixture(negative, sources).encode("ascii"))
        executable = output / ("lambda-contract.exe" if os.name == "nt" else "lambda-contract")
        flags = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
        compile_result = command([*flags, str(production_file), "-o", str(executable)], output, "compile-production-lambda")
        report["commands"].append(compile_result)
        if compile_result["returnCode"]:
            raise ValueError("The exact committed production lambda did not compile")
        execution = command([str(executable)], output, "run-lambda-contract")
        report["commands"].append(execution)
        if execution["returnCode"]:
            raise ValueError("Production lambda fixture contract failed")
        runtime = json.loads((output / "run-lambda-contract.stdout.log").read_bytes())
        if runtime.get("contractCases") != 12 or runtime.get("fixturePassed") is not True:
            raise ValueError("Fixture receipt does not match the executed contract")
        report["runtime"] = runtime
        report["executable"] = {"bytes": executable.stat().st_size, "sha256": sha256(executable.read_bytes())}
        baseline = command([*flags, "-fsyntax-only", str(negative_file)], output, "compile-untyped-negative")
        report["commands"].append(baseline)
        negative_expected = runtime["uint64IsUnsignedLong"]
        report["negativeCompileFailureExpected"] = negative_expected
        if negative_expected:
            diagnostics = (output / "compile-untyped-negative.stderr.log").read_text(errors="replace")
            if baseline["returnCode"] == 0 or "min(" not in diagnostics:
                raise ValueError("The original LP64 min failure was not reproduced")
        elif baseline["returnCode"]:
            raise ValueError("The untyped negative lambda failed on a matching-type ABI")
        after, _ = bind_source(args.source_commit, args.expected_source_sha256)
        write_new(output / "source-binding-after.json", json.dumps(after, indent=2).encode() + b"\n")
        if after != before:
            raise ValueError("Source/runner identity changed while the contract check ran")
        report["inputIdentityUnchanged"] = True
        report["contractGatePassed"] = True
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
    report["outputBytesBeforeReceipt"] = sum(p.stat().st_size for p in output.iterdir() if p.is_file())
    if report["outputBytesBeforeReceipt"] > 8 * 1024 * 1024:
        report["contractGatePassed"] = False
        report["error"] = "The small contract workload exceeded its 8 MiB output budget"
    write_new(output / "result.json", json.dumps(report, indent=2).encode() + b"\n")
    print(json.dumps(report, indent=2))
    return 0 if report["contractGatePassed"] else 1


class ExtractionTests(unittest.TestCase):
    def sample(self, expression: str = UNTYPED) -> str:
        return ("    result.archiveRead = [operation](std::uint64_t maximumBytes) {\n"
                "        check();\n        auto bytes = " + expression + ";\n"
                "        check(); return bytes;\n    };\n")

    def test_complete_block(self):
        self.assertEqual(archive_block(self.sample()), self.sample())

    def test_only_min_changes(self):
        candidate, fixed = candidate_block(archive_block(self.sample()))
        self.assertFalse(fixed)
        self.assertEqual(candidate, self.sample(TYPED))

    def test_already_typed_is_not_rewritten(self):
        block = self.sample(TYPED)
        self.assertEqual(candidate_block(block), (block, True))

    def test_missing_closure_is_rejected(self):
        with self.assertRaises(ValueError):
            archive_block(self.sample().removesuffix("    };\n"))

    def test_duplicate_block_is_rejected(self):
        with self.assertRaises(ValueError):
            archive_block(self.sample() * 2)

    def test_changed_cancellation_is_rejected(self):
        with self.assertRaises(ValueError):
            archive_block(self.sample().replace("check();", "check_changed();", 1))

    def test_changed_quota_is_rejected(self):
        with self.assertRaises(ValueError):
            candidate_block(self.sample().replace("64ULL", "128ULL"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--source-commit")
    parser.add_argument("--expected-source-sha256")
    parser.add_argument("--output-directory")
    parser.add_argument("--compiler", default="g++")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ExtractionTests)
        sys.exit(0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1)
    if not all((args.source_commit, args.expected_source_sha256, args.output_directory)):
        parser.error("Require explicit source commit, source SHA256 and a fresh owned output directory")
    try:
        sys.exit(run(args))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
