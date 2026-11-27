'use strict';

// Asset extraction/check tooling only; no Node.js code ships in nickel_identity.
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const crypto = require('node:crypto');
const root = path.resolve(__dirname, '../../..');
const output = path.join(root, 'native/core/media/builtin_heads.hpp');
const assets = [
    { name: 'steve', sha256: '9b6bb76a9c787c33504cab6ef0688a4d2abb24f38b93f73dd414f3d603dbe38c' },
    { name: 'alex', sha256: '95496b5cd6749febb0f5d676e9375fb7500c60b1d510b53cb541429f2fb9d286' }
];

function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
        crc ^= byte;
        for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
}

function paeth(left, above, corner) {
    const prediction = left + above - corner;
    const a = Math.abs(prediction - left), b = Math.abs(prediction - above), c = Math.abs(prediction - corner);
    return a <= b && a <= c ? left : b <= c ? above : corner;
}

function decodeAsset(asset) {
    const input = fs.readFileSync(path.join(root, `src/assets/skins/${asset.name}-head.png`));
    const digest = crypto.createHash('sha256').update(input).digest('hex');
    if (digest !== asset.sha256 || !input.subarray(0, 8).equals(Buffer.from([137,80,78,71,13,10,26,10])))
        throw new Error(`Original ${asset.name} asset differs from the frozen functional asset`);
    let width = 0, height = 0, ended = false;
    const compressed = [];
    for (let offset = 8; offset < input.length;) {
        if (offset + 12 > input.length) throw new Error('Truncated PNG chunk');
        const length = input.readUInt32BE(offset);
        const end = offset + length + 12;
        if (end > input.length) throw new Error('PNG chunk exceeds asset');
        const type = input.toString('ascii', offset + 4, offset + 8);
        const chunk = input.subarray(offset + 8, end - 4);
        if (crc32(input.subarray(offset + 4, end - 4)) !== input.readUInt32BE(end - 4))
            throw new Error('PNG chunk CRC mismatch');
        if (type === 'IHDR') {
            if (width || offset !== 8 || length !== 13 || chunk[8] !== 8 || chunk[9] !== 6 ||
                chunk[10] !== 0 || chunk[11] !== 0 || chunk[12] !== 0)
                throw new Error('Extractor accepts only the frozen single-frame RGBA8 assets');
            width = chunk.readUInt32BE(0);
            height = chunk.readUInt32BE(4);
            if (width !== 256 || height !== 256) throw new Error('Unexpected frozen head dimensions');
        } else if (type === 'IDAT') {
            if (!width) throw new Error('IDAT before IHDR');
            compressed.push(chunk);
        } else if (type === 'IEND') {
            if (length || end !== input.length) throw new Error('Invalid PNG end');
            ended = true;
        } else if (type === 'acTL') {
            throw new Error('Animated builtin heads are not permitted');
        }
        offset = end;
    }
    if (!ended || !compressed.length) throw new Error('Missing PNG image data');
    const stride = width * 4;
    const filtered = zlib.inflateSync(Buffer.concat(compressed), { maxOutputLength: (stride + 1) * height });
    if (filtered.length !== (stride + 1) * height) throw new Error('PNG scanline size mismatch');
    const rgba = Buffer.alloc(stride * height);
    for (let y = 0; y < height; ++y) {
        const filter = filtered[y * (stride + 1)];
        if (filter > 4) throw new Error('Invalid PNG filter');
        for (let x = 0; x < stride; ++x) {
            const left = x >= 4 ? rgba[y * stride + x - 4] : 0;
            const above = y ? rgba[(y - 1) * stride + x] : 0;
            const corner = y && x >= 4 ? rgba[(y - 1) * stride + x - 4] : 0;
            const predictor = [0, left, above, Math.floor((left + above) / 2), paeth(left, above, corner)][filter];
            rgba[y * stride + x] = (filtered[y * (stride + 1) + x + 1] + predictor) & 255;
        }
    }
    const runs = [];
    for (let offset = 0; offset < rgba.length;) {
        let count = 1;
        while (offset + count * 4 < rgba.length && rgba.subarray(offset, offset + 4).equals(rgba.subarray(offset + count * 4, offset + count * 4 + 4))) ++count;
        runs.push({ count, color: Array.from(rgba.subarray(offset, offset + 4)) });
        offset += count * 4;
    }
    const reconstructed = Buffer.alloc(rgba.length);
    let cursor = 0;
    for (const run of runs) for (let i = 0; i < run.count; ++i) {
        reconstructed.set(run.color, cursor);
        cursor += 4;
    }
    if (!reconstructed.equals(rgba)) throw new Error('Lossless asset encoding failed');
    const md5 = crypto.createHash('md5').update(rgba).digest('hex');
    const rgbaSha = crypto.createHash('sha256').update(rgba).digest('hex');
    console.log(`${asset.name}: ${width}x${height}; runs=${runs.length}; RGBA MD5=${md5}; RGBA SHA256=${rgbaSha}`);
    return { ...asset, width, height, runs, md5, rgbaSha };
}

const decoded = assets.map(decodeAsset);
let header = '#pragma once\n\n// Generated losslessly from the original functional PNG assets. Do not redraw.\n';
header += '// Regenerate/check: node native/tests/identity/generate_builtin_heads.cjs [--check]\n';
header += '#include <array>\n#include <cstdint>\n\nnamespace nickel::media::detail {\n';
header += 'struct BuiltinHeadRun {\n    std::uint32_t count;\n    std::array<std::uint8_t, 4> rgba;\n};\n';
for (const asset of decoded) {
    header += `\n// src/assets/skins/${asset.name}-head.png SHA256: ${asset.sha256}\n`;
    header += `// Original unpremultiplied RGBA8888 SHA256: ${asset.rgbaSha}\n`;
    header += `inline constexpr std::uint32_t ${asset.name}_head_width = ${asset.width};\n`;
    header += `inline constexpr std::uint32_t ${asset.name}_head_height = ${asset.height};\n`;
    header += `inline constexpr std::array<BuiltinHeadRun, ${asset.runs.length}> ${asset.name}_head_runs{{\n`;
    for (const run of asset.runs) header += `    {${run.count}, {${run.color.join(', ')}}},\n`;
    header += '}};\n';
}
header += '\n} // namespace nickel::media::detail\n';
if (process.argv.includes('--check')) {
    // The header is a generated text artifact. Its canonical git blob is LF, but a Windows
    // checkout with core.autocrlf=true materializes CRLF. Normalize line terminators only;
    // every remaining byte (hash comments, run counts, RGBA values, layout) must match exactly.
    const checked = fs.readFileSync(output, 'utf8').replace(/\r\n/g, '\n');
    if (checked !== header) throw new Error('Builtin head data does not match original asset pixels');
    console.log('Native builtin head data matches both original PNGs byte-for-byte after decode');
} else {
    fs.writeFileSync(output, header);
    console.log(`Wrote ${path.relative(root, output)}`);
}
