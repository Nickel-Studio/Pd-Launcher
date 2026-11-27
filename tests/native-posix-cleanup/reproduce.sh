#!/usr/bin/env bash
set -euo pipefail
root="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/../..")"
owner="$root/.snow/posix-cleanup-acceptance"
base="$root/.snow/native-shared"
shared=""
archive=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-root) [[ $# -ge 2 ]] || exit 2; shared="$(readlink -f "$2")"; shift 2 ;;
        --archive-root) [[ $# -ge 2 ]] || exit 2; archive="$(readlink -f "$2")"; shift 2 ;;
        --*) exit 2 ;;
        *) break ;;
    esac
done
[[ $# -ge 1 && $# -le 2 && -f "$owner/owner.json" ]] || exit 2
[[ "$shared" == "$root/.snow/"* && -f "$shared/publication-complete.json" && ! -e "$shared/.publisher.lock" ]] || exit 2
[[ -z "$archive" || "$archive" == "$root/.snow/"* ]] || exit 2
build="$(readlink -f "$1")"
repeats="${2:-5}"
[[ "$build" == "$owner/build/"* && ! -L "$1" && -f "$build/owner.txt" ]] || exit 2
grep -qx 'owner=posix-cleanup-acceptance' "$build/owner.txt" || exit 2
[[ "$repeats" =~ ^[1-9][0-9]?$ && "$repeats" -le 20 ]] || exit 2
export TEMP="$owner/tmp" TMP="$owner/tmp" TMPDIR="$owner/tmp" PYTHONDONTWRITEBYTECODE=1 LC_ALL=C.UTF-8
stamp="$(date -u +%Y%m%dT%H%M%SZ)-$$"
evidence="$owner/evidence/repro-$stamp"
mkdir "$evidence"
printf 'owner=posix-cleanup-acceptance\nrun=%s\n' "$stamp" > "$evidence/owner.txt"
exec > >(tee "$evidence/runner.log") 2>&1
manifest="$(sha256sum "$shared/snapshot.json" | cut -d ' ' -f 1)"
base_manifest="$(sha256sum "$base/snapshot.json" | cut -d ' ' -f 1)"
verify=(python3 "$root/tests/native-posix-cleanup/verify_snapshot.py" --worktree "$root" --source-root "$shared" --expect-manifest "$manifest" --expect-base-manifest "$base_manifest")
[[ -z "$archive" ]] || verify+=(--archive-root "$archive")
finish() {
    code=$?
    trap - EXIT
    printf 'runner_exit=%s\n' "$code" | tee "$evidence/runner.exit.txt"
    du -sb "$owner" | tee "$evidence/owner-size.txt"
    printf '%s\n' "$evidence" > "$owner/latest-repro.txt"
    exit "$code"
}
trap finish EXIT
capacity() {
    local script
    script="$(wslpath -w "$root/tests/native-posix-cleanup/capacity.ps1")"
    powershell.exe -NoLogo -NoProfile -NonInteractive -File "$script" -Phase "$1" | tee -a "$evidence/capacity.log"
}
capacity preflight

printf 'REPRO_EVIDENCE=%s\nBUILD=%s\n' "$evidence" "$build"
"${verify[@]}" > "$evidence/inputs-before.txt"
[[ -f "$build/CMakeCache.txt" && -f "$build/compile_commands.json" ]] || { echo 'configured owner build required'; exit 2; }
cached_source="$(sed -n 's/^NICKEL_POSIX_SOURCE_ROOT:PATH=//p' "$build/CMakeCache.txt")"
cached_manifest="$(sed -n 's/^NICKEL_POSIX_MANIFEST_SHA256:STRING=//p' "$build/CMakeCache.txt")"
[[ -n "$cached_source" && "$(readlink -f "$cached_source")" == "$shared" && "$cached_manifest" == "$manifest" ]] || { echo 'build source root/SHA does not match explicitly selected frozen input'; exit 2; }
"${verify[@]}" --build-dir "$build" > "$evidence/build-inputs.txt"
printf 'CACHED_BUILD_SOURCE_ROOT=%s\nCACHED_BUILD_MANIFEST_SHA256=%s\n' "$cached_source" "$cached_manifest"
cp --update=none "$build/CMakeCache.txt" "$evidence/CMakeCache.txt"
cp --update=none "$shared/snapshot.json" "$evidence/snapshot.json"
cp --update=none "$shared/publication-complete.json" "$evidence/publication-complete.json"
cp --update=none "$base/snapshot.json" "$evidence/base-snapshot.json"
cp --update=none "$base/publication-complete.json" "$evidence/base-publication-complete.json"
printf 'production_only=%s\nbase_evidence_only=%s\narchive_evidence_only=%s\n' "$shared" "$base" "$archive" > "$evidence/source-roots.txt"
"${verify[@]}" --copy-provenance-to "$evidence" > "$evidence/provenance-copy.txt"
bash "$root/tests/native-posix-cleanup/host.sh" > "$evidence/host.txt" 2>&1
first="$owner/evidence/run-20261121T141732Z-58756"
for binary in posix_cleanup_plain posix_cleanup_controlled; do
    if [[ -f "$first/executable-sha256.txt" && ! -e "$first/$binary" ]]; then
        expected="$(awk -v name="$binary" '$2 ~ ("/" name "$") { print $1 }' "$first/executable-sha256.txt")"
        actual="$(sha256sum "$build/$binary" | cut -d ' ' -f 1)"
        [[ "$actual" == "$expected" ]] || { echo 'first-round ELF changed before evidence preservation'; exit 3; }
        cp --update=none "$build/$binary" "$first/$binary"
        printf 'FROZEN_FIRST_ROUND_ELF %s sha=%s\n' "$binary" "$actual"
    fi
done
[[ "$(du -sb "$owner" | cut -f 1)" -lt 134217728 ]] || exit 3
capacity build
printf 'cmake --build %q --target posix_cleanup_controlled --parallel 1 --verbose\n' "$build" > "$evidence/build.command.txt"
set +e
cmake --build "$build" --target posix_cleanup_controlled --parallel 1 --verbose > "$evidence/build.log" 2>&1
code=$?
set -e
printf 'exit=%s\n' "$code" > "$evidence/build.exit.txt"
[[ "$code" -eq 0 ]] || { cat "$evidence/build.log"; exit "$code"; }
cp --update=none "$build/compile_commands.json" "$evidence/compile_commands.json"
sha256sum "$build/posix_cleanup_controlled" > "$evidence/executable-sha256.txt"
cp --update=none "$build/posix_cleanup_controlled" "$evidence/posix_cleanup_controlled"
mkdir "$evidence/test-inputs"
for name in acceptance.cpp fixtures.hpp CMakeLists.txt run.sh host.sh verify_snapshot.py reproduce.sh capacity.ps1 verify_fix1.ps1 test_verify_snapshot.py; do
    cp --update=none "$root/tests/native-posix-cleanup/$name" "$evidence/test-inputs/$name"
done
"${verify[@]}" > "$evidence/inputs-pretest.txt"
failed=0
for ((i=1; i<=repeats; ++i)); do
    capacity "repro-$i"
    printf '%q --case quarantine-same-inode-content-change-restored-mtime-conflict\n' "$build/posix_cleanup_controlled" > "$evidence/case-$i.command.txt"
    set +e
    "$build/posix_cleanup_controlled" --case quarantine-same-inode-content-change-restored-mtime-conflict > "$evidence/case-$i.log" 2>&1
    code=$?
    set -e
    printf 'exit=%s\n' "$code" > "$evidence/case-$i.exit.txt"
    cat "$evidence/case-$i.log"
    [[ "$code" -eq 0 ]] || failed=$((failed+1))
done
"${verify[@]}" > "$evidence/inputs-after.txt"
printf 'MINIMAL_REPRO_SUMMARY attempts=%s safety_failures=%s no_sleep=true production_source_unchanged=true\n' "$repeats" "$failed"
[[ "$(du -sb "$owner" | cut -f 1)" -lt 134217728 ]] || exit 3
[[ "$failed" -eq 0 ]]
