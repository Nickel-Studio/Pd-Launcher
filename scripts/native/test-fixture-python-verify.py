"""Fail-closed self-check for the exact-pinned independent test-fixture cryptography toolchain.

Hosted CI runs this right after the hash-pinned wheel install (scripts/native/test-fixture-python.lock)
and before any CMake configure: the fixture generators (native/tests/foundation/make_fixtures.py and
the native/tests/{transport,community}/socket_fixture.py fixtures) import cryptography, so a missing
or silently different install must abort the workflow before configure instead of failing late
inside the serial build. This toolchain is test-only: it never enters the product and never
substitutes the supplied static OpenSSL 3.5.9.
"""
import importlib.metadata
import platform
import site
import sys
import sysconfig
from pathlib import Path

PINNED = {"cryptography": "48.0.1", "cffi": "2.1.1", "pycparser": "2.23"}

# NIST GCM revised spec known-answer tests (AES-256, all-zero key/nonce), independently
# cross-checked with OpenSSL (node:crypto) and identical to the approved macOS fixture
# toolchain self-check in scripts/native/macos-delivery.test-tools.py.
NIST_GCM_AES256_EMPTY_TAG = "530f8afbc74536b9a963b4f1c4cb738b"
NIST_GCM_AES256_BLOCK_CT = "cea7403d4d606b6e074ec5d3baf39d18"
NIST_GCM_AES256_BLOCK_TAG = "d0d1c8a799996bf0265b98b5d48ab919"


def fail(message):
    raise SystemExit(f"test-fixture-python-verify: {message}")


def main():
    if platform.python_implementation() != "CPython":
        fail(f"fixture toolchain supports CPython only, found {platform.python_implementation()}")
    for distribution, version in PINNED.items():
        try:
            actual = importlib.metadata.version(distribution)
        except importlib.metadata.PackageNotFoundError:
            fail(f"{distribution} is not installed in this interpreter")
        if actual != version:
            fail(f"{distribution} {actual} differs from the pinned {version}")
    try:
        import cryptography
        from cryptography import x509  # noqa: F401  (same wheel serves socket_fixture.py)
        from cryptography.exceptions import InvalidTag
        from cryptography.hazmat.primitives import hashes, serialization  # noqa: F401
        from cryptography.hazmat.primitives.asymmetric import rsa  # noqa: F401
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except Exception as error:  # pragma: no cover - fail-closed reporting path
        fail(f"cryptography cannot be imported: {error!r}")
    module_root = Path(cryptography.__file__).resolve().parent.parent
    allowed_roots = {Path(sysconfig.get_paths()["purelib"]).resolve()}
    if site.ENABLE_USER_SITE:
        allowed_roots.add(Path(site.getusersitepackages()).resolve())
    if module_root not in allowed_roots:
        fail(f"cryptography loaded from {module_root}, not this interpreter's own site-packages")
    zero_key, zero_nonce = bytes(32), bytes(12)
    sealed = AESGCM(zero_key).encrypt(zero_nonce, b"", None)
    if sealed.hex() != NIST_GCM_AES256_EMPTY_TAG:
        fail(f"AES-256-GCM empty known-answer mismatch: {sealed.hex()}")
    sealed_block = AESGCM(zero_key).encrypt(zero_nonce, bytes(16), None)
    if sealed_block.hex() != NIST_GCM_AES256_BLOCK_CT + NIST_GCM_AES256_BLOCK_TAG:
        fail(f"AES-256-GCM single-block known-answer mismatch: {sealed_block.hex()}")
    if AESGCM(zero_key).decrypt(zero_nonce, sealed_block, None) != bytes(16):
        fail("AES-256-GCM decrypt round-trip failed")
    fixture_key, fixture_nonce = bytes(range(32)), bytes(range(12))
    fixture_plain = "\u96ea/\U0001f680/quote\"\\".encode("utf-8")
    round_trip = AESGCM(fixture_key).decrypt(
        fixture_nonce, AESGCM(fixture_key).encrypt(fixture_nonce, fixture_plain, None), None)
    if round_trip != fixture_plain:
        fail("fixture-shaped UTF-8 round-trip failed")
    try:
        AESGCM(zero_key).decrypt(zero_nonce, sealed_block[:-1] + bytes([sealed_block[-1] ^ 1]), None)
    except InvalidTag:
        pass
    else:
        fail("tampered AES-256-GCM tag was accepted")
    print("Pinned independent test-fixture cryptography toolchain verified: "
          + ", ".join(f"{name}=={version}" for name, version in PINNED.items())
          + f" on CPython {platform.python_version()} ({sys.platform})")


if __name__ == "__main__":
    main()
