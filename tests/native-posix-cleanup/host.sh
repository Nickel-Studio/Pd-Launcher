#!/usr/bin/env bash
set -euo pipefail

root="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/../..")"
owner="$root/.snow/posix-cleanup-acceptance"
[[ -f "$owner/owner.json" ]] || { echo 'missing owner marker' >&2; exit 2; }
[[ -d "$owner/tmp" && -d "$owner/evidence" ]] || exit 2
export TEMP="$owner/tmp" TMP="$owner/tmp" TMPDIR="$owner/tmp"

printf '%s\n' '=== POSIX CLEANUP ACCEPTANCE HOST ==='
date -u +%FT%TZ
printf 'worktree=%s\nowner=%s\nTEMP=%s\nTMP=%s\nTMPDIR=%s\n' "$root" "$owner" "$TEMP" "$TMP" "$TMPDIR"
printf '%s\n' '=== OS AND KERNEL ==='
cat /etc/os-release
uname -a
id
printf '%s\n' '=== COMPILER AND TOOLS ==='
g++ --version
g++ -dumpmachine
g++ -dumpfullversion
cmake --version | head -n 1
ninja --version
python3 --version
printf '%s\n' '=== ACTUAL FILESYSTEMS ==='
findmnt -T "$root"
findmnt -T "$TMPDIR"
findmnt -T /dev/shm
df -B1 "$root" /dev/shm
printf 'fixture_filesystem=%s\n' "$(findmnt -n -o FSTYPE -T /dev/shm)"
printf 'name_max=%s\npath_max=%s\n' "$(getconf NAME_MAX /dev/shm)" "$(getconf PATH_MAX /dev/shm)"
printf '%s\n' '=== LIMITS ==='
ulimit -a
printf 'owner_bytes=%s\n' "$(du -sb "$owner" | cut -f 1)"
printf 'owner_build_limit=134217728\nfixture_limit=1048576\n'
[[ "$(g++ -dumpfullversion)" == 13.3.0 ]] || { echo 'GCC 13.3.0 required' >&2; exit 3; }
[[ "$(findmnt -n -o FSTYPE -T /dev/shm)" == tmpfs ]] || { echo 'tmpfs fixtures required' >&2; exit 3; }
grep -q '^VERSION_ID="24.04"$' /etc/os-release || { echo 'Ubuntu 24.04 required' >&2; exit 3; }
grep -q 'microsoft-standard-WSL2' /proc/sys/kernel/osrelease || { echo 'real WSL2 kernel required' >&2; exit 3; }
printf '%s\n' 'HOST_PREFLIGHT_PASS'
