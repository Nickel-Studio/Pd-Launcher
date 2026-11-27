"""Generate independent, synthetic compatibility vectors; never read user data."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path

import cryptography
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


ROOT_TOKEN = "$ROOT"
BASE_MANIFEST = {
    "schemaVersion": 1,
    "format": "nickel-portable",
    "packageId": "0123456789abcdef0123456789abcdef",
    "name": "Synthetic pack",
    "version": "1.0.0",
    "instanceName": "Synthetic instance",
    "payload": "Nickel/pack.mrpack",
    "gameDirectory": ".minecraft",
    "dataDirectory": "Nickel",
}


def encode_payload(nonce: bytes, ciphertext: bytes) -> str:
    return "v1:" + base64.b64encode(nonce + ciphertext).decode("ascii")


def vault_vectors() -> dict:
    valid = []
    texts = ["", "synthetic password", "p@ss word \u5bc6\u7801", "\u6d4b\u8bd5\U0001f680", "a\x00b", "\n\tvalue\r\n"]
    for index, text in enumerate(texts):
        key = bytes((byte + index) % 256 for byte in range(32))
        nonce = bytes((byte + 31 * index) % 256 for byte in range(12))
        sealed = encode_payload(nonce, AESGCM(key).encrypt(nonce, text.encode("utf-8"), None))
        valid.append({"name": f"utf8-{index}", "keyHex": key.hex(), "nonceHex": nonce.hex(),
                      "plaintext": text, "sealed": sealed})
    zero_key, zero_nonce = bytes(32), bytes(12)
    zero_ciphertext = AESGCM(zero_key).encrypt(zero_nonce, b"", None)
    # Published AES-256-GCM empty-plaintext vector, independent of product code.
    if zero_ciphertext.hex() != "530f8afbc74536b9a963b4f1c4cb738b":
        raise RuntimeError("Independent AES-GCM implementation disagrees with NIST vector")
    valid.append({"name": "nist-empty", "keyHex": zero_key.hex(), "nonceHex": zero_nonce.hex(),
                  "plaintext": "", "sealed": encode_payload(zero_nonce, zero_ciphertext)})
    first = valid[1]
    payload = bytearray(base64.b64decode(first["sealed"][3:], validate=True))
    invalid = []
    for name, at in [("nonce-tamper", 0), ("ciphertext-tamper", 12), ("tag-tamper", len(payload) - 1)]:
        changed = payload.copy()
        changed[at] ^= 0x80
        invalid.append({"name": name, "keyHex": first["keyHex"],
                        "sealed": "v1:" + base64.b64encode(changed).decode("ascii")})
    wrong_key = bytes([255]) * 32
    invalid.append({"name": "wrong-key", "keyHex": wrong_key.hex(), "sealed": first["sealed"]})
    for name, sealed in [("plaintext-rejected", "synthetic password"), ("unknown-prefix", "v2:" + first["sealed"][3:]),
                         ("bad-base64", "v1:!!!!"), ("nonce-only", "v1:" + base64.b64encode(bytes(12)).decode()),
                         ("missing-tag", "v1:" + base64.b64encode(bytes(13)).decode()),
                         ("empty-payload", "v1:"), ("missing-padding", first["sealed"].rstrip("="))]:
        invalid.append({"name": name, "keyHex": first["keyHex"], "sealed": sealed})
    key, nonce = bytes([17]) * 32, bytes([99]) * 12
    invalid.append({"name": "authenticated-non-utf8", "keyHex": key.hex(),
                    "sealed": encode_payload(nonce, AESGCM(key).encrypt(nonce, b"\xff\xfe\x80", None))})
    return {"schemaVersion": 1, "origin": "pyca cryptography AESGCM; protocol read from legacy vault.rs",
            "valid": valid, "invalid": invalid}


def manifest_vectors() -> dict:
    valid = [{"name": "no-launcher", "manifest": BASE_MANIFEST}]
    for os_name, filename in [("windows", "Nickel.exe"), ("linux", "Nickel-launcher")]:
        valid.append({"name": os_name, "manifest": {**BASE_MANIFEST, "launcher": {
            "path": filename, "os": os_name, "arch": "x86_64"}, "personalization": "Nickel/personalization.json"}})
    invalid = []
    changes = [
        ("future-schema", "schemaVersion", 2), ("float-schema", "schemaVersion", 1.0),
        ("old-brand-format", "format", "nickelPack"), ("short-package-id", "packageId", "a" * 31),
        ("bad-package-id", "packageId", "g" * 32), ("empty-name", "name", " "),
        ("control-version", "version", "1\n2"), ("oversized-name", "name", "a" * 513),
        ("path-payload", "payload", "../pack.mrpack"), ("alternate-data-root", "dataDirectory", "data"),
        ("alternate-game-root", "gameDirectory", "minecraft"),
        ("path-personalization", "personalization", "Nickel/../other.json"),
        ("blank-instance", "instanceName", ""), ("instance-traversal", "instanceName", "test..bad"),
        ("reserved-instance", "instanceName", "CON"), ("padded-instance", "instanceName", " test"),
        ("trailing-dot-instance", "instanceName", "test."),
        ("unknown-top-level", "futureField", True),
    ]
    for name, field, value in changes:
        invalid.append({"name": name, "manifest": {**BASE_MANIFEST, field: value}})
    for name, launcher in [
        ("wrong-exe", {"path": "bin/Nickel.exe", "os": "windows", "arch": "x64"}),
        ("wrong-os", {"path": "Nickel.exe", "os": "macos", "arch": "arm64"}),
        ("bad-arch", {"path": "Nickel.exe", "os": "windows", "arch": "../x64"}),
        ("empty-arch", {"path": "Nickel.exe", "os": "windows", "arch": ""}),
        ("nested-unknown", {"path": "Nickel.exe", "os": "windows", "arch": "x64", "extra": True}),
    ]:
        invalid.append({"name": name, "manifest": {**BASE_MANIFEST, "launcher": launcher}})
    return {"schemaVersion": 1, "legacySchema": 1, "valid": valid, "invalid": invalid}


def path_vectors() -> dict:
    app = "system-config/studio.nickel.launcher"
    adjacent = "exe/Nickel"
    cases = []

    def add(name, files=None, directories=None, expected_config=adjacent, expected_game="exe/.minecraft", **extra):
        cases.append({"name": name, "executable": "exe/Nickel.exe", "macos": False,
                      "files": files or {}, "directories": directories or [], "expectedConfig": expected_config,
                      "expectedGame": expected_game, "expectedBundledGame": "exe/.minecraft", **extra})

    add("empty-roots")
    add("adjacent-existing", {f"{adjacent}/accounts.json": "[]"})
    add("whole-root-priority", {f"{app}/credentials.key": "wrong-but-existing-key",
                                f"{adjacent}/accounts.json": "[]"}, expected_config=app)
    add("ignore-unrelated-file", {f"{app}/unrelated.json": "{}"})
    for entry in ["settings.json", "accounts.json", "active_account.json", "game.json", "instances.json",
                  "secrets.local.json", "credentials.key", "offline-skins", "agents", "cache"]:
        add(f"existing-marker-{entry}", {f"{app}/{entry}": "corrupt-or-cached"}, expected_config=app)
    add("game-json-precedes-registry", {f"{app}/game.json": json.dumps({"gameDir": "$ROOT/games/custom"}),
                                       f"{app}/instances.json": json.dumps({"roots": ["$ROOT/games/first"], "active": "$ROOT/games/active"})},
        expected_config=app, expected_game="games/custom")
    add("registry-active-precedes-first", {f"{app}/instances.json": json.dumps({"roots": ["$ROOT/games/first"], "active": "$ROOT/games/active"})},
        expected_config=app, expected_game="games/active")
    add("registry-first-fallback", {f"{app}/game.json": '{"gameDir":"  "}',
                                    f"{app}/instances.json": json.dumps({"roots": ["$ROOT/games/first"], "active": " "})},
        expected_config=app, expected_game="games/first")
    add("legacy-versions-root", directories=["old-app-data/minecraft/versions"], expected_game="old-app-data/minecraft")
    add("legacy-game-data-root", {"old-app-data/minecraft/gameData.json": "{}"}, expected_game="old-app-data/minecraft")
    add("empty-legacy-root-not-selected", directories=["old-app-data/minecraft"])
    add("portable-marker-precedes-all", {"exe/nickel.portable.json": json.dumps(BASE_MANIFEST),
                                          f"{app}/settings.json": "{}", f"{app}/game.json": '{"gameDir":"$ROOT/games/custom"}'})
    add("corrupt-portable-propagates", {"exe/nickel.portable.json": "{broken", f"{app}/settings.json": "{}"}, expectError=True)
    add("future-portable-propagates", {"exe/nickel.portable.json": json.dumps({**BASE_MANIFEST, "schemaVersion": 2}), f"{app}/settings.json": "{}"}, expectError=True)
    add("canonical-macos-bundle", executable="exe/Nickel.app/Contents/MacOS/nickel", macos=True)
    add("incomplete-macos-not-ancestor", executable="exe/not-a-bundle/Contents/MacOS/nickel", macos=True,
        expected_config="exe/not-a-bundle/Contents/MacOS/Nickel", expected_game="exe/not-a-bundle/Contents/MacOS/.minecraft",
        expectedBundledGame="exe/not-a-bundle/Contents/MacOS/.minecraft")
    return {"schemaVersion": 1, "rootToken": ROOT_TOKEN, "cases": cases}


def settings_vectors() -> dict:
    unknown = {"themeMode": "dark", "accent": "custom", "future": {
        "integerBeyondDouble": 9007199254740993, "unsignedMax": 18446744073709551615,
        "signedMin": -9223372036854775808, "array": [True, None, {"deep": "\u4fdd\u7559"} ]}}
    valid = [{"name": "empty-object", "text": "{}", "expected": {}},
             {"name": "unknown-integers", "text": json.dumps(unknown, ensure_ascii=True), "expected": unknown}]
    invalid = [{"name": name, "text": text, "expected": {}} for name, text in [
        ("empty", ""), ("malformed", "{not-json"), ("array", "[]"), ("null", "null"),
        ("boolean", "true"), ("number", "7"), ("string", '"text"')]]
    invalid.append({"name": "oversized", "text": '{"padding":"' + "x" * (256 * 1024) + '"}', "expected": {}})
    return {"schemaVersion": 1, "limitBytes": 256 * 1024, "valid": valid, "invalid": invalid}


def write_fixtures(output: Path) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    documents = {"vault.json": vault_vectors(), "portable.json": manifest_vectors(),
                 "paths.json": path_vectors(), "settings.json": settings_vectors()}
    hashes = {}
    for filename, value in documents.items():
        data = (json.dumps(value, ensure_ascii=True, indent=2) + "\n").encode("utf-8")
        (output / filename).write_bytes(data)
        hashes[filename] = hashlib.sha256(data).hexdigest()
    manifest = {"schemaVersion": 1, "kind": "synthetic-native-golden", "cryptographyVersion": cryptography.__version__,
                "references": ["native/core/config/paths.cpp", "native/app/main.cpp",
                               "native/core/packs/portable.cpp", "native/core/config/vault.cpp"],
                "files": hashes}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = write_fixtures(args.output)
    print(json.dumps({"output": str(args.output.resolve()), "sha256": manifest["files"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
