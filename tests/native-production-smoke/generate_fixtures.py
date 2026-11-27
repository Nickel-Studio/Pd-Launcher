"""Generate owner-local synthetic legacy data, not a replacement production service."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import uuid
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evidence

SYNTHETIC_SECRETS = ["SMOKE_ACCESS_0123456789_PUBLIC_TEST", "SMOKE_REFRESH_0123456789_PUBLIC_TEST",
                     "SMOKE_CLIENT_0123456789_PUBLIC_TEST", "SMOKE_PASSWORD_0123456789_PUBLIC_TEST"]
NAMED = "Named-Synthetic"
MC_VERSION = "1.20.1"


def json_bytes(value) -> bytes:
    return (json.dumps(value, ensure_ascii=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def archive_bytes(entries: dict[str, bytes]) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, payload in sorted(entries.items()):
            evidence.relative_path(name)
            entry = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, payload)
    return output.getvalue()


def skin_png() -> bytes:
    width, height = 64, 64
    raw = b"".join(b"\0" + bytes((32 + row, 120, 180, 255)) * width for row in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


TEMPLATE_ROLE = "immutable-template"
RUN_ROLE = "runtime-instance"
RUN_DEPTH = "deep-run-path-preserved-0123456789"
MAX_FIXTURE_BYTES = 32 * 1024 * 1024


def generation_binding(source_commit=None, source_sha256=None, harness_sha256=None) -> dict | None:
    values = (source_commit, source_sha256, harness_sha256)
    if all(value is None for value in values):
        return None
    if not isinstance(source_commit, str) or not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise evidence.EvidenceError("fixture template requires the full immutable production commit")
    return {"sourceCommit": source_commit,
            "sourceGenerationManifestSha256": evidence.require_hash(source_sha256, "fixture source manifest pin"),
            "harnessManifestSha256": evidence.require_hash(harness_sha256, "fixture compiled harness pin")}


def fixture_run_root(case_output: str | Path) -> Path:
    output = evidence.checked_local(case_output, writable=True)
    root = output.parent / (output.name + ".fixtures")
    while len(str(root).encode("utf-8")) < 310:
        root /= RUN_DEPTH
    return evidence.checked_local(root, writable=True)


def relocate_json(value, replacements: dict[str, str], template_root: str, *, depth: int = 0):
    if depth > 32:
        raise evidence.EvidenceError("fixture JSON relocation exceeded its depth bound")

    def relocate(text: str) -> str:
        if text in replacements:
            return replacements[text]
        if text == template_root or text.startswith((template_root + "/", template_root + "\\")):
            raise evidence.EvidenceError("fixture JSON contains an unregistered template path")
        return text

    if isinstance(value, str):
        return relocate(value)
    if isinstance(value, list):
        return [relocate_json(item, replacements, template_root, depth=depth + 1) for item in value]
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            target = relocate(key)
            if target in result:
                raise evidence.EvidenceError("fixture JSON relocation collides with an existing key")
            result[target] = relocate_json(item, replacements, template_root, depth=depth + 1)
        return result
    return value


def verify_template(manifest: str | Path, manifest_sha256: str, *, source_commit: str,
                    source_sha256: str, harness_sha256: str, require_deep: bool = True) -> dict:
    path = evidence.checked_local(manifest)
    pin = evidence.require_hash(manifest_sha256, "fixture template manifest pin")
    if evidence.sha256(path) != pin:
        raise evidence.EvidenceError("fixture template manifest changed")
    document = evidence.read_json(path)
    binding = generation_binding(source_commit, source_sha256, harness_sha256)
    if (not isinstance(document, dict) or document.get("kind") != "synthetic-production-fixtures"
            or document.get("owner") != "production-smoke" or document.get("fixtureRole") != TEMPLATE_ROLE
            or any(document.get(key) != value for key, value in binding.items())):
        raise evidence.EvidenceError("fixtures are not an immutable template for this production/harness generation")
    root_text = document.get("root")
    if not isinstance(root_text, str) or not root_text or not Path(root_text).is_absolute():
        raise evidence.EvidenceError("fixture template root is not absolute")
    root = evidence.checked_local(root_text)
    if path != root / "fixture-manifest.json" or not root.is_dir():
        raise evidence.EvidenceError("fixture template manifest is outside its declared root")
    if require_deep and len(str(root).encode("utf-8")) < 310:
        raise evidence.EvidenceError("fixture template long path was shortened")
    token = document.get("token")
    if not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{32}", token):
        raise evidence.EvidenceError("fixture template owner token is invalid")
    owner = evidence.read_json(root / "fixture-owner.json")
    if owner.get("owner") != "production-smoke" or owner.get("token") != token or owner.get("fixtureRole") != TEMPLATE_ROLE:
        raise evidence.EvidenceError("fixture template owner marker differs")
    evidence.require_hash(document.get("generatorSha256"), "fixture generator pin")
    files = evidence.manifest_records(document)
    if not 0 < len(files) <= 4096:
        raise evidence.EvidenceError("fixture template inventory is empty or exceeds its bound")
    names, folded, total = set(), set(), 0
    for row in files:
        relative = evidence.relative_path(row.get("path"))
        size = row.get("size")
        expected = evidence.require_hash(row.get("sha256"), "fixture file pin")
        if (relative in names or relative.casefold() in folded or relative == "fixture-manifest.json"
                or isinstance(size, bool) or not isinstance(size, int) or not 0 <= size <= evidence.MAX_JSON
                or row.get("format") not in {"bytes", "json"}):
            raise evidence.EvidenceError("fixture template file record is duplicate or malformed")
        file = evidence.checked_local(root / relative)
        if not stat.S_ISREG(file.lstat().st_mode) or file.stat().st_size != size or evidence.sha256(file) != expected:
            raise evidence.EvidenceError("fixture template file size/hash changed: " + relative)
        if row["format"] == "json":
            evidence.read_json(file)
        names.add(relative)
        folded.add(relative.casefold())
        total += size
    if "fixture-owner.json" not in names or total > MAX_FIXTURE_BYTES:
        raise evidence.EvidenceError("fixture template owner or byte closure is incomplete")
    directories = document.get("directories")
    if not isinstance(directories, list) or len(directories) > 8192:
        raise evidence.EvidenceError("fixture template directory closure is missing")
    expected_dirs = {evidence.relative_path(value) for value in directories}
    if len(expected_dirs) != len(directories) or len({value.casefold() for value in directories}) != len(directories):
        raise evidence.EvidenceError("fixture template directory inventory collides")
    actual_files, actual_dirs = set(), set()
    for candidate in root.rglob("*"):
        candidate = evidence.checked_local(candidate)
        relative = candidate.relative_to(root).as_posix()
        mode = candidate.lstat().st_mode
        if stat.S_ISREG(mode):
            actual_files.add(relative)
        elif stat.S_ISDIR(mode):
            actual_dirs.add(relative)
        else:
            raise evidence.EvidenceError("fixture template contains a special file")
    if actual_files != names | {"fixture-manifest.json"} or actual_dirs != expected_dirs:
        raise evidence.EvidenceError("fixture template has an unlisted file or directory")
    paths = document.get("pathTable")
    if not isinstance(paths, list) or not 0 < len(paths) <= 4096:
        raise evidence.EvidenceError("fixture template path table is missing or unbounded")
    registered = set()
    for row in paths:
        if not isinstance(row, dict):
            raise evidence.EvidenceError("fixture template path row is malformed")
        relative = evidence.relative_path(row.get("relative"))
        value = row.get("value")
        if not isinstance(value, str) or value in registered or value != str(root / relative):
            raise evidence.EvidenceError("fixture template path table escaped or repeats a path")
        registered.add(value)
    if not isinstance(document.get("scenarios"), list) or not document["scenarios"]:
        raise evidence.EvidenceError("fixture template scenario inventory is missing")
    replacements = {value: value for value in registered}
    replacements[str(root)] = str(root)
    relocate_json(document["scenarios"], replacements, str(root))
    for row in files:
        if row["format"] == "json":
            relocate_json(evidence.read_json(root / row["path"]), replacements, str(root))
    if evidence.sha256(path) != pin:
        raise evidence.EvidenceError("fixture template manifest changed during verification")
    return {"root": str(root), "fileCount": len(files), "totalBytes": total,
            "manifestSha256": pin, "document": document}



class FixtureWriter:
    def __init__(self, root: Path, app_version: str, binding: dict | None = None):
        self.root = evidence.checked_local(root, writable=True)
        if self.root.exists():
            raise evidence.EvidenceError("fixture namespace already exists")
        self.disk = evidence.capacity(self.root, 2 * 1024 * 1024, 256 * 1024 * 1024)
        self.root.mkdir(parents=True, exist_ok=False)
        self.records: list[dict] = []
        self.paths: dict[str, str] = {}
        self.token = uuid.uuid4().hex
        self.app_version = app_version
        self.binding = binding
        self.json("fixture-owner.json", {"owner": "production-smoke", "token": self.token,
            "kind": "synthetic-fixture", "fixtureRole": TEMPLATE_ROLE if binding else "mutable-legacy-fixture",
            "createdAt": evidence.utc_now()})

    def write(self, relative: str, data: bytes, *, json_document: bool = False) -> Path:
        rel = evidence.relative_path(relative)
        target = evidence.checked_local(self.root / rel, writable=True)
        evidence.capacity(target, len(data), 256 * 1024 * 1024)
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        self.records.append({"path": rel, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                             "format": "json" if json_document else "bytes"})
        return target

    def json(self, relative: str, value) -> Path:
        return self.write(relative, json_bytes(value), json_document=True)

    def path_ref(self, value: Path) -> str:
        path = evidence.checked_local(value)
        relative = evidence.relative_path(path.relative_to(self.root).as_posix())
        text = str(path)
        self.paths[text] = relative
        return text

    def scenario(self, name: str, *, accepted: bool = True, system_proof: bool = False,
                 corrupt_game: bool = False) -> dict:
        evidence.relative_path(name)
        if corrupt_game and not system_proof:
            raise evidence.EvidenceError("corrupt game proof requires the selected system root")
        prefix = name + "/"
        base = self.root / name
        executable = base / "exe/Pd-Launcher.exe"
        adjacent = base / "exe/Nickel"
        system_base = base / "system-config"
        app_base = base / "old-app-data"
        config = system_base / "studio.nickel.launcher" if system_proof else adjacent
        game = base / "games/registered"
        alternate = base / "games/alternate"
        settings = {"themeMode": "dark", "accent": "blue", "future": {
            "integerBeyondDouble": 9007199254740993, "unsignedMax": 18446744073709551615,
            "signedMin": -9223372036854775808, "nested": [True, None, "\u4fdd\u7559"]}}
        stored_accounts = [
            {"id": "offline-one", "type": "offline", "name": "SyntheticOne",
             "uuid": "0123456789abcdef0123456789abcdef", "email": None, "server": None, "avatarUrl": None},
            {"id": "offline-two", "type": "offline", "name": "SyntheticTwo",
             "uuid": "fedcba9876543210fedcba9876543210", "email": None, "server": None, "avatarUrl": None},
            {"id": "redaction-one", "type": "microsoft", "name": "SyntheticSecret",
             "uuid": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "email": "synthetic@example.invalid",
             "accessToken": SYNTHETIC_SECRETS[0], "refreshToken": SYNTHETIC_SECRETS[1],
             "clientToken": SYNTHETIC_SECRETS[2], "expiresAt": 1}]
        relative_config = config.relative_to(self.root).as_posix()
        self.json(relative_config + "/settings.json", settings)
        self.json(relative_config + "/accounts.json", stored_accounts)
        self.json(relative_config + "/active_account.json", {"id": "offline-one"})
        corrupt_bytes = b'{"gameDir": "unclosed-proof"\n'
        if corrupt_game:
            self.write(relative_config + "/game.json", corrupt_bytes)
        else:
            self.json(relative_config + "/game.json", {"gameDir": self.path_ref(game)})
        self.json(relative_config + "/instances.json", {"roots": [self.path_ref(game), self.path_ref(alternate)],
            "active": self.path_ref(game), "labels": {self.path_ref(game): "Display root"}})
        if accepted:
            self.json(relative_config + "/onboarding.json", {"eulaVersion": "minecraft-eula-v1",
                "eulaAcceptedAt": 1795251306000, "changelogSeenVersion": self.app_version})
        else:
            self.json(relative_config + "/onboarding.json", {})
        for directory in (executable.parent, system_base, app_base, alternate):
            evidence.capacity(directory, 0, 256 * 1024 * 1024)
            directory.mkdir(parents=True, exist_ok=True)
        foreign_proofs = []
        if system_proof:
            foreign_accounts = self.json(prefix + "exe/Nickel/accounts.json", [{"id": "must-not-merge", "type": "offline",
                "name": "ForeignRootAccount", "uuid": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}])
            foreign_game = self.json(prefix + "exe/Nickel/game.json", {"gameDir": self.path_ref(alternate)})
            foreign_proofs = [{"path": self.path_ref(path), "sha256": evidence.sha256(path)}
                              for path in (foreign_accounts, foreign_game)]
        game_prefix = game.relative_to(self.root).as_posix() + "/"
        named = game / "versions" / NAMED
        named_prefix = named.relative_to(self.root).as_posix() + "/"
        client = archive_bytes({"META-INF/MANIFEST.MF": b"Manifest-Version: 1.0\n", "fixture.txt": b"synthetic-client\n"})
        metadata = {"id": MC_VERSION, "type": "release", "mainClass": "net.minecraft.client.main.Main",
                    "javaVersion": {"component": "java-runtime-gamma", "majorVersion": 17},
                    "assets": "synthetic", "assetIndex": {"id": "synthetic", "totalSize": 0},
                    "downloads": {"client": {"sha1": hashlib.sha1(client).hexdigest(), "size": len(client)}},
                    "libraries": [], "arguments": {"jvm": ["-cp", "${classpath}"],
                    "game": ["--username", "${auth_player_name}", "--gameDir", "${game_directory}",
                             "--accessToken", "${auth_access_token}", "--version", "${version_name}"]}}
        self.json(game_prefix + "versions/" + MC_VERSION + "/" + MC_VERSION + ".json", metadata)
        self.write(game_prefix + "versions/" + MC_VERSION + "/" + MC_VERSION + ".jar", client)
        self.json(game_prefix + "assets/indexes/synthetic.json", {"objects": {}})
        self.json(named_prefix + "nickel.profile.json", {"schemaVersion": 1, "name": NAMED,
            "minecraftVersion": MC_VERSION, "loader": None, "loaderVersion": "", "loaderArtifact": None, "ready": True})
        self.json(named_prefix + NAMED + ".json", {**metadata, "id": NAMED, "inheritsFrom": MC_VERSION,
            "nickelProfile": {"minecraftVersion": MC_VERSION, "loader": None, "loaderVersion": ""},
            "nickelCustom": {"name": "Same display label", "favorite": False, "icon": "grass"}})
        self.json(named_prefix + "versions/" + MC_VERSION + "/" + MC_VERSION + ".json", metadata)
        self.write(named_prefix + "versions/" + MC_VERSION + "/" + MC_VERSION + ".jar", client)
        self.json(named_prefix + "assets/indexes/synthetic.json", {"objects": {}})
        self.json(named_prefix + "nickel.settings.json", {"futureInstance": {"integerBeyondDouble": 9007199254740993}})
        mod = archive_bytes({"fabric.mod.json": json_bytes({"schemaVersion": 1, "id": "smoke_mod",
            "version": "1.0.0", "name": "Synthetic Mod", "description": "Synthetic acceptance only",
            "environment": "*", "depends": {"minecraft": ">=1.20.1"}})})
        self.write(named_prefix + "mods/synthetic.jar", mod)
        self.write(named_prefix + "mods/foreign-preserve.bin", b"not-owned-by-resource-transaction\n")
        pack_metadata = json_bytes({"pack": {"pack_format": 15, "description": "Synthetic resource pack"}})
        resource_pack = archive_bytes({"pack.mcmeta": pack_metadata, "assets/smoke/readme.txt": b"synthetic\n"})
        self.write(prefix + "inputs/resource-valid.zip", resource_pack)
        self.write(prefix + "inputs/resource-invalid.zip", b"not-an-archive\n")
        self.write(prefix + "inputs/skin.png", skin_png())
        mrpack = archive_bytes({"modrinth.index.json": json_bytes({"formatVersion": 1, "game": "minecraft",
            "versionId": "1.0.0", "name": "Synthetic Imported", "summary": "Independent production fixture",
            "files": [], "dependencies": {"minecraft": MC_VERSION}}),
            "overrides/config/synthetic.txt": b"imported-by-real-pack-service\n"})
        self.write(prefix + "inputs/synthetic.mrpack", mrpack)
        self.write(named_prefix + "logs/latest.log", b"synthetic old game log\n")
        self.write(named_prefix + "saves/foreign-world/level.dat", b"must-never-clean\n")
        result = {"name": name, "root": self.path_ref(base), "executable": self.path_ref(executable),
                  "systemConfigBase": self.path_ref(system_base), "appDataBase": self.path_ref(app_base), "macos": False,
                  "expectedConfigRoot": self.path_ref(config), "expectedGameRoot": self.path_ref(game),
                  "namedVersionId": NAMED, "minecraftVersion": MC_VERSION,
                  "expectedNamedDirectory": self.path_ref(named), "accepted": accepted,
                  "settingsUnknown": settings["future"], "inputs": self.path_ref(base / "inputs"),
                  "foreignRootProofs": foreign_proofs}
        if corrupt_game:
            result.update({"corruptProof": True, "corruptProofFile": self.path_ref(config / "game.json"),
                           "corruptProofSha256": hashlib.sha256(corrupt_bytes).hexdigest()})
        return result

    def finish(self, scenarios: list[dict]) -> dict:
        records = sorted(self.records, key=lambda row: row["path"])
        directories = sorted(path.relative_to(self.root).as_posix() for path in self.root.rglob("*") if path.is_dir())
        document = {"schema": 1, "kind": "synthetic-production-fixtures", "owner": "production-smoke",
            "token": self.token, "root": str(self.root), "rootCharacters": len(str(self.root)),
            "fixtureRole": TEMPLATE_ROLE if self.binding else "mutable-legacy-fixture",
            "generatorSha256": evidence.sha256(Path(__file__).resolve()),
            "appVersion": self.app_version, "capacity": self.disk, "scenarios": scenarios,
            "syntheticRedactionSentinels": SYNTHETIC_SECRETS, "files": records, "directories": directories,
            "pathTable": [{"value": value, "relative": relative} for value, relative in sorted(self.paths.items())],
            "productionPass": False,
            "references": ["native/core/instances/settings.cpp", "native/core/instances/instances.cpp",
                           "native/app/service/startup_actions.cpp"]}
        if self.binding:
            document.update(self.binding)
        evidence.write_json_new(self.root / "fixture-manifest.json", document)
        return document


def generate(output: str | Path, app_version: str, *, deep: bool = True,
             source_commit=None, source_sha256=None, harness_sha256=None) -> dict:
    if not isinstance(app_version, str) or not app_version or len(app_version) > 100:
        raise evidence.EvidenceError("explicit product version is required")
    binding = generation_binding(source_commit, source_sha256, harness_sha256)
    root = evidence.checked_local(output, writable=True)
    if root.exists():
        raise evidence.EvidenceError("fixture publication namespace already exists")
    if deep:
        while len(str(root).encode("utf-8")) < 310:
            root /= "deep-synthetic-path-preserved-0123456789"
    writer = FixtureWriter(root, app_version, binding)
    scenarios = [writer.scenario("accepted"), writer.scenario("unaccepted", accepted=False),
                 writer.scenario("system-priority", system_proof=True),
                 writer.scenario("corrupt-proof", system_proof=True, corrupt_game=True)]
    return writer.finish(scenarios)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--app-version", required=True)
    parser.add_argument("--source-commit", help="Full production commit; all three generation pins are required together")
    parser.add_argument("--source-sha256", help="Raw immutable production source manifest SHA256")
    parser.add_argument("--harness-sha256", help="Raw compiled owned harness source manifest SHA256")
    parser.add_argument("--shallow", action="store_true", help="Only for tool self-tests, never production paths")
    args = parser.parse_args()
    try:
        result = generate(args.output, args.app_version, deep=not args.shallow,
            source_commit=args.source_commit, source_sha256=args.source_sha256, harness_sha256=args.harness_sha256)
        print(json.dumps({"root": result["root"], "rootCharacters": result["rootCharacters"],
                          "fixtureRole": result["fixtureRole"], "fileCount": len(result["files"]),
                          "productionPass": False}, indent=2))
        return 0
    except (OSError, evidence.EvidenceError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
