"""Bind current Pd production and owned acceptance inputs without copying libraries."""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import stat
import sys
from pathlib import Path, PurePosixPath, PureWindowsPath

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evidence
import generate_fixtures

TARGET = "nickel_production_smoke_existing_matrix"
COMMIT = re.compile(r"[0-9a-f]{40}\Z")


def require_commit(value) -> str:
    if not isinstance(value, str) or not COMMIT.fullmatch(value):
        raise evidence.EvidenceError("the production source commit must be full lowercase Git SHA1")
    return value


def artifact_platform(value: str | None = None) -> str:
    if value is None:
        value = "windows" if sys.platform == "win32" else "linux" if sys.platform.startswith("linux") else None
    if not isinstance(value, str) or value not in {"windows", "linux"}:
        raise evidence.EvidenceError("same-graph artifacts require an explicit supported target platform")
    return value


def artifact_path(row: dict, target_platform: str) -> Path:
    platform = artifact_platform(target_platform)
    role = row.get("role")
    physical = row.get("physicalPath")
    if (not isinstance(role, str) or role not in {"production-library", "production-executable", "smoke-executable"}
            or not isinstance(physical, str) or not physical or any(ord(ch) < 32 for ch in physical)):
        raise evidence.EvidenceError("production artifact role/path is invalid")
    path = Path(physical)
    if not path.is_absolute():
        raise evidence.EvidenceError("production artifact path is not absolute")
    if role == "production-library":
        suffix = ".lib" if platform == "windows" else ".a"
        if path.suffix != suffix and not (platform == "windows" and path.suffix.lower() == suffix):
            raise evidence.EvidenceError("production library extension differs from its target platform")
    elif platform == "windows":
        if path.suffix.lower() != ".exe":
            raise evidence.EvidenceError("Windows executable is not the published EXE")
    elif path.suffix.lower() in {".exe", ".dll", ".lib", ".a", ".so"}:
        raise evidence.EvidenceError("Linux executable role contains a foreign binary or library suffix")
    return path


def linux_artifact_format(prefix: bytes, role: str, size: int, stream) -> str:
    if role == "production-library":
        if not prefix.startswith(b"!<arch>\n"):
            raise evidence.EvidenceError("Linux static library is not a regular ar archive; thin archives need separate member closure")
        offset, has_member = 8, False
        while offset < size:
            stream.seek(offset)
            header = stream.read(60)
            name, length = header[:16].rstrip(), header[48:58].strip()
            if (len(header) != 60 or header[58:60] != b"`\n" or not name or not length.isdigit()
                    or any(ch < 32 for ch in name)):
                raise evidence.EvidenceError("Linux ar member header is malformed or truncated")
            member_size = int(length)
            start = offset + 60
            if member_size > size - start:
                raise evidence.EvidenceError("Linux ar member extends outside its published bytes")
            if name.startswith(b"#1/"):
                if not name[3:].isdigit() or not 0 < int(name[3:]) <= member_size:
                    raise evidence.EvidenceError("Linux BSD ar member name is not bounded")
            if name not in {b"/", b"//", b"/SYM64/"} and not name.startswith(b"__.SYMDEF"):
                has_member = has_member or member_size > 0
            offset = start + member_size
            if member_size % 2:
                stream.seek(offset)
                if stream.read(1) != b"\n":
                    raise evidence.EvidenceError("Linux ar odd member padding is missing")
                offset += 1
        if offset != size or not has_member:
            raise evidence.EvidenceError("Linux ar archive has no bounded library member")
        return "ar-static-library"
    if len(prefix) < 52 or prefix[:4] != b"\x7fELF" or prefix[4] not in (1, 2) or prefix[5] not in (1, 2) or prefix[6] != 1:
        raise evidence.EvidenceError("Linux executable is not a bounded ELF header")
    elf64 = prefix[4] == 2
    minimum, header_offset, program_size = (64, 52, 56) if elf64 else (52, 40, 32)
    if len(prefix) < minimum:
        raise evidence.EvidenceError("Linux ELF header is truncated")
    byteorder = "little" if prefix[5] == 1 else "big"
    number = lambda start, length: int.from_bytes(prefix[start:start + length], byteorder)
    kind, machine, version = number(16, 2), number(18, 2), number(20, 4)
    header_size = number(header_offset, 2)
    entry = number(24, 8 if elf64 else 4)
    program_offset = number(32 if elf64 else 28, 8 if elf64 else 4)
    program_entry_size, program_count = number(header_offset + 2, 2), number(header_offset + 4, 2)
    if (kind not in (2, 3) or machine == 0 or version != 1 or header_size != minimum or not entry
            or program_entry_size != program_size or not 0 < program_count < 0xffff
            or program_offset < minimum or program_offset + program_count * program_size > size):
        raise evidence.EvidenceError("Linux ELF lacks a bounded executable header/program table")
    executable_entry = False
    for index in range(program_count):
        stream.seek(program_offset + index * program_size)
        program = stream.read(program_size)
        if len(program) != program_size:
            raise evidence.EvidenceError("Linux ELF program header is truncated")
        field = lambda start, length: int.from_bytes(program[start:start + length], byteorder)
        if field(0, 4) != 1:
            continue
        flags = field(4 if elf64 else 24, 4)
        file_offset = field(8 if elf64 else 4, 8 if elf64 else 4)
        address = field(16 if elf64 else 8, 8 if elf64 else 4)
        file_size = field(32 if elf64 else 16, 8 if elf64 else 4)
        memory_size = field(40 if elf64 else 20, 8 if elf64 else 4)
        if file_size > memory_size or file_offset + file_size > size:
            raise evidence.EvidenceError("Linux ELF load segment extends outside its published bytes")
        executable_entry = executable_entry or bool(flags & 1 and address <= entry < address + file_size)
    if not executable_entry:
        raise evidence.EvidenceError("Linux ELF is a shared/object file or has no file-backed executable entry")
    return "elf-executable" if kind == 2 else "elf-pie-executable"


def hash_artifact(row: dict, *, target_platform: str = "windows") -> dict:
    platform = artifact_platform(target_platform)
    source = artifact_path(row, platform)
    size = row.get("bytes")
    expected = evidence.require_hash(row.get("sha256"), "artifact hash")
    if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
        raise evidence.EvidenceError("bound artifact size is not a positive integer")
    before = source.stat()
    if not stat.S_ISREG(before.st_mode) or before.st_size != size:
        raise evidence.EvidenceError("bound artifact type/size changed: " + str(source))
    physical = source.resolve(strict=True)
    identity = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, stat.S_IFMT(info.st_mode))
    stable = lambda info: (*identity(info), info.st_ctime_ns, info.st_mode)
    digest = hashlib.sha256()
    prefix = b""
    binary_format = "windows-static-library" if row["role"] == "production-library" else "windows-executable"
    with source.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        # Windows path stat infers EXE mode and exposes a different ctime than handle stat.
        if identity(opened) != identity(before):
            raise evidence.EvidenceError("bound artifact changed before its handle opened")
        while chunk := stream.read(256 * 1024):
            if not prefix:
                prefix = chunk[:64]
            digest.update(chunk)
        if platform == "linux":
            binary_format = linux_artifact_format(prefix, row["role"], size, stream)
            if sys.platform.startswith("linux") and row["role"] != "production-library" and not before.st_mode & 0o111:
                raise evidence.EvidenceError("published Linux ELF is not executable by file mode")
        if stable(os.fstat(stream.fileno())) != stable(opened):
            raise evidence.EvidenceError("bound artifact changed while its handle was open")
    after = source.stat()
    if stable(before) != stable(after) or source.resolve(strict=True) != physical or digest.hexdigest() != expected:
        raise evidence.EvidenceError("bound artifact identity/hash changed: " + str(source))
    return {"logicalPath": str(source), "resolvedPhysicalPath": str(physical), "size": after.st_size,
            "sha256": digest.hexdigest(), "handlesClosed": True, "targetPlatform": platform, "binaryFormat": binary_format}


def artifact_paths_equal(left: str | Path, right: str | Path, target_platform: str) -> bool:
    path_type = PureWindowsPath if artifact_platform(target_platform) == "windows" else PurePosixPath
    return path_type(str(left)) == path_type(str(right))



def load_bound(path: str | Path, pin: str):
    source = evidence.checked_local(path)
    expected = evidence.require_hash(pin, "bound input hash")
    if evidence.sha256(source) != expected:
        raise evidence.EvidenceError("bound input differs from the independently supplied hash")
    return source, evidence.read_json(source)


def source_records(document: dict) -> list[dict]:
    if not isinstance(document, dict):
        raise evidence.EvidenceError("source inventory is not an object")
    rows = []
    seen = set()
    for row in evidence.manifest_records(document):
        relative = evidence.relative_path(row.get("path"))
        key = relative.casefold()
        size = row.get("size", row.get("bytes"))
        if (key in seen or isinstance(size, bool) or not isinstance(size, int) or size < 0
                or ("size" in row and "bytes" in row and row["size"] != row["bytes"])):
            raise evidence.EvidenceError("duplicate, ambiguous or unbounded source record")
        seen.add(key)
        rows.append({"path": relative, "size": size,
                     "sha256": evidence.require_hash(row.get("sha256"), "source record hash")})
    if not rows:
        raise evidence.EvidenceError("source inventory is empty")
    return rows


def validate_graph(source: dict, harness: dict, ledger: dict, complete: dict, *,
                   source_pin: str, source_commit: str, harness_pin: str,
                   ledger_pin: str, compile_pin: str, link_pin: str,
                   target_platform: str = "windows") -> list[dict]:
    platform = artifact_platform(target_platform)
    require_commit(source_commit)
    for pin in (source_pin, harness_pin, ledger_pin, compile_pin, link_pin):
        evidence.require_hash(pin, "generation binding")
    if not all(isinstance(document, dict) for document in (source, harness, ledger, complete)):
        raise evidence.EvidenceError("a same-graph binding is not an object")
    if source.get("sourceCommit") != source_commit:
        raise evidence.EvidenceError("raw production source belongs to a different commit")
    if harness.get("kind") != "owned-production-smoke-source-delta" or harness.get("owner") != "production-smoke":
        raise evidence.EvidenceError("not an owned immutable harness publication")
    identities = {"sourceCommit": source_commit, "sourceGenerationManifestSha256": source_pin,
                  "harnessManifestSha256": harness_pin}
    if ledger.get("kind") != "matched-same-graph-production-artifacts":
        raise evidence.EvidenceError("not a matched same-graph production artifact ledger")
    for document in (ledger, complete):
        if any(document.get(key) != value for key, value in identities.items()) or document.get("targetPlatform", "windows") != platform:
            raise evidence.EvidenceError("production, acceptance and target-platform generations are not all bound")
    if (complete.get("publicationComplete") is not True or complete.get("receiver") != "production-smoke"
            or complete.get("artifactManifestSha256") != ledger_pin
            or complete.get("compileCommandsSha256") != compile_pin or complete.get("linkRecipeSha256") != link_pin):
        raise evidence.EvidenceError("production build publication is not complete")
    artifacts = ledger.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        raise evidence.EvidenceError("no published production artifacts")
    roles = {"production-library": 0, "production-executable": 0, "smoke-executable": 0}
    seen = set()
    for row in artifacts:
        if not isinstance(row, dict):
            raise evidence.EvidenceError("same-graph artifact record is not an object")
        path = artifact_path(row, platform)
        key = str(path).casefold() if platform == "windows" else str(path)
        size = row.get("bytes")
        if key in seen or isinstance(size, bool) or not isinstance(size, int) or size <= 0:
            raise evidence.EvidenceError("duplicate or invalid production artifact")
        evidence.require_hash(row.get("sha256"), "artifact hash")
        roles[row["role"]] += 1
        seen.add(key)
    if roles["production-library"] == 0 or roles["production-executable"] != 1 or roles["smoke-executable"] != 1:
        raise evidence.EvidenceError("production/consumer binary and library closure is incomplete")
    return artifacts


def validate_inventory(inventory: dict, smoke: dict, argv: list[str], *, target_platform: str = "windows") -> None:
    platform = artifact_platform(target_platform)
    evidence.ctest_inventory(inventory, [TARGET])
    tests = [row for row in inventory["tests"] if row["name"] == TARGET]
    if len(tests) != 1 or not isinstance(tests[0].get("command"), list):
        raise evidence.EvidenceError("the actual CTest matrix entry is missing or duplicated")
    actual = tests[0]["command"]
    if (len(actual) != len(argv) or len(argv) != 11
            or not all(isinstance(arg, str) and arg for arg in actual + argv)):
        raise evidence.EvidenceError("actual CTest arguments do not identify this 22-case run")
    path_arguments = {0, 2, 4, 6, 10}
    for index, (observed, expected) in enumerate(zip(actual, argv)):
        matches = artifact_paths_equal(observed, expected, platform) if index in path_arguments else observed == expected
        if not matches:
            raise evidence.EvidenceError("actual CTest command binds a different run")
    if not artifact_paths_equal(actual[0], smoke["physicalPath"], platform):
        raise evidence.EvidenceError("CTest identifies a different executable from the artifact ledger")


def validate_compile_commands(commands, expected_source: Path, *, source_pin: str,
                              source_commit: str, harness_pin: str, target_platform: str = "windows") -> Path:
    platform = artifact_platform(target_platform)
    evidence.require_hash(source_pin, "compile source pin")
    require_commit(source_commit)
    evidence.require_hash(harness_pin, "compile harness pin")
    if not isinstance(commands, list) or not commands:
        raise evidence.EvidenceError("actual compile command inventory is missing")
    defines = {"NICKEL_SMOKE_SAME_GRAPH": "1", "NICKEL_SMOKE_SOURCE_SHA256": source_pin,
               "NICKEL_SMOKE_SOURCE_COMMIT": source_commit, "NICKEL_SMOKE_HARNESS_SHA256": harness_pin}
    directories = []
    for row in commands:
        if not isinstance(row, dict) or not isinstance(row.get("file"), str) or not row["file"]:
            raise evidence.EvidenceError("actual compile command record has no source file")
        if not artifact_paths_equal(row["file"], expected_source, platform):
            continue
        directory = row.get("directory")
        if not isinstance(directory, str) or not directory or not Path(directory).is_absolute():
            raise evidence.EvidenceError("actual acceptance compile command has no absolute graph directory")
        if "command" in row:
            text = row["command"]
        else:
            arguments = row.get("arguments")
            if not isinstance(arguments, list) or not all(isinstance(arg, str) for arg in arguments):
                raise evidence.EvidenceError("actual acceptance compile arguments are malformed")
            text = " ".join(arguments)
        if not isinstance(text, str) or not text:
            raise evidence.EvidenceError("actual acceptance compile command is empty")
        normalized = text.replace('\\"', '"')
        for key, pin in defines.items():
            pattern = r'(?:^|\s)["\']?[/-]D\s*["\']?' + key + r'=([^\s]+)'
            values = [match.group(1).strip('"\'') for match in re.finditer(pattern, normalized)]
            if values != [pin]:
                raise evidence.EvidenceError("actual compile command lacks a unique named generation definition " + key)
        directories.append(Path(directory))
    if len(directories) != 1:
        raise evidence.EvidenceError("actual compile graph omits or ambiguously locates the acceptance consumer")
    return directories[0]


def validate_link_recipe(path: Path, artifacts: list[dict], graph_directory: Path, *,
                         target_platform: str = "windows") -> None:
    platform = artifact_platform(target_platform)
    if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
        raise evidence.EvidenceError("actual link recipe is missing or exceeds the evidence quota")
    try:
        text = path.read_text(encoding="utf-8-sig")
    except UnicodeError as error:
        raise evidence.EvidenceError("actual link recipe is not UTF-8 text") from error
    if TARGET not in text:
        raise evidence.EvidenceError("actual link recipe omits the acceptance target")

    def normalized(value: str) -> str:
        result = re.sub(r"/+", "/", value.replace("$:", ":").replace("$ ", " ").replace("\\", "/"))
        return result.casefold() if platform == "windows" else result

    recipe = normalized(text)
    roots = {graph_directory, graph_directory.resolve(strict=True)}
    for row in artifacts:
        artifact = artifact_path(row, platform)
        locations = {artifact, artifact.resolve(strict=True)}
        candidates = {normalized(str(location)) for location in locations}
        for root in roots:
            for location in locations:
                try:
                    candidates.add(normalized(os.path.relpath(location, root)))
                except ValueError:
                    continue
        if not any(re.search(r"(?<![a-zA-Z0-9_./-])" + re.escape(candidate) + r"(?![a-zA-Z0-9_./-])", recipe)
                   for candidate in candidates):
            raise evidence.EvidenceError("published artifact path is absent from the actual graph link recipe: " + row["physicalPath"])


def prepare(args) -> dict:
    platform = artifact_platform(getattr(args, "target_platform", None))
    source_path, source = load_bound(args.source_manifest, args.source_sha256)
    harness_path, harness = load_bound(args.harness_manifest, args.harness_sha256)
    artifact_manifest_path, ledger = load_bound(args.artifact_manifest, args.artifact_sha256)
    complete_path, complete = load_bound(args.complete_marker, args.complete_sha256)
    compile_path, commands = load_bound(args.compile_commands, args.compile_sha256)
    link_path = evidence.checked_local(args.link_recipe)
    if evidence.sha256(link_path) != evidence.require_hash(args.link_sha256, "link recipe hash"):
        raise evidence.EvidenceError("raw same-graph link recipe changed")
    inventory_path, inventory = load_bound(args.ctest_inventory, args.ctest_sha256)
    artifacts = validate_graph(source, harness, ledger, complete,
        source_pin=args.source_sha256, source_commit=args.source_commit, harness_pin=args.harness_sha256,
        ledger_pin=args.artifact_sha256, compile_pin=args.compile_sha256, link_pin=args.link_sha256, target_platform=platform)
    rows = source_records(source)
    harness_root = evidence.checked_local(args.harness_root)
    if not harness_root.is_dir():
        raise evidence.EvidenceError("compiled immutable harness root is missing")
    required = {"tests/native-production-smoke/" + name for name in (
        "production_main.cpp", "suite_cases.hpp", "qt_probe.hpp", "same_graph_inputs.hpp", "fixture_inputs.hpp",
        "generate_fixtures.py", "CMakeLists.txt", "matrix.json")}
    present = set()
    compiled_files = []
    for row in source_records(harness):
        if not row["path"].startswith("tests/native-production-smoke/"):
            continue
        path = evidence.checked_local(harness_root / row["path"])
        if path.stat().st_size != row["size"] or evidence.sha256(path) != row["sha256"]:
            raise evidence.EvidenceError("compiled acceptance source differs from its raw immutable freeze")
        present.add(row["path"])
        compiled_files.append((path, row["size"], row["sha256"]))
    if not required.issubset(present):
        raise evidence.EvidenceError("harness freeze omits a compiled matrix or fixture generation input")
    graph_directory = validate_compile_commands(commands, harness_root / "tests/native-production-smoke/production_main.cpp",
        source_pin=args.source_sha256, source_commit=args.source_commit, harness_pin=args.harness_sha256, target_platform=platform)
    validate_link_recipe(link_path, artifacts, graph_directory, target_platform=platform)
    output = evidence.checked_local(args.output, writable=True)
    case_output = evidence.checked_local(args.case_output, writable=True)
    run_envelope = evidence.checked_local(case_output.parent / (case_output.name + ".fixtures"), writable=True)
    run_root = generate_fixtures.fixture_run_root(case_output)
    if output.exists() or case_output.exists() or run_envelope.exists() or output == case_output:
        raise evidence.EvidenceError("prepared input, case output and runtime fixture namespace must be distinct fresh unused paths")
    fixtures = evidence.checked_local(args.fixtures)
    fixture_pin = evidence.sha256(fixtures)
    fixture_verified = generate_fixtures.verify_template(fixtures, fixture_pin, source_commit=args.source_commit,
        source_sha256=args.source_sha256, harness_sha256=args.harness_sha256)
    fixture_document = fixture_verified["document"]
    fixture_root = evidence.checked_local(fixture_document["root"])
    if (evidence._inside(run_root, fixture_root) or evidence._inside(fixture_root, run_root)
            or evidence._inside(output, fixture_root) or evidence._inside(case_output, fixture_root)
            or evidence._inside(output, run_envelope)):
        raise evidence.EvidenceError("runtime outputs collide with their immutable fixture template")
    generator = harness_root / "tests/native-production-smoke/generate_fixtures.py"
    if evidence.sha256(generator) != fixture_document["generatorSha256"]:
        raise evidence.EvidenceError("fixture template was generated by another harness source generation")
    matrix = harness_root / "tests/native-production-smoke/matrix.json"
    matrix_pin = evidence.sha256(matrix)
    if len(evidence.load_matrix(matrix)["cases"]) != 22:
        raise evidence.EvidenceError("the existing matrix case count changed")
    smoke = next(row for row in artifacts if row["role"] == "smoke-executable")
    argv = [smoke["physicalPath"], "--freeze", str(output), "--fixtures", str(fixtures),
            "--matrix", str(matrix), "--manifest-sha256", args.source_sha256, "--output", str(case_output)]
    validate_inventory(inventory, smoke, argv, target_platform=platform)
    disk = evidence.capacity(output, fixture_verified["totalBytes"] + 2 * 1024 * 1024, 256 * 1024 * 1024)
    if sys.platform == "win32" and shutil.disk_usage("C:/").free < 1024 * 1024 * 1024:
        raise evidence.EvidenceError("C is below the shared 1 GiB execution reserve")
    bound_inputs = {"sourceManifest": (source_path, args.source_sha256),
                    "harnessManifest": (harness_path, args.harness_sha256),
                    "artifactManifest": (artifact_manifest_path, args.artifact_sha256),
                    "completeMarker": (complete_path, args.complete_sha256),
                    "compileCommands": (compile_path, args.compile_sha256),
                    "linkRecipe": (link_path, args.link_sha256),
                    "actualCTestInventory": (inventory_path, args.ctest_sha256),
                    "fixtureManifest": (fixtures, fixture_pin), "matrix": (matrix, matrix_pin)}
    references = [{**hash_artifact(row, target_platform=platform), "role": row["role"]} for row in artifacts]
    result = {"schema": 1, "kind": "verified-same-graph-production-inputs", "verifiedAt": evidence.utc_now(),
        "sourceCommit": args.source_commit, "sourceGenerationManifestSha256": args.source_sha256,
        "targetPlatform": platform, "files": rows, "fileCount": len(rows), "artifactReferences": references,
        "capacity": disk, "copiedLibraryBytes": 0, "actualRunArgv": argv, "fixtureRole": "immutable-template",
        "fixtureRunRoot": str(run_root), "fixtureTemplateFileCount": fixture_verified["fileCount"],
        "fixtureGeneratorSha256": fixture_document["generatorSha256"],
        "compileGraphDirectory": str(graph_directory), "productionPass": False, "runtimeStatus": "WrittenNotRun"}
    hash_fields = {"sourceManifest": "sourceGenerationManifestSha256", "harnessManifest": "harnessManifestSha256",
                   "matrix": "matrixSha256"}
    for key, (path, pin) in bound_inputs.items():
        if evidence.sha256(path) != pin:
            raise evidence.EvidenceError("same-graph input changed during verification: " + key)
        result[key] = str(path)
        result[hash_fields.get(key, key + "Sha256")] = pin
    for path, size, pin in compiled_files:
        if path.stat().st_size != size or evidence.sha256(path) != pin:
            raise evidence.EvidenceError("compiled harness input changed during verification")
    generate_fixtures.verify_template(fixtures, fixture_pin, source_commit=args.source_commit,
        source_sha256=args.source_sha256, harness_sha256=args.harness_sha256)
    if output.exists() or case_output.exists() or run_envelope.exists():
        raise evidence.EvidenceError("runtime outputs were consumed during preparation")
    evidence.write_json_new(output, result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("source-manifest", "source-sha256", "source-commit", "harness-manifest", "harness-sha256",
                  "harness-root", "artifact-manifest", "artifact-sha256", "complete-marker", "complete-sha256",
                  "compile-commands", "compile-sha256", "link-recipe", "link-sha256", "ctest-inventory",
                  "ctest-sha256", "fixtures", "case-output", "output"):
        parser.add_argument("--" + field, required=True)
    parser.add_argument("--target-platform", choices=("windows", "linux"), help="Defaults to the native supported host; Linux requires bound ELF/ar artifacts")
    args = parser.parse_args()
    try:
        result = prepare(args)
        print("prepared current Pd", result["targetPlatform"], result["fileCount"], "source records; actual run still required")
        return 0
    except (OSError, KeyError, TypeError, ValueError, evidence.EvidenceError) as error:
        print("BLOCKED:", error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
