'use strict';

const fs = require('node:fs');
const path = require('node:path');
const child = require('node:child_process');
const crypto = require('node:crypto');

const root = path.resolve(__dirname, '../..');
const seeds = [
  'native/core/config/atomic_file.cpp', 'native/core/config/path_guard.cpp',
  'native/core/config/detail.cpp', 'native/core/config/crypto.cpp',
  'native/core/tasks/secure_fs.cpp', 'native/core/network/hash.cpp',
  'native/core/network/control.cpp', 'native/core/config/path_guard.hpp',
  'native/core/tasks/test_hooks.hpp', 'native/core/common/error.hpp',
  'scripts/native/msvc.py'
];

function digest(bytes) {
  return crypto.createHash('sha256').update(bytes).digest('hex');
}

function ownedPath(relative) {
  if (typeof relative !== 'string' || !relative || relative.includes('\\') ||
      path.posix.isAbsolute(relative) || relative.split('/').some(part => !part || part === '.' || part === '..')) {
    throw new Error('Invalid snapshot path');
  }
  const result = path.resolve(root, relative);
  const local = path.relative(root, result);
  if (local === '..' || local.startsWith(`..${path.sep}`) || path.isAbsolute(local)) throw new Error('Snapshot escapes the worktree');
  let parent = root;
  for (const part of local.split(path.sep)) {
    parent = path.join(parent, part);
    if (fs.existsSync(parent) && fs.lstatSync(parent).isSymbolicLink()) throw new Error('Snapshot path contains a link');
  }
  return result;
}

function capacity(bytes = 0) {
  const disk = fs.statfsSync(root, { bigint: true });
  const free = disk.bavail * disk.bsize;
  if (free < 256n * 1024n * 1024n + BigInt(bytes)) throw new Error('beforewrite: insufficient worktree volume reserve');
  if (process.platform === 'win32') {
    if (path.parse(root).root.toLowerCase() === 'c:\\') throw new Error('beforewrite: refusing worktree writes on C');
    const system = fs.statfsSync('C:/', { bigint: true });
    if (system.bavail * system.bsize < 1024n * 1024n * 1024n) throw new Error('beforewrite: C reserve is below 1 GiB');
  }
  return free.toString();
}

function git(args) {
  return child.execFileSync('git', args, { cwd: root, maxBuffer: 16 * 1024 * 1024, windowsHide: true });
}

function tree(commit) {
  if (!/^[0-9a-f]{40}$/.test(commit)) throw new Error('A complete immutable Git commit is required');
  if (git(['rev-parse', `${commit}^{commit}`]).toString('utf8').trim() !== commit) throw new Error('Git commit identity mismatch');
  const files = new Map();
  for (const entry of git(['ls-tree', '-r', '-z', commit]).toString('utf8').split('\0')) {
    if (!entry) continue;
    const match = /^(\d+) blob ([0-9a-f]{40})\t(.+)$/.exec(entry);
    if (match) files.set(match[3], { mode: match[1], blob: match[2] });
  }
  return files;
}

function verifySnapshot(directory) {
  const relative = path.relative(root, path.resolve(directory)).split(path.sep).join('/');
  const shared = ownedPath(relative);
  const manifestBytes = fs.readFileSync(path.join(shared, 'snapshot.json'));
  const manifest = JSON.parse(manifestBytes);
  const marker = JSON.parse(fs.readFileSync(path.join(shared, 'snapshot-complete.json')));
  const manifestSha256 = digest(manifestBytes);
  if (manifest.format !== 'palladium-cleanup-git-inputs-v1' || marker.format !== 'palladium-cleanup-snapshot-complete-v1' ||
      marker.manifestSha256 !== manifestSha256 || marker.sourceCommit !== manifest.sourceCommit || marker.complete !== true) {
    throw new Error('Incomplete or mismatched dependency publication');
  }
  const objects = tree(manifest.sourceCommit);
  let count = 0;
  let totalBytes = 0;
  for (const [relativeFile, expected] of Object.entries(manifest.files)) {
    const file = ownedPath(`${relative}/${relativeFile}`);
    const object = objects.get(relativeFile);
    const data = fs.readFileSync(file);
    if (!object || object.mode !== '100644' || object.blob !== expected.gitBlob ||
        data.length !== expected.bytes || digest(data) !== expected.sha256 ||
        digest(git(['cat-file', 'blob', object.blob])) !== expected.sha256) {
      throw new Error(`Dependency provenance mismatch: ${relativeFile}`);
    }
    totalBytes += data.length;
    count++;
  }
  if (!count || count !== marker.filesMatched || totalBytes !== marker.totalBytes) throw new Error('Dependency publication count mismatch');
  return { shared, manifestSha256, sourceCommit: manifest.sourceCommit, filesMatched: count, totalBytes, files: manifest.files };
}

function capture(commit) {
  const objects = tree(commit);
  const relative = `.snow/palladium-cleanup/dependencies/${commit}`;
  const destination = ownedPath(relative);
  if (fs.existsSync(destination)) return verifySnapshot(destination);
  const pending = [...seeds];
  const captured = new Map();
  while (pending.length) {
    const file = pending.shift();
    if (captured.has(file)) continue;
    const object = objects.get(file);
    if (!object || object.mode !== '100644') throw new Error(`Missing regular Git dependency: ${file}`);
    const data = git(['cat-file', 'blob', object.blob]);
    captured.set(file, { data, gitBlob: object.blob });
    for (const match of data.toString('utf8').matchAll(/^\s*#\s*include\s*"([^"]+)"/gm)) {
      const beside = path.posix.normalize(path.posix.join(path.posix.dirname(file), match[1]));
      const native = path.posix.normalize(`native/${match[1]}`);
      const resolved = objects.has(beside) ? beside : objects.has(native) ? native : undefined;
      if (resolved && !captured.has(resolved)) pending.push(resolved);
      else if (!resolved) throw new Error(`Unresolved quoted include in ${file}: ${match[1]}`);
    }
  }
  const totalBytes = [...captured.values()].reduce((sum, value) => sum + value.data.length, 0);
  capacity(totalBytes + 64 * 1024);
  fs.mkdirSync(destination, { recursive: true });
  const files = {};
  for (const [file, value] of [...captured.entries()].sort(([a], [b]) => a.localeCompare(b))) {
    const output = ownedPath(`${relative}/${file}`);
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.writeFileSync(output, value.data, { flag: 'wx' });
    files[file] = { bytes: value.data.length, sha256: digest(value.data), gitBlob: value.gitBlob };
  }
  const manifest = { format: 'palladium-cleanup-git-inputs-v1', product: 'Palladium',
    sourceCommit: commit, capturedAt: new Date().toISOString(), scope: 'Minimal immutable production cleanup dependency closure; not product acceptance', files };
  const manifestBytes = Buffer.from(`${JSON.stringify(manifest, null, 2)}\n`);
  fs.writeFileSync(path.join(destination, 'snapshot.json'), manifestBytes, { flag: 'wx' });
  fs.writeFileSync(path.join(destination, 'snapshot-complete.json'), `${JSON.stringify({
    format: 'palladium-cleanup-snapshot-complete-v1', complete: true, sourceCommit: commit,
    manifestSha256: digest(manifestBytes), filesMatched: captured.size, totalBytes
  }, null, 2)}\n`, { flag: 'wx' });
  return verifySnapshot(destination);
}

module.exports = { capture, verifySnapshot, capacity, digest, root };

if (require.main === module) {
  try {
    if (process.argv.length !== 3) throw new Error('Usage: node tests/native-cleanup/snapshot.cjs <full-git-commit>');
    const result = capture(process.argv[2]);
    console.log(JSON.stringify({ snapshot: path.relative(root, result.shared).split(path.sep).join('/'),
      sourceCommit: result.sourceCommit, manifestSha256: result.manifestSha256,
      filesMatched: result.filesMatched, totalBytes: result.totalBytes }, null, 2));
  } catch (error) {
    console.error(String(error));
    process.exitCode = 1;
  }
}
