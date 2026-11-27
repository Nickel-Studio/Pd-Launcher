#!/usr/bin/env bash
set -euo pipefail

root="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/../..")"
owner="$root/.snow/posix-cleanup-acceptance"
base="$root/.snow/native-shared"
shared=""
archive=""
requested_build=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-root) [[ $# -ge 2 ]] || exit 2; shared="$(readlink -f "$2")"; shift 2 ;;
        --archive-root) [[ $# -ge 2 ]] || exit 2; archive="$(readlink -f "$2")"; shift 2 ;;
        --reuse-build) [[ $# -ge 2 ]] || exit 2; requested_build="$(readlink -f "$2")"; shift 2 ;;
        *) echo 'usage: run.sh --source-root exact-owner-source [--archive-root exact-owner-evidence] [--reuse-build exact-owner-build]' >&2; exit 2 ;;
    esac
done
[[ "$shared" == "$root/.snow/"* && -f "$shared/publication-complete.json" && ! -e "$shared/.publisher.lock" ]] || exit 2
[[ -z "$archive" || "$archive" == "$root/.snow/"* ]] || exit 2
[[ -f "$owner/owner.json" ]] || exit 2
export TEMP="$owner/tmp" TMP="$owner/tmp" TMPDIR="$owner/tmp" PYTHONDONTWRITEBYTECODE=1 LC_ALL=C.UTF-8
[[ -d "$TMPDIR" ]] || exit 2
stamp="$(date -u +%Y%m%dT%H%M%SZ)-$$"
if [[ -n "$requested_build" ]]; then
    build="$requested_build"
    [[ "$build" == "$owner/build/"* && -f "$build/owner.txt" ]] || exit 2
    grep -qx 'owner=posix-cleanup-acceptance' "$build/owner.txt" || exit 2
else
    build="$owner/build/run-$stamp"
    mkdir "$build"
    printf 'owner=posix-cleanup-acceptance\nrun=%s\n' "$stamp" > "$build/owner.txt"
fi
evidence="$owner/evidence/run-$stamp"
mkdir "$evidence"
printf 'owner=posix-cleanup-acceptance\nrun=%s\n' "$stamp" > "$evidence/owner.txt"
exec > >(tee "$evidence/runner.log") 2>&1
printf 'RUN_EVIDENCE=%s\nRUN_BUILD=%s\n' "$evidence" "$build"
manifest="$(sha256sum "$shared/snapshot.json" | cut -d ' ' -f 1)"
base_manifest="$(sha256sum "$base/snapshot.json" | cut -d ' ' -f 1)"
verify=(python3 "$root/tests/native-posix-cleanup/verify_snapshot.py" --worktree "$root" --source-root "$shared" --expect-manifest "$manifest" --expect-base-manifest "$base_manifest")
[[ -z "$archive" ]] || verify+=(--archive-root "$archive")
finish() {
    status=$?
    trap - EXIT
    printf 'runner_exit=%s\n' "$status" | tee "$evidence/runner.exit.txt"
    du -sb "$owner" "$build" "$evidence" | tee "$evidence/final-size.txt"
    printf '%s\n' "$evidence" > "$owner/latest-run.txt"
    exit "$status"
}
trap finish EXIT
capacity() {
    local script
    script="$(wslpath -w "$root/tests/native-posix-cleanup/capacity.ps1")"
    powershell.exe -NoLogo -NoProfile -NonInteractive -File "$script" -Phase "$1" | tee -a "$evidence/capacity.log"
}
capacity preflight

bash "$root/tests/native-posix-cleanup/host.sh" > "$evidence/host.txt" 2>&1
"${verify[@]}" > "$evidence/inputs-before.txt"
cp --update=none "$shared/snapshot.json" "$evidence/snapshot.json"
cp --update=none "$shared/publication-complete.json" "$evidence/publication-complete.json"
cp --update=none "$base/snapshot.json" "$evidence/base-snapshot.json"
cp --update=none "$base/publication-complete.json" "$evidence/base-publication-complete.json"
printf 'production_only=%s\nbase_evidence_only=%s\narchive_evidence_only=%s\n' "$shared" "$base" "$archive" > "$evidence/source-roots.txt"
"${verify[@]}" --copy-provenance-to "$evidence" > "$evidence/provenance-copy.txt"
{
    dpkg-query -W g++-13 gcc-13 libssl-dev nlohmann-json3-dev cmake ninja-build
    pkg-config --modversion openssl
    sha256sum "$(readlink -f "$(command -v g++)")" /usr/include/nlohmann/json.hpp
    sha256sum "$(readlink -f /usr/lib/x86_64-linux-gnu/libcrypto.so)"
    ldd --version | head -n 1
} > "$evidence/system-inputs.txt" 2>&1
bytes="$(du -sb "$owner" | cut -f 1)"
[[ "$bytes" -lt 134217728 ]] || { echo 'owner 128 MiB cap exceeded before build'; exit 3; }
capacity configure
configure=(cmake -S "$root/tests/native-posix-cleanup" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG -g0' "-DNICKEL_POSIX_SOURCE_ROOT=$shared" "-DNICKEL_POSIX_MANIFEST_SHA256=$manifest")
printf '%q ' "${configure[@]}" > "$evidence/configure.command.txt"; printf '\n' >> "$evidence/configure.command.txt"
set +e
"${configure[@]}" > "$evidence/configure.log" 2>&1
code=$?
set -e
printf 'exit=%s\n' "$code" > "$evidence/configure.exit.txt"
[[ "$code" -eq 0 ]] || { cat "$evidence/configure.log"; exit "$code"; }
capacity build
printf 'cmake --build %q --parallel 1 --verbose\n' "$build" > "$evidence/build.command.txt"
set +e
cmake --build "$build" --parallel 1 --verbose > "$evidence/build.log" 2>&1
code=$?
set -e
printf 'exit=%s\n' "$code" > "$evidence/build.exit.txt"
[[ "$code" -eq 0 ]] || { cat "$evidence/build.log"; exit "$code"; }
"${verify[@]}" --build-dir "$build" > "$evidence/build-inputs.txt"
cp --update=none "$build/CMakeCache.txt" "$evidence/CMakeCache.txt"
cp --update=none "$build/compile_commands.json" "$evidence/compile_commands.json"
sha256sum "$build/posix_cleanup_plain" "$build/posix_cleanup_controlled" > "$evidence/executable-sha256.txt"
cp --update=none "$build/posix_cleanup_plain" "$evidence/posix_cleanup_plain"
cp --update=none "$build/posix_cleanup_controlled" "$evidence/posix_cleanup_controlled"
mkdir "$evidence/test-inputs"
for name in acceptance.cpp fixtures.hpp CMakeLists.txt run.sh host.sh verify_snapshot.py reproduce.sh capacity.ps1 verify_fix1.ps1 test_verify_snapshot.py; do
    cp --update=none "$root/tests/native-posix-cleanup/$name" "$evidence/test-inputs/$name"
done
ldd "$build/posix_cleanup_plain" > "$evidence/linked-libraries.txt"
bytes="$(du -sb "$owner" | cut -f 1)"
[[ "$bytes" -lt 134217728 ]] || { echo 'owner 128 MiB cap exceeded after build'; exit 3; }
"${verify[@]}" > "$evidence/inputs-pretest.txt"
failed=0
for binary in posix_cleanup_plain posix_cleanup_controlled; do
    capacity "$binary"
    printf '%q\n' "$build/$binary" > "$evidence/$binary.command.txt"
    set +e
    "$build/$binary" > "$evidence/$binary.test.log" 2>&1
    code=$?
    set -e
    printf 'exit=%s\n' "$code" > "$evidence/$binary.exit.txt"
    cat "$evidence/$binary.test.log"
    [[ "$code" -eq 0 ]] || failed=1
done
capacity residual
printf '%q --exercise-residual-boundary\n' "$build/posix_cleanup_controlled" > "$evidence/residual.command.txt"
set +e
"$build/posix_cleanup_controlled" --exercise-residual-boundary > "$evidence/residual-boundary.log" 2>&1
code=$?
set -e
printf 'exit=%s\n' "$code" > "$evidence/residual.exit.txt"
cat "$evidence/residual-boundary.log"
[[ "$code" -eq 0 ]] || failed=1
"${verify[@]}" > "$evidence/inputs-after.txt"
[[ "$(du -sb "$owner" | cut -f 1)" -lt 134217728 ]] || { echo 'owner 128 MiB cap exceeded'; exit 3; }
exit "$failed"
