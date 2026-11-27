#!/usr/bin/env python3
"""Execute byte-exact committed CMake resource blocks with real CMake/Ninja."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

from run_regression import OWNED, ROOT, Regression, digest, owned_size, write_json


BRAND_CMAKE = "native/tests/brand/CMakeLists.txt"
APP_CMAKE = "native/app/CMakeLists.txt"
MARKER = b"# Original artwork comes from the oracle; brand/map materials stay with the test sources.\n"
APP_MARKER = b"# Only relocate the immutable oracle asset paths in the generated build input.\n"
FIX_DEFAULT = "df4347b81f71349f62736220dfe58bd056936c39"
BASE_DEFAULT = "caa8790910dce129af9d2d4519960d5167410a5e"


def resource_block(data, marker, final_command):
    lines = data.splitlines(keepends=True)
    starts = [index for index, line in enumerate(lines) if line == marker]
    if len(starts) != 1:
        raise RuntimeError("Expected exactly one audited resource block marker")
    first = starts[0]
    ends = [index for index in range(first, len(lines)) if lines[index].startswith(final_command)]
    if len(ends) != 1:
        raise RuntimeError("Expected exactly one audited final complete command")
    last = ends[0]
    block = b"".join(lines[first:last + 1])
    if any(token in block for token in (b"function(", b"macro(", b"foreach(", b"if(")):
        raise RuntimeError("Refuse to extract a partial nested CMake code unit")
    if not block.rstrip().endswith(b")"):
        raise RuntimeError("The resource block must end with a complete CMake command")
    return block, first + 1, last + 1


class CommittedRegression(Regression):
    def __init__(self, args):
        super().__init__(args)
        self.sources.extend(path for path in sorted((OWNED / "committed-fixture").rglob("*")) if path.is_file())
        self.sources.append(OWNED / "run_committed_regression.py")
        self.summary["sourceHashes"] = self.source_hashes()
        self.summary["scope"] = "Windows local committed-resource generation only; not root build, Qt runtime, or cross-platform acceptance"
        self.summary["originalModuleTested"] = False
        self.summary["originalModuleHistoricalOnly"] = str(self.module)
        self.summary["fixCommitRequested"] = args.fix_commit
        self.summary["baseCommitRequested"] = args.base_commit
        self.payloads = self.records / "git-payloads"
        self.payloads.mkdir()
        self.save()

    def git_scalar(self, label, argv):
        record, output = self.execute(label, ["git", *argv])
        self.check(f"{label}: read-only Git query exits 0", record["returnCode"] == 0, output)
        return output.strip()

    def git_payload(self, label, commit, path):
        record, output = self.execute(label, ["git", "show", f"{commit}:{path}"])
        self.check(f"{label}: original Git blob available", record["returnCode"] == 0, output)
        raw = (ROOT / record["stdout"]).read_bytes()
        self.summary.setdefault("gitPayloads", {})[label] = {
            "commit": commit, "path": path, "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(), "record": record["stdout"],
        }
        self.save()
        return raw

    def prepare(self):
        self.fixed_commit = self.git_scalar("git.fix-commit", ["rev-parse", f"{self.args.fix_commit}^{{commit}}"])
        self.before_commit = self.git_scalar("git.fix-parent", ["rev-parse", f"{self.fixed_commit}^"])
        self.base_commit = self.git_scalar("git.base-commit", ["rev-parse", f"{self.args.base_commit}^{{commit}}"])
        changed = self.git_scalar("git.fix-owned-scope", ["diff-tree", "--no-commit-id", "--name-only", "-r", self.fixed_commit])
        self.check("brand emergency commit changes only the two committed owner paths",
                   set(changed.splitlines()) == {BRAND_CMAKE, "native/tests/brand/test_brand.py"}, changed)
        self.summary["fixCommit"] = self.fixed_commit
        self.summary["beforeCommit"] = self.before_commit
        self.summary["baseCommit"] = self.base_commit
        self.before_brand = self.git_payload("git.before-brand-cmake", self.before_commit, BRAND_CMAKE)
        self.fixed_brand = self.git_payload("git.fixed-brand-cmake", self.fixed_commit, BRAND_CMAKE)
        self.app = self.git_payload("git.base-app-cmake", self.base_commit, APP_CMAKE)
        self.metadata = self.git_payload("git.base-build-metadata", self.base_commit, "native/app/service/build_metadata.cpp")
        self.application_qrc = self.git_payload("git.base-application-qrc", self.base_commit, "native/app/service/application_resources.qrc")
        self.before_test_cpp = self.git_payload("git.before-brand-test-cpp", self.before_commit, "native/tests/brand/brand_resources_test.cpp")
        self.fixed_test_cpp = self.git_payload("git.fixed-brand-test-cpp", self.fixed_commit, "native/tests/brand/brand_resources_test.cpp")
        self.before_test_py = self.git_payload("git.before-brand-test-py", self.before_commit, "native/tests/brand/test_brand.py")
        self.fixed_test_py = self.git_payload("git.fixed-brand-test-py", self.fixed_commit, "native/tests/brand/test_brand.py")
        self.blocks = {}
        for case, data, commit, path, marker, end in (
            ("before-brand", self.before_brand, self.before_commit, BRAND_CMAKE, MARKER, b"file(WRITE \"${_nickel_functional_qrc_path}\""),
            ("fixed-brand", self.fixed_brand, self.fixed_commit, BRAND_CMAKE, MARKER, b"file(WRITE \"${_nickel_functional_qrc_path}\""),
            ("base-app", self.app, self.base_commit, APP_CMAKE, APP_MARKER, b"set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS"),
        ):
            block, start, finish = resource_block(data, marker, end)
            destination = self.payloads / f"{case}.fragment.cmake"
            destination.write_bytes(block)
            self.blocks[case] = block
            self.summary.setdefault("consumedBlocks", {})[case] = {
                "originCommit": commit, "originPath": path, "startLine": start, "endLine": finish,
                "wholeBlobSha256": hashlib.sha256(data).hexdigest(),
                "blockSha256": hashlib.sha256(block).hexdigest(), "blockBytes": len(block),
                "savedFragment": destination.relative_to(ROOT).as_posix(), "modified": False,
            }
        self.save()

    def fixture_case(self, case, fixed, special=False):
        source = self.scratch / case / ("source & oracle" if special else "source")
        shutil.copytree(OWNED / "committed-fixture", source)
        brand_fragment = source / "native/tests/brand/brand-resource-input.fragment.cmake"
        app_fragment = source / "native/app/app-resource-input.fragment.cmake"
        brand_fragment.write_bytes(self.blocks["fixed-brand" if fixed else "before-brand"])
        app_fragment.write_bytes(self.blocks["base-app"])
        self.check(f"{case}: consumed fragment bytes equal the immutable Git blocks",
                   brand_fragment.read_bytes() == self.blocks["fixed-brand" if fixed else "before-brand"]
                   and app_fragment.read_bytes() == self.blocks["base-app"])
        return source, self.scratch / case / "build"

    def configure_case(self, label, source, build):
        trace = self.records / f"{label}.trace.jsonl"
        argv = [self.cmake, "-S", source, "-B", build, "-G", "Ninja",
                f"-DCMAKE_MAKE_PROGRAM:FILEPATH={self.ninja.as_posix()}",
                "--trace-expand", "--trace-format=json-v1", f"--trace-redirect={trace}",
                f"--trace-source={source / 'native/app/app-resource-input.fragment.cmake'}",
                f"--trace-source={source / 'native/tests/brand/brand-resource-input.fragment.cmake'}"]
        return self.execute(f"{label}.configure", argv)

    def before_case(self):
        source, build = self.fixture_case("committed-before", False)
        record, output = self.configure_case("committed-before", source, build)
        self.check("committed parent CMake configure/generate exits 0", record["returnCode"] == 0, output)
        self.archive_generation("committed-before", build)
        record, output = self.execute("committed-before.ninja-parse", [self.ninja, "-C", build, "-j", "1", "-n"])
        diagnostics = ("multiple rules generate", "duplicate", "is defined as an output multiple times")
        self.check("actual committed parent reproduces the resource duplicate Ninja failure",
                   record["returnCode"] != 0 and "functional-resources.qrc" in output
                   and any(message in output for message in diagnostics), output)
        self.summary["committedBefore"] = "EXPECTED_REAL_NINJA_FAILURE_REPRODUCED"
        self.save()

    def check_outputs(self, source, build, label):
        app = build / "native/app/functional-resources.qrc"
        brand = build / "native/tests/brand/nickel_brand_functional_resources.qrc"
        copies = [build / "native/app/app-resource-copy.qrc", build / "native/tests/brand/brand-resource-copy.qrc"]
        self.check(f"{label}: real custom commands copied both generated resources",
                   all(path.is_file() for path in (app, brand, *copies))
                   and digest(app) == digest(copies[0]) and digest(brand) == digest(copies[1]))
        entries = []
        for path in (app, brand):
            tree = ET.parse(path).getroot()
            qresource = tree.find("qresource")
            entry = qresource.find("file")
            actual = Path(entry.text)
            entries.append({"resource": str(path), "prefix": qresource.get("prefix"),
                            "alias": entry.get("alias"), "source": str(actual)})
            self.check(f"{label}: {path.name} preserves alias and XML-escaped oracle reference",
                       qresource.get("prefix") == "/nickel/functional" and entry.get("alias") == "fixture.txt"
                       and actual.is_absolute() and actual.samefile(source / "src/assets/fixture.txt"), entries[-1])
        self.check(f"{label}: test-only basename does not replace production functional resource",
                   app.name == "functional-resources.qrc" and brand.name == "nickel_brand_functional_resources.qrc"
                   and not (brand.parent / "functional-resources.qrc").exists())
        contract = (build / "native/tests/brand/brand-configure-inputs.txt").read_text(encoding="utf-8").splitlines()
        self.check(f"{label}: configure dependency is canonical before CMake records it",
                   contract[0] == (source / "native/assets/brand/functional-resources.qrc").as_posix()
                   and "/../" not in contract[0], contract)
        self.summary.setdefault("generatedResourceHashes", {})[label] = {
            "app": digest(app), "brand": digest(brand), "appCopy": digest(copies[0]), "brandCopy": digest(copies[1]),
        }
        self.save()

    def trace_contract(self, case, source):
        trace = self.records / f"{case}.trace.jsonl"
        events = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        fragment = (source / "native/tests/brand/brand-resource-input.fragment.cmake").as_posix().lower()
        commands = [event for event in events if event.get("file", "").replace("\\", "/").lower() == fragment and event.get("cmd")]
        names = [event["cmd"].lower() for event in commands]
        normalize = names.index("get_filename_component")
        read = next(index for index, event in enumerate(commands) if event["cmd"].lower() == "file" and event["args"][0] == "READ")
        register = names.index("set_property")
        self.check(f"{case}: executed trace normalizes before READ and dependency registration",
                   normalize < read < register, names)
        expected = (source / "native/assets/brand/functional-resources.qrc").as_posix()
        self.check(f"{case}: real READ and configure dependency use the same canonical path",
                   commands[read]["args"][1] == expected and commands[register]["args"][-1] == expected)
        self.summary.setdefault("traceHashes", {})[case] = digest(trace)
        self.save()

    def after_case(self, case, special=False, regenerate=False):
        source, build = self.fixture_case(case, True, special)
        record, output = self.configure_case(case, source, build)
        self.check(f"{case}: committed fix configure/generate exits 0", record["returnCode"] == 0, output)
        self.archive_generation(f"{case}-initial", build)
        record, output = self.execute(f"{case}.ninja-parse", [self.ninja, "-C", build, "-j", "1", "-n"])
        self.check(f"{case}: real Ninja parse exits 0 without dupbuild warn", record["returnCode"] == 0, output)
        record, output = self.execute(f"{case}.build", [self.cmake, "--build", build, "--parallel", "1"])
        self.check(f"{case}: real single-worker two-command build exits 0", record["returnCode"] == 0, output)
        self.check_outputs(source, build, f"{case}-initial")
        self.trace_contract(case, source)
        if regenerate:
            record, output = self.configure_case(f"{case}-repeat", source, build)
            self.check(f"{case}: repeated configure exits 0", record["returnCode"] == 0, output)
            record, output = self.execute(f"{case}.repeat-build", [self.ninja, "-C", build, "-j", "1"])
            self.check(f"{case}: repeated build exits 0", record["returnCode"] == 0, output)
            resource = source / "native/assets/brand/functional-resources.qrc"
            before = digest(resource)
            deadline = max(path.stat().st_mtime_ns for path in build.rglob("*.ninja")) + 2_000_000_000
            while time.time_ns() <= deadline:
                time.sleep(0.05)
            with resource.open("ab") as stream:
                stream.write(b"<!-- scratch mutation must cause real CMake regeneration -->\n")
            self.summary["resourceMutation"] = {
                "path": str(resource), "beforeSha256": before, "afterSha256": digest(resource),
                "mtimeNs": resource.stat().st_mtime_ns, "futureMtimeForced": False,
                "productionInputModified": False,
            }
            record, output = self.execute(f"{case}.resource-regenerate", [self.ninja, "-C", build, "-j", "1"])
            self.check(f"{case}: resource mutation triggers actual successful CMake regeneration",
                       record["returnCode"] == 0 and "Re-running CMake" in output, output)
            self.check_outputs(source, build, f"{case}-regenerated")
            self.check(f"{case}: mutation appears in both current generated resource files",
                       b"scratch mutation" in (build / "native/app/functional-resources.qrc").read_bytes()
                       and b"scratch mutation" in (build / "native/tests/brand/nickel_brand_functional_resources.qrc").read_bytes())
            record, output = self.execute(f"{case}.final-parse", [self.ninja, "-C", build, "-j", "1", "-n"])
            self.check(f"{case}: regenerated Ninja still parses", record["returnCode"] == 0, output)
            self.archive_generation(f"{case}-final", build)
        self.summary.setdefault("committedAfter", {})[case] = "REAL_CMAKE_NINJA_BUILD_PASS"
        self.save()

    def naming_contract(self):
        old_names = re.findall(rb"Q_INIT_RESOURCE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", self.metadata)
        self.check("base root manual Q_INIT_RESOURCE remains application_resources only", old_names == [b"application_resources"])
        self.check("brand test C++ has no manual Q_INIT_RESOURCE before or after",
                   b"Q_INIT_RESOURCE" not in self.before_test_cpp and b"Q_INIT_RESOURCE" not in self.fixed_test_cpp
                   and self.before_test_cpp == self.fixed_test_cpp)
        self.check("base app production functional and application QRC basenames are unchanged",
                   b'"${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc"' in self.app
                   and b"service/application_resources.qrc" in self.app and b'"${CMAKE_CURRENT_BINARY_DIR}/application_resources.qrc"' in self.app)
        self.check("fixed brand target still consumes its real generated QRC variable",
                   b'"${_nickel_functional_qrc_path}"' in self.fixed_brand.split(b"add_executable(nickel_brand_resource_test", 1)[1]
                   and b"set(CMAKE_AUTORCC ON)" in self.fixed_brand)
        self.check("committed test script follows test-only generated basename",
                   b'generated = output / "nickel_brand_functional_resources.qrc"' in self.fixed_test_py)
        self.summary["resourceNaming"] = {
            "status": "SOURCE_AND_REAL_GENERATED_QRC_CONTRACT_VERIFIED_NO_QT_LINK_CLAIM",
            "rootFunctionalQrc": "functional-resources.qrc",
            "rootFunctionalSymbolConvention": "functional_resources",
            "testOnlyQrc": "nickel_brand_functional_resources.qrc",
            "testOnlySymbolConvention": "nickel_brand_functional_resources",
            "manualApplicationInit": "application_resources",
            "manualTestInit": None,
            "rccOrQtLinkExecuted": False,
            "immutableApplicationQrcSha256": hashlib.sha256(self.application_qrc).hexdigest(),
        }
        self.save()

    def verify(self):
        self.prepare()
        self.before_case()
        self.after_case("committed-fixed", regenerate=True)
        self.after_case("committed-fixed-xml-path", special=True)
        self.naming_contract()
        self.check("all consumed blocks remain byte-exact after real CMake/Ninja execution",
                   all((self.payloads / f"{case}.fragment.cmake").read_bytes() == block for case, block in self.blocks.items()))
        self.summary["verification"] = "BYTE_EXACT_COMMITTED_PARENT_FAIL_FIXED_CMAKE_NINJA_EXIT0"
        self.save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", default=datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-committed")
    parser.add_argument("--fix-commit", default=FIX_DEFAULT)
    parser.add_argument("--base-commit", default=BASE_DEFAULT)
    parser.add_argument("--cmake")
    parser.add_argument("--ninja")
    parser.add_argument("--timeout", type=int, choices=range(1, 121), default=60, metavar="1..120")
    args = parser.parse_args()
    for value in (args.fix_commit, args.base_commit):
        if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
            parser.error("Commit inputs must be exact 40-hex local Git commit IDs")
    args.phase = "verify"
    args.module = "cmake/NickelDomains.cmake"
    args.expected_module_sha256 = None
    try:
        return CommittedRegression(args).run()
    except Exception as error:
        print(f"Setup failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
