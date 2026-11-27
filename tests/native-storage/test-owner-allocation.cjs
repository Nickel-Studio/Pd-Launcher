'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const audit = require('./owner-allocation-audit.cjs');
const { physicalDNoLinks, RESERVE } = require('./storage-audit.cjs');
const { hash } = require('./compression-audit.cjs');

const ownerRoot = 'D:\\synthetic\\transport';
const physicalPath = ownerRoot + '\\closed\\sample.obj';
const volume = '\\\\?\\Volume{12345678-1234-1234-1234-123456789abc}\\';
const digest = 'a'.repeat(64);
const bindings = { planSha256: 'b'.repeat(64), rowsSha256: 'c'.repeat(64), summarySha256: 'd'.repeat(64) };
const results = [];

function fixtures() {
  const before = { allocatedBytes: 8192, allocatedClusters: 2, compressionFormat: 0,
    compressionStorageBytes: 7000, fileId: '01'.repeat(16), links: 1, logicalBytes: 7000,
    retrievalExtentCount: 1, sha256: digest, standardAllocationBytes: 8192,
    volumeSerial: '15720999219562370985' };
  const after = { ...before, allocatedBytes: 4096, allocatedClusters: 1, compressionFormat: 2,
    compressionStorageBytes: 4000, standardAllocationBytes: 4096 };
  const plan = { schemaVersion: 1, owner: 'transport', tag: 'synthetic-closed', sourceCommit: 'e'.repeat(40),
    sourceCompletionSha256: digest, allocationMethod: audit.CLUSTER_METHOD, uncompressedOnly: true,
    deletions: false, directoryCompression: false, createdUtc: '2026-11-21T19:15:01.000Z',
    volume: { fileSystem: 'NTFS', physicalRoot: 'D:\\', clusterBytes: 4096, volumeName: volume },
    explicitAllowlist: [{ before, physicalPath, path: 'closed/sample.obj', exclusiveReadWriteHandle: true, shareMode: 0 }] };
  const rows = [{ before: { ...before }, after, physicalPath, path: 'closed/sample.obj', allocatedBytesReleased: 4096,
    exclusiveReadWriteHandle: true, shareMode: 0, setCompressionSucceeded: true, nativeError: 0,
    byteExact: true, identityUnchanged: true, startedUtc: '2026-11-21T19:15:02.000Z', completedUtc: '2026-11-21T19:15:03.000Z' }];
  const done = { schemaVersion: 1, owner: 'transport', tag: plan.tag, sourceCommit: plan.sourceCommit,
    sourceCompletionSha256: digest, allocationMethod: audit.CLUSTER_METHOD,
    manifest: { path: ownerRoot + '\\closed\\plan.json', sha256: bindings.planSha256 },
    perFileReceipts: { path: ownerRoot + '\\closed\\pairs.jsonl', sha256: bindings.rowsSha256 },
    directoryCompression: false, filesMovedOrDeleted: false, sharedPrefixModified: false, exclusiveHandlesClosed: true,
    allFileIdsUnchanged: true, allOriginalPathsRetained: true, allSha256Unchanged: true, frozenSourcesZipsLogsUnchanged: true,
    volume: { ...plan.volume }, successFileCount: 1, completedUtc: '2026-11-21T19:15:04.000Z',
    allocatedBytesBefore: 8192, allocatedBytesAfter: 4096, allocatedBytesReleased: 4096,
    logicalBytesBefore: 7000, logicalBytesAfter: 7000 };
  return { plan, rows, done };
}

function runReview(f) {
  return audit.reviewTransport(f.plan, f.rows, f.done, ownerRoot, bindings);
}

function rejects(f, code) {
  assert.throws(() => runReview(f), error => error.code === code);
}

function test(name, run) {
  try { run(); results.push({ name, status: 'PASS' }); }
  catch (error) { results.push({ name, status: 'FAIL', detail: error.message }); }
}

test('paired native clusters, not logical bytes, produce historical owner measurements', () => {
  const r = runReview(fixtures());
  assert.equal(r.ownerObservedNativeClusterReleaseBytes, 4096);
  assert.equal(r.productionAdmission, false);
  assert.equal(r.reclaimCreditedToCapacityBytes, 0);
  assert.equal(r.completeCrossOwnerTotalReleasedBytes, null);
  assert.equal(r.currentLeaseAbsenceProven, false);
  assert.equal(r.foreignArtifactReads, 0);
});

test('source owner, full digests and generation must bind', () => {
  let f = fixtures(); f.done.owner = 'foreign'; rejects(f, 'OWNER_BINDING');
  f = fixtures(); f.done.manifest.sha256 = 'f'.repeat(64); rejects(f, 'RECEIPT_BINDING');
  f = fixtures(); f.done.sourceCompletionSha256 = 'f'.repeat(64); rejects(f, 'OWNER_BINDING');
  f = fixtures(); f.plan.sourceCommit = 123; f.done.sourceCommit = 123; rejects(f, 'OWNER_BINDING');
  f = fixtures(); f.plan.sourceCommit = 'g'.repeat(40); f.done.sourceCommit = f.plan.sourceCommit; rejects(f, 'OWNER_BINDING');
});

test('unqualified native size and FileStandard labels are not paired clusters', () => {
  for (const method of ['GetCompressedFileSizeW', 'FileCompressionInfo.CompressedFileSize', 'FileStandardInfo.AllocationSize', 'logical-length']) {
    const f = fixtures(); f.plan.allocationMethod = method; f.done.allocationMethod = method;
    rejects(f, 'NATIVE_CLUSTER_METHOD');
  }
});

test('cluster geometry and both cluster counters must agree', () => {
  let f = fixtures(); f.rows[0].after.allocatedClusters = 2; rejects(f, 'CLUSTER_COUNTER_MISMATCH');
  f = fixtures(); f.done.volume.clusterBytes = 8192; rejects(f, 'CLUSTER_GEOMETRY');
  f = fixtures(); f.rows[0].after.retrievalExtentCount = 0; rejects(f, 'CLUSTER_COUNTER_MISMATCH');
});

test('foreign/C-backed paths and unknown volume cannot pass', () => {
  let f = fixtures(); f.plan.explicitAllowlist[0].physicalPath = 'C:\\synthetic\\sample.obj'; rejects(f, 'FILE_SCOPE');
  f = fixtures(); f.done.volume.volumeName = 'D:\\'; rejects(f, 'PHYSICAL_VOLUME_UNKNOWN');
  f = fixtures(); f.done.manifest.path = 'D:\\foreign\\plan.json'; rejects(f, 'OWNER_ROOT_UNKNOWN');
});

test('exact coverage, unique physical identity and paths are mandatory', () => {
  let f = fixtures(); f.rows = []; rejects(f, 'ENTRY_COVERAGE');
  f = fixtures(); f.plan.explicitAllowlist.push({ ...f.plan.explicitAllowlist[0], physicalPath: ownerRoot + '\\closed\\other.obj' });
  f.rows.push({ ...f.rows[0], physicalPath: ownerRoot + '\\closed\\other.obj' }); f.done.successFileCount = 2;
  rejects(f, 'PHYSICAL_DUPLICATE');
  f = fixtures(); f.rows[0].path = 'closed/changed.obj'; rejects(f, 'ENTRY_COVERAGE');
});

test('changed byte hashes, links, identity or baseline cannot pass', () => {
  let f = fixtures(); f.rows[0].after.sha256 = 'f'.repeat(64); rejects(f, 'CONTENT_CHANGED');
  f = fixtures(); f.rows[0].after.fileId = '02'.repeat(16); rejects(f, 'IDENTITY_CHANGED');
  f = fixtures(); f.rows[0].after.links = 2; rejects(f, 'FILE_STATE_UNKNOWN');
  f = fixtures(); f.rows[0].before.allocatedBytes = 4096; rejects(f, 'BEFORE_BINDING');
});

test('closed retained exclusive window is historical, never a current lease', () => {
  let f = fixtures(); f.rows[0].shareMode = 1; rejects(f, 'EXCLUSIVE_HANDLE_UNKNOWN');
  f = fixtures(); f.rows[0].completedUtc = '2026-11-21T19:16:00Z'; rejects(f, 'CLOSED_WINDOW');
  f = fixtures(); f.done.filesMovedOrDeleted = true; rejects(f, 'PRESERVATION_UNKNOWN');
  assert.equal(runReview(fixtures()).currentLeaseAbsenceProven, false);
});

test('unsafe native serial JSON token stays exact, rounded numbers fail', () => {
  const doc = audit.parseJson('{"volumeSerial":15720999219562370985,"bytes":9007199254740992}');
  assert.equal(doc.volumeSerial, '15720999219562370985');
  assert.equal(doc.bytes, '9007199254740992');
  const f = fixtures(); f.rows[0].after.volumeSerial = Number(doc.volumeSerial); rejects(f, 'SERIAL_PRECISION');
});

test('original unsigned FILE_ID_128 and volume GUID are string identity keys', () => {
  assert.equal(audit.identity(volume.toUpperCase(), 'AA'.repeat(16)), audit.identity(volume, 'aa'.repeat(16)));
  assert.notEqual(audit.identity(volume, '01'.repeat(16)), audit.identity(volume, '02'.repeat(16)));
  assert.throws(() => audit.identity('D:\\', '01'.repeat(16)), error => error.code === 'PHYSICAL_VOLUME_UNKNOWN');
});

test('allocation increases remain negative instead of reclaimed credit', () => {
  const f = fixtures(); f.rows[0].after.allocatedBytes = 12288; f.rows[0].after.allocatedClusters = 3;
  f.rows[0].allocatedBytesReleased = -4096; f.done.allocatedBytesAfter = 12288; f.done.allocatedBytesReleased = -4096;
  const r = runReview(f); assert.equal(r.ownerObservedNativeClusterReleaseBytes, -4096);
  assert.equal(r.reclaimCreditedToCapacityBytes, 0);
});

test('summary totals and counters cannot substitute logical EOF', () => {
  let f = fixtures(); f.done.allocatedBytesBefore = 7000; rejects(f, 'SUMMARY_BINDING');
  f = fixtures(); f.rows[0].after.allocatedBytes = 4000; rejects(f, 'CLUSTER_COUNTER_MISMATCH');
  f = fixtures(); f.rows[0].after.allocatedClusters = Number.MAX_SAFE_INTEGER; rejects(f, 'CLUSTER_COUNTER_MISMATCH');
});

test('local raw SHA, locked publication, path boundary and CreateNew fixtures', () => {
  if (process.platform !== 'win32') return;
  physicalDNoLinks(__dirname, __dirname, false);
  const relative = 'fixtures/owner-allocation-' + process.pid + '.json';
  const destination = path.resolve(__dirname, relative);
  physicalDNoLinks(__dirname, path.dirname(destination), false);
  const raw = Buffer.from('{"receipt":"synthetic-small"}\n');
  fs.writeFileSync(destination, raw, { flag: 'wx' });
  assert.deepEqual(audit.readRaw(relative, hash(raw)), raw);
  assert.throws(() => audit.readRaw(relative, digest), error => error.code === 'RECEIPT_HASH_MISMATCH');
  assert.throws(() => audit.readRaw('../outside.json', digest), error => error.code === 'PATH_BOUNDARY');
  const original = fs.lstatSync;
  try {
    fs.lstatSync = function (file, options) {
      if (path.basename(String(file)) === '.publisher.lock') return { present: true };
      return original.call(this, file, options);
    };
    assert.throws(() => audit.readRaw(relative, hash(raw)), error => error.code === 'PUBLISHER_LOCK');
  } finally { fs.lstatSync = original; }
});

test('execution modes and reserve mutations are not part of this reviewer', () => {
  for (const mode of ['compact', 'cleanup', 'move', 'build', 'reserve', 'kill']) {
    assert.throws(() => audit.command([mode]), error => error.code === 'OPERATION_REFUSED');
  }
  assert.equal(RESERVE.incremental, 1073741824);
  assert.equal(RESERVE.game, 4294967296);
});

test('baseline object key order does not change its bound native evidence', () => {
  const f = fixtures();
  f.rows[0].before = Object.fromEntries(Object.entries(f.rows[0].before).reverse());
  assert.equal(runReview(f).ownerObservedNativeClusterReleaseBytes, 4096);
});

const name = process.argv[2];
assert.ok(typeof name === 'string' && /^results-owner-allocation-[a-z0-9-]+\.json$/i.test(name), 'Unused result filename required');
physicalDNoLinks(__dirname, __dirname, false);
const destination = path.resolve(__dirname, name);
assert.ok(!fs.existsSync(destination), 'CreateNew only; preserve old results');
const report = { schemaVersion: 1, suite: 'owner-native-cluster-receipt-review', capturedAt: new Date().toISOString(),
  syntheticData: true, actualCompressionExecuted: false, foreignArtifactReads: 0, productionAdmission: false,
  pass: results.filter(row => row.status === 'PASS').length, fail: results.filter(row => row.status === 'FAIL').length, results };
fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n', { flag: 'wx' });
process.stdout.write(JSON.stringify({ path: name, pass: report.pass, fail: report.fail }) + '\n');
if (report.fail) process.exitCode = 1;
