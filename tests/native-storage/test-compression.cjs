'use strict';

const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const audit = require('./compression-audit.cjs');
const { physicalDNoLinks } = require('./storage-audit.cjs');
const now = Date.parse('2026-11-21T18:40:00Z');
const owner = 'fixture-owner';
const sha = 'a'.repeat(64);
const root = 'D:\\owner-closed-fixture';
const results = [];
const clone = value => JSON.parse(JSON.stringify(value));

function plan() {
  return { schemaVersion: 1, kind: audit.KIND, ownerId: owner, leaseEpoch: 'fixture-epoch',
    method: audit.METHOD, ownerCompressionAuthorized: true, exclusionInventoryComplete: true,
    excludedRoots: [root + '\\protected'], inventory: { schemaVersion: 1, ownerId: owner,
      authority: { providedBy: owner, scope: 'owner-attested', foreignRecursiveRead: false },
      leaseEpoch: 'fixture-epoch', readerLeaseInventoryComplete: true, writerInventoryComplete: true,
      sourceOwnedRoots: [root], targetOwnedRoot: root, capturedAt: new Date(now - 1000).toISOString(),
      entries: [{ id: 'obj1', ownerId: owner, leaseEpoch: 'fixture-epoch', closedAttestedBy: owner,
        markedClosed: true, protected: false, kind: 'build', sourcePath: root + '\\old\\one.obj',
        targetPath: root + '\\old\\one.obj', byte: 100, sha256: sha, allocatedBytes: 4096,
        allocatedVolume: 'D', allocationEvidence: 'owner-native-allocated', fileIdentity: 'fixture-file1',
        activeWriterPids: [], readerLeases: [], hardlinkCount: 1,
        sourceIsReparsePoint: false, targetIsReparsePoint: false,
        compressionReview: { authorizedBy: owner, oldGeneratedCopy: true, fileSystem: 'NTFS',
          isRegularFile: true, deletePending: false, encrypted: false, sparse: false,
          inActiveCompileTree: false, failureEvidence: false, sourceMaterial: false, logOrReceipt: false,
          sourcePackage: false, relinkMaterial: false, protectedEvidence: false,
          ancestorReparsePoints: false, alreadyCompressed: false, streamInventoryComplete: true, namedStreams: [],
          allocationMethod: 'GetFileInformationByHandleEx(FileCompressionInfo).CompressedFileSize' } }] } };
}

function completion(p) {
  const e = p.inventory.entries[0];
  return { schemaVersion: 1, kind: 'owner-ntfs-compression-complete', ownerId: owner, authorizedBy: owner,
    method: audit.METHOD, planSha256: sha, leaseEpoch: p.leaseEpoch, closedThroughout: true,
    exclusionsUnchanged: true, noFilesMovedOrDeleted: true, noActiveCompileDuringOperation: true,
    allFilesAccountedFor: true, startedAt: new Date(now - 800).toISOString(), completedAt: new Date(now - 100).toISOString(),
    entries: [{ id: e.id, path: e.sourcePath, allocatedVolume: 'D', allocationEvidence: 'owner-native-allocated',
      byteHashVerifiedBy: owner, streamsUnchanged: true, linksUnchanged: true, compressionSucceeded: true,
      beforeCompressed: false, afterCompressed: true, beforeByte: e.byte, afterByte: e.byte,
      beforeSha256: e.sha256, afterSha256: e.sha256, beforeFileIdentity: e.fileIdentity,
      afterFileIdentity: e.fileIdentity, beforeAllocatedBytes: e.allocatedBytes, afterAllocatedBytes: 1024,
      beforeAllocationMethod: e.compressionReview.allocationMethod,
      afterAllocationMethod: e.compressionReview.allocationMethod }] };
}

function test(name, run) {
  try { run(); results.push({ name, status: 'PASS' }); }
  catch (error) { results.push({ name, status: 'FAIL', detail: error.message }); }
}

function refuses(fn, code) {
  assert.throws(fn, error => error.code === code, code);
}

function recoverySample() {
  return { schemaVersion: 1, kind: 'storage-recovery-planning-sample',
    sample: { providedBy: 'lead', source: 'lead-attested-physical-DriveInfo',
      capturedAt: '2026-11-21T18:31:00Z', availableBytes: { C: 1705369600, D: 3294576640 } },
    demand: { C: { growthUpperBytes: null, tempPeakUpperBytes: null, outstandingHoldsBytes: null, otherWriterUpperBytes: null },
      D: { growthUpperBytes: null, tempPeakUpperBytes: null, outstandingHoldsBytes: null, otherWriterUpperBytes: null } } };
}

test('closed plan is metadata-only with zero guaranteed reclaim', () => {
  const r = audit.reviewPlan(plan(), owner, now);
  assert.equal(r.candidateCount, 1);
  assert.equal(r.potentialCeilingBytes.D, 4096);
  assert.equal(r.guaranteedReclaimBytes, 0);
  assert.equal(r.foreignFilesOpened, 0);
  assert.equal(r.productionAdmission, false);
});

test('foreign owner and missing authority fail closed', () => {
  refuses(() => audit.reviewPlan(plan(), 'foreign', now), 'COMPRESSION_AUTHORITY');
  const p = plan(); p.ownerCompressionAuthorized = false;
  refuses(() => audit.reviewPlan(p, owner, now), 'COMPRESSION_AUTHORITY');
});

test('active writer and reader lease cannot be compressed', () => {
  const p = plan(); p.inventory.entries[0].activeWriterPids = [28388];
  refuses(() => audit.reviewPlan(p, owner, now), 'ACTIVE_WRITER');
  p.inventory.entries[0].activeWriterPids = []; p.inventory.entries[0].readerLeases = ['protected-reader'];
  refuses(() => audit.reviewPlan(p, owner, now), 'READER_LEASE');
});

test('failure, relink, source, log, archive and active compile exclusions are mandatory', () => {
  for (const key of ['failureEvidence', 'relinkMaterial', 'sourceMaterial', 'logOrReceipt',
    'sourcePackage', 'protectedEvidence', 'inActiveCompileTree', 'ancestorReparsePoints', 'alreadyCompressed']) {
    const p = plan(); p.inventory.entries[0].compressionReview[key] = true;
    refuses(() => audit.reviewPlan(p, owner, now), 'COMPRESSION_PROTECTED_OR_UNKNOWN');
    const missing = plan(); delete missing.inventory.entries[0].compressionReview[key];
    refuses(() => audit.reviewPlan(missing, owner, now), 'COMPRESSION_PROTECTED_OR_UNKNOWN');
  }
});

test('streams, non-NTFS and hardlinks rejected', () => {
  const p = plan(); p.inventory.entries[0].compressionReview.namedStreams = ['secret'];
  refuses(() => audit.reviewPlan(p, owner, now), 'COMPRESSION_PROTECTED_OR_UNKNOWN');
  const n = plan(); n.inventory.entries[0].compressionReview.fileSystem = 'ReFS';
  refuses(() => audit.reviewPlan(n, owner, now), 'COMPRESSION_PROTECTED_OR_UNKNOWN');
  const h = plan(); h.inventory.entries[0].hardlinkCount = 2;
  refuses(() => audit.reviewPlan(h, owner, now), 'HARDLINK_UNKNOWN');
});

test('root, ADS, C-backed and source-extension boundaries', () => {
  for (const file of [root + '\\..\\outside\\one.obj', root + '\\old\\one.obj:ads']) {
    const p = plan(); p.inventory.entries[0].sourcePath = file;
    refuses(() => audit.reviewPlan(p, owner, now), 'PATH_BOUNDARY');
  }
  const p = plan(); const e = p.inventory.entries[0]; e.sourcePath = e.targetPath = root + '\\one.cpp';
  refuses(() => audit.reviewPlan(p, owner, now), 'COMPRESSION_SCOPE');
  const c = plan(); c.inventory.sourceOwnedRoots = ['C:\\owner'];
  c.inventory.entries[0].sourcePath = 'C:\\owner\\one.obj'; c.inventory.entries[0].allocatedVolume = 'C';
  refuses(() => audit.reviewPlan(c, owner, now), 'COMPRESSION_SCOPE');
});

test('protected roots and stale plans rejected', () => {
  const p = plan(); p.excludedRoots = [root];
  refuses(() => audit.reviewPlan(p, owner, now), 'PROTECTED_ROOT');
  const s = plan(); s.inventory.capturedAt = new Date(now - 120001).toISOString();
  refuses(() => audit.reviewPlan(s, owner, now), 'EVIDENCE_STALE');
});

test('allocation identity cannot be counted twice', () => {
  const p = plan(), e = clone(p.inventory.entries[0]); e.id = 'obj2';
  e.sourcePath = e.targetPath = root + '\\old\\two.obj'; p.inventory.entries.push(e);
  refuses(() => audit.reviewPlan(p, owner, now), 'IDENTITY_DUPLICATE');
});

test('bit-identical owner completion accounts allocation, never grants admission', () => {
  const p = plan(), r = audit.reviewCompletion(p, completion(p), sha, owner, now);
  assert.equal(r.observedNetReleasedBytes, 3072);
  assert.equal(r.reclaimCreditedBytes, 0);
  assert.equal(r.foreignFilesOpened, 0);
  assert.equal(r.productionAdmission, false);
});

test('changed bytes, hash or identity invalidate completion', () => {
  for (const key of ['afterByte', 'afterSha256', 'afterFileIdentity']) {
    const p = plan(), d = completion(p);
    d.entries[0][key] = key === 'afterByte' ? 99 : key === 'afterSha256' ? 'b'.repeat(64) : 'different';
    refuses(() => audit.reviewCompletion(p, d, sha, owner, now), 'CONTENT_OR_IDENTITY_CHANGED');
  }
});

test('missing, duplicated, moved or unbound completion refused', () => {
  const p = plan(), missing = completion(p); missing.entries = [];
  refuses(() => audit.reviewCompletion(p, missing, sha, owner, now), 'COMPLETION_COVERAGE');
  const moved = completion(p); moved.noFilesMovedOrDeleted = false;
  refuses(() => audit.reviewCompletion(p, moved, sha, owner, now), 'COMPLETION_AUTHORITY');
  const hashbad = completion(p); hashbad.planSha256 = 'b'.repeat(64);
  refuses(() => audit.reviewCompletion(p, hashbad, sha, owner, now), 'COMPLETION_AUTHORITY');
});

test('allocation increase stays negative, not reclaimed credit', () => {
  const p = plan(), d = completion(p); d.entries[0].afterAllocatedBytes = 8192;
  const r = audit.reviewCompletion(p, d, sha, owner, now);
  assert.equal(r.observedNetReleasedBytes, -4096);
  assert.equal(r.reclaimCreditedBytes, 0);
});

test('historical sample has exact D deficit, C budget and aggregate minimum', () => {
  const r = audit.recovery(recoverySample(), now);
  assert.equal(r.DFloorShortfallBeforeGrowthBytes, 1000390656);
  assert.equal(r.CHeadroomBeforeGrowthBytes, 631627776);
  assert.equal(r.aggregateMinimumNetRecoveryBeforeGrowthBytes, 368762880);
  assert.equal(r.DRequiredRecoveryWithBoundsBytes, null);
  assert.equal(r.unknownDemandBounds.length, 8);
  assert.equal(r.productionAdmission, false);
});

test('bounded growth adds to unchanged floors, not cached admission', () => {
  const s = recoverySample();
  for (const v of ['C', 'D']) for (const k of Object.keys(s.demand[v])) s.demand[v][k] = 0;
  s.demand.D.growthUpperBytes = 100;
  const r = audit.recovery(s, now);
  assert.equal(r.DRequiredRecoveryWithBoundsBytes, 1000390756);
  assert.equal(r.aggregateMinimumNetRecoveryWithBoundsBytes, 368762980);
  assert.equal(r.originalRecoveryFloorsBytes.C, 1073741824);
  assert.equal(r.originalRecoveryFloorsBytes.D, 4294967296);
  assert.equal(r.productionAdmission, false);
});

test('negative, unsafe bytes and future samples rejected', () => {
  const s = recoverySample(); s.sample.availableBytes.D = -1;
  refuses(() => audit.recovery(s, now), 'BYTE_INVALID');
  const n = recoverySample(); n.demand.D.growthUpperBytes = Number.MAX_SAFE_INTEGER + 1;
  refuses(() => audit.recovery(n, now), 'BYTE_INVALID');
  const f = recoverySample(); f.sample.capturedAt = new Date(now + 1).toISOString();
  refuses(() => audit.recovery(f, now), 'EVIDENCE_FUTURE');
});

test('historical locator never marks extension candidates closed', () => {
  const copy = { schemaVersion: 1, kind: 'owned-storage-copy-verified', sourceAndCopySha256Match: true,
    groups: [{ name: 'fixture', destination: root, records: [
      { path: 'old/one.obj', bytes: 100, sha256: sha }, { path: 'source/code.cpp', bytes: 10, sha256: sha } ] }] };
  const allocation = { schemaVersion: 1, kind: 'owned-storage-allocation-and-cache-audit', copyManifestSha256: sha,
    groups: [{ name: 'fixture', destination: root, copyAllocatedBytes: 8192 }] };
  const r = audit.locate(copy, allocation, sha);
  assert.equal(r.candidateCount, 1);
  assert.equal(r.groups[0].candidates[0].currentClosedAuthorization, false);
  assert.equal(r.reclaimCreditedBytes, 0);
  const unsafe = clone(copy); unsafe.groups[0].records[0].path = '../one.obj';
  refuses(() => audit.locate(unsafe, allocation, sha), 'PATH_BOUNDARY');
  const unrelated = clone(allocation); unrelated.copyManifestSha256 = 'b'.repeat(64);
  refuses(() => audit.locate(copy, unrelated, sha), 'RECEIPT_BINDING');
});

test('operation allowlist excludes all execution and cleanup', () => {
  for (const op of ['compact', 'compress', 'delete', 'cleanup', 'move', 'build', 'kill', 'reserve']) {
    refuses(() => audit.command([op]), 'OPERATION_REFUSED');
  }
});

test('allocation methods, encrypted/sparse files and duplicate paths fail closed', () => {
  const p = plan(); p.inventory.entries[0].compressionReview.allocationMethod = 'logical-Length';
  refuses(() => audit.reviewPlan(p, owner, now), 'COMPRESSION_ALLOCATION_METHOD');
  for (const key of ['encrypted', 'sparse', 'deletePending']) {
    const bad = plan(); bad.inventory.entries[0].compressionReview[key] = true;
    refuses(() => audit.reviewPlan(bad, owner, now), 'COMPRESSION_PROTECTED_OR_UNKNOWN');
  }
  const q = plan(), e = clone(q.inventory.entries[0]); e.id = 'obj2'; e.fileIdentity = 'invented-identity';
  q.inventory.entries.push(e);
  refuses(() => audit.reviewPlan(q, owner, now), 'PATH_DUPLICATE');
  const completed = completion(plan()); completed.entries[0].afterAllocationMethod = 'GetCompressedFileSizeW';
  refuses(() => audit.reviewCompletion(plan(), completed, sha, owner, now), 'COMPRESSION_ALLOCATION_METHOD');
});

test('receipt SHA and path boundaries are enforced before any foreign read', () => {
  const local = 'compression-plan-1831.json';
  const raw = fs.readFileSync(path.join(__dirname, local));
  assert.equal(audit.readReceipt(local, audit.hash(raw)).kind, 'storage-recovery-planning-sample');
  refuses(() => audit.readReceipt(local, 'b'.repeat(64)), 'RECEIPT_HASH_MISMATCH');
  for (const unsafe of ['../receipt.json', 'C:/foreign/receipt.json', 'D:relative.json',
    'receipt.json:ads', 'NUL.json', 'receipt?.json']) {
    refuses(() => audit.readReceipt(unsafe, sha), 'PATH_BOUNDARY');
  }
});

test('publisher lock is checked before and after reading a local receipt', () => {
  const original = fs.lstatSync, marker = Symbol('lock-refusal');
  let calls = 0;
  try {
    fs.lstatSync = function(file, options) {
      if (path.basename(String(file)) === '.publisher.lock') throw marker;
      return original.call(fs, file, options);
    };
    assert.throws(() => audit.readReceipt('compression-plan-1831.json', sha), error => error === marker);
    fs.lstatSync = function(file, options) {
      if (path.basename(String(file)) === '.publisher.lock') return ++calls > 1 ? { lock: true } : undefined;
      return original.call(fs, file, options);
    };
    const raw = fs.readFileSync(path.join(__dirname, 'compression-plan-1831.json'));
    refuses(() => audit.readReceipt('compression-plan-1831.json', audit.hash(raw)), 'PUBLISHER_LOCK');
    assert.equal(calls, 2);
  } finally {
    fs.lstatSync = original;
  }
});

test('closed generated PDB/EXE are preservation-only, not reader or relink exceptions', () => {
  for (const ext of ['pdb', 'exe']) {
    const p = plan(), e = p.inventory.entries[0];
    e.sourcePath = e.targetPath = root + '\\old\\one.' + ext;
    assert.equal(audit.reviewPlan(p, owner, now).candidates[0].fileClass, 'closed-generated');
    e.readerLeases = ['still-open'];
    refuses(() => audit.reviewPlan(p, owner, now), 'READER_LEASE');
    e.readerLeases = []; e.compressionReview.relinkMaterial = true;
    refuses(() => audit.reviewPlan(p, owner, now), 'COMPRESSION_PROTECTED_OR_UNKNOWN');
  }
});

test('tools source archives require independent compression authorization and preserved provenance', () => {
  const p = plan(), e = p.inventory.entries[0], c = e.compressionReview;
  e.kind = 'source'; e.sourcePath = e.targetPath = root + '\\tools\\source.tar.xz';
  c.oldGeneratedCopy = false; c.sourceMaterial = true; c.sourcePackage = true;
  refuses(() => audit.reviewPlan(p, owner, now), 'SOURCE_ARCHIVE_AUTHORITY');
  c.toolsOwnedSourceArchive = true; c.archiveCompressionAuthorizedBy = owner;
  c.sourceArchiveOriginReceiptSha256 = sha;
  assert.equal(audit.reviewPlan(p, owner, now).candidates[0].fileClass, 'owner-tools-source-archive');
  assert.equal(audit.reviewCompletion(p, completion(p), sha, owner, now).productionAdmission, false);
  c.relinkMaterial = true;
  refuses(() => audit.reviewPlan(p, owner, now), 'COMPRESSION_PROTECTED_OR_UNKNOWN');
  c.relinkMaterial = false; c.archiveCompressionAuthorizedBy = 'other-owner';
  refuses(() => audit.reviewPlan(p, owner, now), 'SOURCE_ARCHIVE_AUTHORITY');
});

const report = { schemaVersion: 1, suite: 'receipt-only-compression-and-recovery',
  capturedAt: new Date().toISOString(), fixtureClock: new Date(now).toISOString(),
  fixtureDataSynthetic: true, realCompressionExecuted: false, foreignFilesOpened: 0,
  capacityProbeExecuted: false, productionAdmission: false,
  pass: results.filter(r => r.status === 'PASS').length, fail: results.filter(r => r.status === 'FAIL').length, results };
const name = process.argv[2];
assert.ok(typeof name === 'string' && /^results-compression-[a-z0-9-]+\.json$/i.test(name), 'Unused bounded result filename required');
const destination = path.join(__dirname, name);
physicalDNoLinks(__dirname, __dirname, false);
assert.ok(!fs.existsSync(destination), 'Preserve every earlier result; CreateNew only');
fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n', { flag: 'wx', encoding: 'utf8' });
console.log(JSON.stringify(report, null, 2));
if (report.fail) process.exitCode = 1;
