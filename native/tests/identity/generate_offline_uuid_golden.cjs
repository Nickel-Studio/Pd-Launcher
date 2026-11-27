'use strict';

// Test-only old-source port. No output files, compiler, downloads, or runtime dependencies are created.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const commit = 'aa7402ecdce1b4beb60ca2652a5f454d53585ca0';
const sourcePath = 'native/tests/identity/offline_uuid_legacy_oracle.rs';
const blob = '2710485d7eff2529e11c3067d5e0f3dace1b7c64';
const source = Buffer.from(fs.readFileSync(path.join(__dirname, 'offline_uuid_legacy_oracle.rs'), 'utf8').replace(/\r\n/g, '\n'));
const gitBlob = crypto.createHash('sha1').update(`blob ${source.length}\0`).update(source).digest('hex');
assert.equal(gitBlob, blob, 'Frozen test oracle canonical LF bytes must match the immutable Git source blob');
const rust = source.toString('utf8');
const constant = expression => {
    const match = rust.match(expression);
    assert.ok(match, 'Expected immutable Rust constant');
    return BigInt(`0x${match[1]}`);
};
const offsetA = constant(/let mut a = 0x([0-9a-f]+)u64;/);
const offsetB = constant(/let mut b = 0x([0-9a-f]+)u64;/);
const primeA = constant(/a = a\.wrapping_mul\(0x([0-9a-f]+)\);/);
const primeB = constant(/b = b\.wrapping_mul\(0x([0-9a-f]+)\);/);
assert.ok(rust.includes('for byte in username.bytes() {') && rust.includes('a ^= byte as u64;') && rust.includes('b ^= a;'));
assert.ok(rust.includes('"{:08x}-{:04x}-3{:03x}-{:04x}-{:012x}"') && rust.includes('a & 0x0000_ffff_ffff_ffff'));
const hex = (value, width) => value.toString(16).padStart(width, '0');
function oldSourcePort(bytes) {
    let a = offsetA, b = offsetB;
    for (const byte of bytes) {
        a = BigInt.asUintN(64, (a ^ BigInt(byte)) * primeA);
        b = BigInt.asUintN(64, (b ^ a) * primeB);
    }
    return `${hex(b >> 32n, 8)}-${hex((b >> 16n) & 0xffffn, 4)}-3${hex(b & 0xfffn, 3)}-${hex(((a >> 48n) & 0x3fffn) | 0x8000n, 4)}-${hex(a & 0xffffffffffffn, 12)}`;
}
// Independent 16-bit arithmetic and field formatting guard the BigInt port against transcription errors.
const words = value => Array.from({ length: 4 }, (_, i) => Number((value >> BigInt(i * 16)) & 0xffffn));
function multiply(left, right) {
    const result = [];
    let carry = 0;
    for (let i = 0; i < 4; ++i) {
        let sum = carry;
        for (let j = 0; j <= i; ++j) sum += left[j] * right[i - j];
        result.push(sum % 65536);
        carry = Math.floor(sum / 65536);
    }
    return result;
}
function limbCrosscheck(bytes) {
    let a = words(offsetA), b = words(offsetB);
    for (const byte of bytes) {
        a[0] ^= byte;
        a = multiply(a, words(primeA));
        b = multiply(b.map((word, i) => word ^ a[i]), words(primeB));
    }
    return `${hex(b[3], 4)}${hex(b[2], 4)}-${hex(b[1], 4)}-3${hex(b[0] & 0xfff, 3)}-${hex((a[3] & 0x3fff) | 0x8000, 4)}-${hex(a[2], 4)}${hex(a[1], 4)}${hex(a[0], 4)}`;
}
const repeat = (text, count, suffix = '') => text.repeat(count) + suffix;
const cases = [
    ['empty-raw', '', '""'],
    ['Notch', 'Notch', '"Notch"'],
    ['notch', 'notch', '"notch"'],
    ['NOTCH', 'NOTCH', '"NOTCH"'],
    ['Steve', 'Steve', '"Steve"'],
    ['Player', 'Player', '"Player"'],
    ['chinese-player', '\u73a9\u5bb6', '"\\xe7\\x8e\\xa9\\xe5\\xae\\xb6"'],
    ['mixed-chinese', 'A\u73a9\u5bb6z', '"A\\xe7\\x8e\\xa9\\xe5\\xae\\xb6z"'],
    ['raw-padding', ' Notch ', '" Notch "'],
    ['raw-whitespace', ' \t\n', '" \\t\\n"'],
    ['ascii-255', repeat('a', 255), "std::string(255, 'a')"],
    ['ascii-256', repeat('a', 256), "std::string(256, 'a')"],
    ['utf8-255', repeat('\u73a9', 85), 'repeat_offline_uuid_name("\\xe7\\x8e\\xa9", 85)'],
    ['utf8-256', repeat('\u73a9', 85, 'a'), 'repeat_offline_uuid_name("\\xe7\\x8e\\xa9", 85) + "a"'],
    ['four-byte-256', repeat('\u{1f600}', 64), 'repeat_offline_uuid_name("\\xf0\\x9f\\x98\\x80", 64)'],
];
const vectors = cases.map(([label, name, cpp]) => {
    const bytes = Buffer.from(name, 'utf8');
    const uuid = oldSourcePort(bytes);
    assert.equal(uuid, limbCrosscheck(bytes), label);
    assert.match(uuid, /^[0-9a-f]{8}-[0-9a-f]{4}-3[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    return { label, utf8Hex: bytes.toString('hex'), byteLength: bytes.length, uuid, cpp };
});
assert.equal(vectors.find(row => row.label === 'Notch').uuid, 'f14dc048-28d2-3bc4-803c-36d82b6053b9');
const golden = {
    schema: 1,
    gate: 'G03',
    oracle: { commit, path: sourcePath, gitBlob: blob, byteLength: source.length, sha256: crypto.createHash('sha256').update(source).digest('hex') },
    evidence: 'immutable-old-Rust-source port, crosschecked with independent 16-bit arithmetic; not Rust or native execution',
    rustRuntime: 'NOT_RUN',
    nativeRuntime: 'NOT_RUN',
    contract: 'lowercase dashed36; exact UTF-8 bytes; case-sensitive; no trimming; empty raw allowed; invalid UTF-8, embedded NUL, or more than 256 bytes rejected by native helper',
    vectors: vectors.map(({ label, utf8Hex, byteLength, uuid }) => {
        const repeated = label.startsWith('ascii-') ? { unitHex: '61', count: byteLength } :
            label.startsWith('utf8-') ? { unitHex: 'e78ea9', count: 85, suffixHex: byteLength === 256 ? '61' : '' } :
            label === 'four-byte-256' ? { unitHex: 'f09f9880', count: 64 } : null;
        return { label, ...(repeated ? { utf8Repeat: repeated } : { utf8Hex }), byteLength, uuid };
    }),
};
const json = JSON.stringify(golden, null, 2) + '\n';
const header = `#pragma once

// Generated by generate_offline_uuid_golden.cjs from immutable old Rust source; not runtime evidence.
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace identity_test {
struct OfflineUuidGolden {
    std::string_view label;
    std::string name;
    std::string_view uuid;
};
inline std::string repeat_offline_uuid_name(std::string_view unit, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count; ++i) result += unit;
    return result;
}
inline const std::array<OfflineUuidGolden, ${vectors.length}> offline_uuid_golden{{
${vectors.map(row => `    {"${row.label}", ${row.cpp}, "${row.uuid}"},`).join('\n')}
}};
} // namespace identity_test
`;
const mode = process.argv[2];
assert.ok(process.argv.length === 3 && ['--print', '--check'].includes(mode), 'Use --print or read-only --check');
if (mode === '--print') {
    process.stdout.write(JSON.stringify({ header, golden: json }, null, 2) + '\n');
} else {
    assert.equal(fs.readFileSync(path.join(__dirname, 'offline_uuid_golden.hpp'), 'utf8').replace(/\r\n/g, '\n'), header);
    assert.equal(fs.readFileSync(path.join(__dirname, 'pd-offline-uuid-golden.json'), 'utf8').replace(/\r\n/g, '\n'), json);
    process.stdout.write(`G03 old-source static golden: ${vectors.length}/${vectors.length}; source=${golden.oracle.sha256}; Rust/native NOT_RUN; no files written\n`);
}
