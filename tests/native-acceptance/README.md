# Independent native acceptance

This directory tests actual compiled services against synthetic legacy-contract inputs and audits real Windows artifacts. It is not a substitute for game/authentication/UI/platform/performance acceptance.

## Synthetic fixtures

`generate_fixtures.py` uses test-only pyca cryptography 50.0.2, not product crypto. Its AES-256-GCM empty-plaintext NIST vector is checked before generation. Fixed keys and nonces are exclusively public test vectors; production encryption must generate fresh nonces. All text is UTF-8; JSON escapes preserve ASCII source files and exact 64-bit integers.

Run `python tests/native-acceptance/generate_fixtures.py --output <private-generated-directory>`. The output manifest records hashes and legacy source references. The generator never reads real accounts, keys, settings or games.

`foundation_golden.cpp` links real nickel_foundation services and operates only below unique fixture-owned temporary directories. Checks throw rather than relying on assertions that disappear in Release builds. It covers whole-root precedence, game/bundle paths, strict portable schema 1, settings shape/size/unknown integers, exact vault encoding and failure side effects. It does not claim process ownership, ACL, native IME, screenshots or full migration recovery are verified.

## Artifact evidence

`audit_windows.py` checks PE normal and delay-load imports and, when supplied, an observed module list from a separately controlled process. Non-system libraries and dynamic CRT dependencies fail. Imports alone never prove absence of late runtime loading; without module evidence that runtime gate remains unverified.

`test_tools.py` validates the independent generator and artifact parser, not the launcher. Actual launcher evidence must identify the executable hash, service scenarios and inspected results. Build-release integrates CTest only after the production library target is present. Generated files/build outputs belong outside version control.
