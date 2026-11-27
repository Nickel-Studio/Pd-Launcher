"""Bind native CI candidate evidence; this is not a release acceptance manifest."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
CLIENT = "client-pd-native-ci"
MAX_JSON_BYTES = 32 * 1024 * 1024
# Test hook macros may be spelled "/DNAME", "-DNAME", or "-D NAME" (MSVC joined form
# has no word boundary before NAME, so a plain \b anchor would miss it).
HOOK_DEFINITION = re.compile(
    r"(?:(?<=[\s\"'=,/;])|(?<=[/-])D|^)NICKEL_[A-Z0-9_]*TEST_(?:HOOKS|FAULTS)(?![A-Z0-9_])")


class EvidenceError(ValueError):
    pass


def read_json(path: Path):
    if path.stat().st_size > MAX_JSON_BYTES:
        raise EvidenceError("CI JSON exceeds its size bound: " + str(path))
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def file_ref(path: Path) -> dict:
    with path.open("rb") as handle:
        digest = hashlib.file_digest(handle, "sha256").hexdigest()
    return {"path": str(path.resolve()), "sha256": digest, "bytes": path.stat().st_size}


def write_json(path: Path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="ascii") as handle:
        json.dump(document, handle, sort_keys=True, ensure_ascii=True, allow_nan=False, indent=2)
        handle.write("\n")


def inside(path: Path, root: Path) -> Path:
    result = path.resolve()
    try:
        result.relative_to(root.resolve())
    except ValueError as error:
        raise EvidenceError("CI input/output escapes its declared root: " + str(path)) from error
    return result


def api_child(reply: Path, name: str) -> Path:
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+\.json", name):
        raise EvidenceError("Invalid CMake File API reply reference")
    return inside(reply / name, reply)


def cmake_application(build: Path, source: Path, configuration: str = "Release") -> dict:
    build, source = build.resolve(), source.resolve()
    reply = build / ".cmake/api/v1/reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise EvidenceError("The actual build has no CMake File API reply")
    index_path = indexes[-1]
    index = read_json(index_path)
    model_ref = index.get("reply", {}).get(CLIENT, {}).get("codemodel-v2")
    if not isinstance(model_ref, dict) or model_ref.get("kind") != "codemodel":
        raise EvidenceError("The CI codemodel query has no successful response")
    model_path = api_child(reply, model_ref.get("jsonFile"))
    model = read_json(model_path)
    if (model.get("kind") != "codemodel" or model.get("version", {}).get("major") != 2
            or Path(model.get("paths", {}).get("source", "")).resolve() != source
            or Path(model.get("paths", {}).get("build", "")).resolve() != build):
        raise EvidenceError("CMake File API identifies a different source/build tree")
    configurations = [item for item in model.get("configurations", []) if item.get("name") == configuration]
    if len(configurations) != 1:
        raise EvidenceError("The requested actual configuration must be unique")
    wanted = source / "native/app/main.cpp"
    matches = []
    for target_ref in configurations[0].get("targets", []):
        target_path = api_child(reply, target_ref.get("jsonFile"))
        target = read_json(target_path)
        if target.get("type") != "EXECUTABLE":
            continue
        # Entry fixtures compile real main.cpp but are never installed.
        if not target.get("install", {}).get("destinations"):
            continue
        inputs = [Path(item["path"]) for item in target.get("sources", [])]
        if not any((path if path.is_absolute() else source / path).resolve() == wanted for path in inputs):
            continue
        name = target.get("nameOnDisk")
        if not isinstance(name, str) or name in ("", ".", "..") or Path(name).name != name or "/" in name or "\\" in name:
            raise EvidenceError("Application artifact has no safe actual nameOnDisk")
        artifacts = []
        for item in target.get("artifacts", []):
            path = Path(item["path"])
            actual = inside(path if path.is_absolute() else build / path, build)
            if actual.name == name:
                artifacts.append(actual)
        if len(artifacts) != 1:
            raise EvidenceError("The actual application must have one primary artifact")
        matches.append({"name": target["name"], "nameOnDisk": name, "artifact": str(artifacts[0]),
                        "targetReply": file_ref(target_path), "indexReply": file_ref(index_path),
                        "codemodelReply": file_ref(model_path)})
    if len(matches) != 1:
        raise EvidenceError("Exactly one installed production executable must compile native/app/main.cpp")
    return matches[0]


def hook_free_graph(build: Path, source: Path) -> dict:
    build, source = build.resolve(), source.resolve()
    cache_path, commands_path = build / "CMakeCache.txt", build / "compile_commands.json"
    references = {"cache": file_ref(cache_path), "compileCommands": file_ref(commands_path)}
    if references["cache"]["bytes"] > MAX_JSON_BYTES:
        raise EvidenceError("The actual production cache exceeds its size bound")
    cache = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        declaration, separator, value = line.partition("=")
        name, typed, _ = declaration.partition(":")
        if not separator or not typed or not name or name in cache:
            raise EvidenceError("The actual production cache is malformed or ambiguous")
        cache[name] = value
    expected = {"CMAKE_BUILD_TYPE": "Release", "NICKEL_BUILD_UI": "ON",
                "NICKEL_BUILD_TESTS": "OFF", "NICKEL_TEST_TRANSPORT_FAULTS": "OFF",
                "NICKEL_PRODUCTION_SMOKE_SAME_GRAPH": "OFF"}
    if any(cache.get(name) != value for name, value in expected.items()):
        raise EvidenceError("Candidate deployment requires an actual tests-OFF/faults-OFF Release graph")
    if (not cache.get("CMAKE_HOME_DIRECTORY") or not cache.get("CMAKE_CACHEFILE_DIR")
            or Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != source
            or Path(cache["CMAKE_CACHEFILE_DIR"]).resolve() != build
            or cache.get("NICKEL_SHARED_NATIVE_ROOT", "")):
        raise EvidenceError("The hook-free production graph must use this exact source/build tree")
    commands = read_json(commands_path)
    if not isinstance(commands, list) or not commands or len(commands) > 100000:
        raise EvidenceError("The actual hook-free compile graph must be nonempty and bounded")
    compiled = set()
    for row in commands:
        if not isinstance(row, dict) or not isinstance(row.get("file"), str) or not row["file"]:
            raise EvidenceError("Every actual compilation must identify a source file")
        directory = row.get("directory")
        if not isinstance(directory, str) or Path(directory).resolve() != build:
            raise EvidenceError("An actual compilation identifies a different build tree")
        arguments, command = row.get("arguments"), row.get("command")
        if arguments is not None:
            if (command is not None or not isinstance(arguments, list) or not arguments
                    or any(not isinstance(value, str) for value in arguments)):
                raise EvidenceError("The actual compilation arguments are malformed or ambiguous")
            command = " ".join(arguments)
        elif not isinstance(command, str) or not command:
            raise EvidenceError("Every actual compilation must identify its command")
        if HOOK_DEFINITION.search(command):
            raise EvidenceError("A test hook definition is present in the actual candidate compile graph")
        path = Path(row["file"])
        compiled.add((path if path.is_absolute() else build / path).resolve())
    required = {source / "native/app" / name for name in (
        "main.cpp", "application_state.cpp", "service/trusted_launcher.cpp", "service/launcher_export_services.cpp")}
    if not required.issubset(compiled):
        raise EvidenceError("The hook-free graph omits a required production factory or application translation unit")
    application = cmake_application(build, source)
    if (file_ref(cache_path) != references["cache"]
            or file_ref(commands_path) != references["compileCommands"]):
        raise EvidenceError("The actual hook-free compilation graph changed during verification")
    return {"source": str(source), "build": str(build), **references,
            "compiledFileCount": len(compiled), "application": application,
            "testsEnabled": False, "transportFaultsEnabled": False}



def installed_application(build: Path, source: Path, prefix: Path) -> tuple[Path, dict]:
    prefix = prefix.resolve()
    application = cmake_application(build, source)
    manifest = build / "install_manifest.txt"
    rows = manifest.read_text(encoding="utf-8").splitlines()
    matches = set()
    for row in rows:
        if not row or Path(row).name != application["nameOnDisk"]:
            continue
        path = inside(Path(row), prefix)
        if not path.is_file():
            raise EvidenceError("The actual install manifest names a missing application")
        matches.add(path)
    if len(matches) != 1:
        raise EvidenceError("The actual installed application must be unique inside its candidate prefix")
    binary = matches.pop()
    built_ref, installed_ref = file_ref(Path(application["artifact"])), file_ref(binary)
    if (built_ref["sha256"], built_ref["bytes"]) != (installed_ref["sha256"], installed_ref["bytes"]):
        raise EvidenceError("The installed candidate is not the actual built application")
    application.update(installManifest=file_ref(manifest), builtBinary=built_ref, installedBinary=installed_ref)
    return binary, application


def test_names(document: dict) -> list[str]:
    if document.get("kind") != "ctestInfo" or document.get("version", {}).get("major") != 1:
        raise EvidenceError("Expected an actual CTest JSON inventory")
    tests = document.get("tests")
    if not isinstance(tests, list) or not tests or len(tests) > 10000:
        raise EvidenceError("The complete CTest inventory cannot be empty or unbounded")
    names = []
    for test in tests:
        name = test.get("name")
        command = test.get("command")
        if not isinstance(name, str) or not name or not isinstance(command, list) or not command:
            raise EvidenceError("Every configured CTest must resolve to an actual command")
        if test.get("disabled") is True or any(
                item.get("name") == "DISABLED" and item.get("value") is True
                for item in test.get("properties", [])):
            raise EvidenceError("The complete CI inventory must not disable configured tests")
        names.append(name)
    if len(names) != len(set(names)):
        raise EvidenceError("CTest names must be unique")
    return sorted(names)


def verify_ctest(inventory: dict, junit: Path) -> list[str]:
    expected = test_names(inventory)
    if junit.stat().st_size > MAX_JSON_BYTES:
        raise EvidenceError("CTest JUnit exceeds its size bound")
    report = ET.parse(junit).getroot()
    cases = [report] if report.tag == "testcase" else list(report.iter("testcase"))
    actual = []
    for case in cases:
        name = case.get("name")
        if not name or case.get("status") not in (None, "run", "passed"):
            raise EvidenceError("CTest contains an unexecuted or failed testcase")
        if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")):
            raise EvidenceError("CTest failure/error/skip cannot pass the complete candidate gate")
        actual.append(name)
    if len(actual) != len(set(actual)) or sorted(actual) != expected:
        raise EvidenceError("Executed CTest JUnit differs from the complete configured inventory")
    return expected


def command_argv(record: dict, program: str) -> list[str]:
    argv = record.get("argv")
    if not isinstance(argv, list) or not argv or any(not isinstance(value, str) for value in argv):
        raise EvidenceError("CI evidence has no actual command arguments")
    if Path(argv[0]).stem.lower() == program:
        return argv
    if (program == "ctest" and len(argv) >= 4 and Path(argv[0]).name == "xvfb-run"
            and argv[1] == "-a" and Path(argv[2]).stem.lower() == program):
        return argv[2:]
    raise EvidenceError("CI evidence does not identify the required actual command")


def checked_command(path: Path, program: str, required: tuple[str, ...] = ()) -> dict:
    record = read_json(path)
    if record.get("complete") is not True or record.get("returnCode") != 0:
        raise EvidenceError("CI command must have completed successfully: " + str(path))
    if record.get("buildInputInventory") is not None and record.get("buildInputGate") != "passed":
        raise EvidenceError("The actual command did not preserve its frozen build inputs")
    argv = command_argv(record, program)
    if any(argument not in argv for argument in required):
        raise EvidenceError("CI evidence does not identify the required actual command")
    log = Path(record["log"])
    if file_ref(log)["sha256"] != record.get("logSha256"):
        raise EvidenceError("The CI raw log no longer matches its command evidence")
    return record


def prepare(build: Path, output: Path):
    if os.path.lexists(build):
        raise EvidenceError("CI must start with a fresh build tree, not a stale cache")
    query = build / ".cmake/api/v1/query" / CLIENT / "codemodel-v2"
    query.parent.mkdir(parents=True)
    query.touch(exist_ok=False)
    write_json(output, {"schemaVersion": 1, "kind": "native-ci-fresh-build-preparation",
                        "sourceRef": os.environ.get("PD_SOURCE_REF", ""),
                        "createdUtc": dt.datetime.now(dt.UTC).isoformat(),
                        "query": str(query.resolve()), "releaseAccepted": False})


def inventory_command(options):
    options.output.parent.mkdir(parents=True, exist_ok=True)
    if os.path.lexists(options.output):
        raise EvidenceError("CTest raw inventory path is already occupied")
    from msvc import DEFAULT_VS, developer_environment
    import shutil
    environment = developer_environment(Path(os.environ.get("NICKEL_VS_ROOT", str(DEFAULT_VS))))
    ctest = shutil.which(options.ctest, path=environment.get("PATH"))
    if ctest is None:
        raise EvidenceError("CTest is unavailable in the actual CI toolchain")
    argv = [ctest, "--test-dir", str(options.build.resolve()), "-C", "Release",
            "--show-only=json-v1", "--no-tests=error"]
    result = subprocess.run(argv, env=environment, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=120)
    with options.output.open("xb") as handle:
        handle.write(result.stdout)
    if result.returncode:
        sys.stderr.buffer.write(result.stderr)
        raise EvidenceError("CTest inventory command failed")
    names = test_names(read_json(options.output))
    write_json(options.binding, {"schemaVersion": 1, "kind": "native-ci-configured-ctest-inventory",
                                "sourceRef": os.environ.get("PD_SOURCE_REF", ""),
                                "argv": argv, "build": str(options.build.resolve()),
                                "inventory": file_ref(options.output), "testCount": len(names),
                                "testNames": names, "executed": False})
    print(json.dumps({"testCount": len(names), "executed": False}))


def ctest_command(options):
    binding = read_json(options.binding)
    inventory_ref = file_ref(options.inventory)
    if (binding.get("kind") != "native-ci-configured-ctest-inventory"
            or binding.get("inventory") != inventory_ref
            or Path(binding.get("build", "")).resolve() != options.build.resolve()
            or binding.get("sourceRef") != os.environ.get("PD_SOURCE_REF", "")):
        raise EvidenceError("CTest inventory binding does not match this CI generation")
    command = checked_command(options.evidence, "ctest", ("--no-tests=error", "--output-junit"))
    argv = command_argv(command, "ctest")
    cwd = Path(command["cwd"]).resolve()
    for flag, path in (("--test-dir", options.build), ("--output-junit", options.junit)):
        if argv.count(flag) != 1 or argv.index(flag) + 1 >= len(argv):
            raise EvidenceError("CTest must name the exact build tree and JUnit output")
        value = Path(argv[argv.index(flag) + 1])
        if (value if value.is_absolute() else cwd / value).resolve() != path.resolve():
            raise EvidenceError("CTest command identifies another build tree or JUnit output")
    for flag, expected in (("-C", "Release"), ("--parallel", "1")):
        if argv.count(flag) != 1 or argv.index(flag) + 1 >= len(argv) or argv[argv.index(flag) + 1] != expected:
            raise EvidenceError("CTest must execute the serial Release configuration")
    forbidden = {"-R", "-E", "-I", "-L", "-LE", "--tests-regex", "--exclude-regex", "--tests-information",
                 "--label-regex", "--label-exclude", "--rerun-failed", "--tests-from-file", "--exclude-from-file",
                 "--show-only", "-N"}
    if any(argument in forbidden or argument.startswith(tuple(flag + "=" for flag in forbidden))
           or any(argument.startswith(flag) and argument != flag for flag in ("-R", "-E", "-I", "-L", "-N"))
           for argument in argv):
        raise EvidenceError("A filtered or show-only CTest run is not the full candidate inventory")
    names = verify_ctest(read_json(options.inventory), options.junit)
    if names != binding.get("testNames") or binding.get("testCount", len(names)) != len(names):
        raise EvidenceError("Configured CTest names changed before execution")
    write_json(options.output, {"schemaVersion": 1, "kind": "native-ci-complete-ctest-execution",
                               "sourceRef": binding["sourceRef"], "testCount": len(names), "testNames": names,
                               "inventory": inventory_ref, "inventoryBinding": file_ref(options.binding),
                               "commandEvidence": file_ref(options.evidence), "junit": file_ref(options.junit),
                               "releaseAccepted": False})
    print(json.dumps({"testCount": len(names), "fullInventoryPassed": True, "releaseAccepted": False}))


def verify_release_command(options):
    from msvc import input_inventory

    source, build = options.source.resolve(), options.build.resolve()
    inventory_path = inside(options.input_inventory, source)
    inventory = input_inventory(inventory_path)
    source_document = read_json(inventory_path)
    source_ref = os.environ.get("PD_SOURCE_REF", "")
    if (not re.fullmatch(r"[0-9a-f]{40}", source_ref)
            or inventory.get("sourceCommit") != source_ref
            or not isinstance(source_document, dict) or not source_document.get("sourceRoot")
            or Path(source_document["sourceRoot"]).resolve() != source):
        raise EvidenceError("The hook-free build must bind the actual fixed CI source inventory")
    receipts = {}
    for role, path, flag in (("configure", options.configure_evidence, "-B"),
                             ("build", options.build_evidence, "--build")):
        record = checked_command(path, "cmake", (flag,))
        argv = command_argv(record, "cmake")
        cwd = Path(record["cwd"]).resolve()
        if (cwd != source or record.get("buildInputInventory") != inventory
                or record.get("buildInputGate") != "passed"
                or argv.count(flag) != 1 or argv.index(flag) + 1 >= len(argv)):
            raise EvidenceError("The actual hook-free command is not bound to this source/build generation")
        command_build = Path(argv[argv.index(flag) + 1])
        if (command_build if command_build.is_absolute() else cwd / command_build).resolve() != build:
            raise EvidenceError("The actual command identifies another production build tree")
        if role == "configure" and any(value in argv for value in ("--build", "--install", "-P")):
            raise EvidenceError("The configure receipt is not an actual production configuration")
        if role == "build":
            for option, value in (("--config", "Release"), ("--parallel", "1")):
                if argv.count(option) != 1 or argv.index(option) + 1 >= len(argv) or argv[argv.index(option) + 1] != value:
                    raise EvidenceError("The production build must execute the complete serial Release graph")
            if any(value == "--target" or value.startswith("--target=") for value in argv):
                raise EvidenceError("A target-filtered build is not the complete production candidate graph")
        receipts[role] = file_ref(path)
    graph = hook_free_graph(build, source)
    graph["application"]["builtBinary"] = file_ref(Path(graph["application"]["artifact"]))
    if input_inventory(inventory_path) != inventory:
        raise EvidenceError("The fixed source inventory changed during production graph verification")
    write_json(options.output, {"schemaVersion": 1, "kind": "native-ci-hook-free-production-build",
                               "sourceRef": source_ref, "sourceInventory": inventory,
                               "commandEvidence": receipts, "graph": graph, "releaseAccepted": False})
    print(json.dumps({"hookFreeGraphVerified": True, "releaseAccepted": False}))


def audit_command(options):
    graph = hook_free_graph(options.build, options.source)
    binary, application = installed_application(options.build, options.source, options.prefix)
    write_json(options.discovery, {"schemaVersion": 1, "kind": "native-ci-installed-application-discovery",
                                  "sourceRef": os.environ.get("PD_SOURCE_REF", ""),
                                  "application": application, "productionGraph": graph, "releaseAccepted": False})
    argv = [sys.executable, "-B", str(ROOT / "scripts/native/audit_binary.py"), str(binary),
            "--output", str(options.output)]
    return subprocess.run(argv).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("--build", type=Path, required=True)
    prepare_parser.add_argument("--output", type=Path, required=True)
    inventory_parser = commands.add_parser("inventory")
    inventory_parser.add_argument("--build", type=Path, required=True)
    inventory_parser.add_argument("--output", type=Path, required=True)
    inventory_parser.add_argument("--binding", type=Path, required=True)
    inventory_parser.add_argument("--ctest", default="ctest")
    verify_parser = commands.add_parser("verify-ctest")
    verify_parser.add_argument("--build", type=Path, required=True)
    verify_parser.add_argument("--inventory", type=Path, required=True)
    verify_parser.add_argument("--binding", type=Path, required=True)
    verify_parser.add_argument("--junit", type=Path, required=True)
    verify_parser.add_argument("--evidence", type=Path, required=True)
    verify_parser.add_argument("--output", type=Path, required=True)
    release_parser = commands.add_parser("verify-release")
    release_parser.add_argument("--build", type=Path, required=True)
    release_parser.add_argument("--source", type=Path, default=ROOT)
    release_parser.add_argument("--input-inventory", type=Path, required=True)
    release_parser.add_argument("--configure-evidence", type=Path, required=True)
    release_parser.add_argument("--build-evidence", type=Path, required=True)
    release_parser.add_argument("--output", type=Path, required=True)
    audit_parser = commands.add_parser("audit-installed")
    audit_parser.add_argument("--build", type=Path, required=True)
    audit_parser.add_argument("--source", type=Path, default=ROOT)
    audit_parser.add_argument("--prefix", type=Path, required=True)
    audit_parser.add_argument("--discovery", type=Path, required=True)
    audit_parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    try:
        if options.command == "prepare":
            prepare(options.build, options.output)
        elif options.command == "inventory":
            inventory_command(options)
        elif options.command == "verify-ctest":
            ctest_command(options)
        elif options.command == "verify-release":
            verify_release_command(options)
        else:
            return audit_command(options)
    except (EvidenceError, OSError, ValueError, KeyError, TypeError, ET.ParseError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
