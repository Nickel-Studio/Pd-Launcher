'use strict';

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { AuditError, LIMIT, RESERVE, absolute, inside, validateInventory,
  physicalDNoLinks } = require('./storage-audit.cjs');
const W = path.win32;
const KIND = 'owner-closed-ntfs-compression-plan';
const METHOD = 'NTFS-native-in-place-file-only';

function check(value, code, detail) {
  if (!value) throw new AuditError(code, detail);
}

function bytes(value, name) {
  check(Number.isSafeInteger(value) && value >= 0, 'BYTE_INVALID', name);
  return value;
}

function total(values, name) {
  return bytes(values.reduce((a, b) => a + bytes(b, name), 0), name);
}

function sha(value) {
  check(typeof value === 'string' && /^[a-f0-9]{64}$/.test(value), 'SHA_INVALID', 'Full SHA-256 required');
  return value;
}

function stamp(value, name) {
  const n = typeof value === 'string' ? Date.parse(value) : NaN;
  check(Number.isFinite(n), 'TIME_INVALID', name);
  return n;
}

function hash(buffer) {
  return crypto.createHash('sha256').update(buffer).digest('hex');
}

function relativeFile(value) {
  check(typeof value === 'string' && value.length > 0 && !W.isAbsolute(value) &&
    !path.isAbsolute(value) && !value.split(/[\\/]/).some(p => p === '..' || p === '.' || p === '' ||
      /[\x00-\x1f<>:"|?*]/.test(p) || /[ .]$/.test(p) ||
      /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(p)),
  'PATH_BOUNDARY', 'Explicit relative file record required');
  return value;
}

function locate(copy, allocation, copySha) {
  sha(copySha);
  check(copy && copy.schemaVersion === 1 && copy.kind === 'owned-storage-copy-verified' &&
    copy.sourceAndCopySha256Match === true && Array.isArray(copy.groups) && copy.groups.length <= 16,
  'LOCATOR_SCHEMA', 'Only the supplied historical owner copy receipt is a locator');
  check(allocation && allocation.schemaVersion === 1 &&
    allocation.kind === 'owned-storage-allocation-and-cache-audit' &&
    allocation.copyManifestSha256 === copySha && Array.isArray(allocation.groups),
  'RECEIPT_BINDING', 'Allocation receipt must bind exact copy receipt bytes');
  let count = 0;
  const roots = new Set();
  const files = new Set();
  const groups = copy.groups.map(g => {
    check(typeof g.name === 'string' && Array.isArray(g.records), 'LOCATOR_SCHEMA', 'Explicit groups required');
    const root = absolute(g.destination);
    check(root.startsWith('D:') && !roots.has(root.toLowerCase()), 'PATH_BOUNDARY', 'Unique supplied D roots');
    roots.add(root.toLowerCase());
    const matches = allocation.groups.filter(a => a.name === g.name && absolute(a.destination) === root);
    check(matches.length === 1, 'RECEIPT_BINDING', g.name);
    const candidates = [];
    for (const r of g.records) {
      check(++count <= LIMIT.entries, 'INVENTORY_LIMIT', 'Bounded owner records only');
      relativeFile(r.path);
      const file = absolute(W.join(root, r.path));
      check(inside(root, file) && file !== root && !files.has(file.toLowerCase()), 'PATH_BOUNDARY', r.path);
      files.add(file.toLowerCase());
      bytes(r.bytes, r.path);
      sha(r.sha256);
      if (/\.(obj|lib|pch)$/i.test(r.path)) {
        candidates.push({ path: file, relativePath: r.path, byte: r.bytes, sha256: r.sha256,
          currentClosedAuthorization: false, currentAllocatedBytes: null });
      }
    }
    return { name: g.name, ownerSuppliedDestination: g.destination, canonicalDestination: root,
      historicalWholeTreeAllocatedBytes: bytes(matches[0].copyAllocatedBytes, g.name),
      generatedExtensionRecords: candidates.length,
      historicalCandidateLogicalBytes: total(candidates.map(r => r.byte), g.name), candidates };
  });
  return { decision: 'HISTORICAL_LOCATOR_NOT_CLOSED_AUTHORIZATION', copyReceiptSha256: copySha,
    recordCount: count, candidateCount: groups.reduce((n, g) => n + g.candidates.length, 0), groups,
    foreignFilesOpened: 0, reclaimCreditedBytes: 0, productionAdmission: false };
}

function reviewPlan(doc, owner, now = Date.now()) {
  check(doc && doc.schemaVersion === 1 && doc.kind === KIND && doc.ownerId === owner &&
    doc.method === METHOD && doc.ownerCompressionAuthorized === true &&
    doc.exclusionInventoryComplete === true && Array.isArray(doc.excludedRoots) && doc.excludedRoots.length <= 32,
  'COMPRESSION_AUTHORITY', 'Explicit owner file-only preservation-compression plan required');
  const inv = doc.inventory;
  const summary = validateInventory(inv, owner, now);
  check(doc.leaseEpoch === inv.leaseEpoch, 'PLAN_OWNER_LEASE', 'Compression and inventory epochs differ');
  const excluded = doc.excludedRoots.map(absolute);
  const paths = new Set();
  const candidates = inv.entries.map(e => {
    const file = absolute(e.sourcePath);
    const c = e.compressionReview;
    const generated = e.kind === 'build' && /\.(obj|lib|pch|pdb|exe)$/i.test(file);
    const archive = e.kind === 'source' && /\.(zip|tar|tar\.gz|tgz|tar\.xz|txz|tar\.bz2|tbz2|tar\.zst|tzst|7z)$/i.test(file);
    check(file === absolute(e.targetPath) && e.allocatedVolume === 'D' && (generated || archive),
      'COMPRESSION_SCOPE', e.id);
    check(!paths.has(file.toLowerCase()), 'PATH_DUPLICATE', e.id);
    paths.add(file.toLowerCase());
    check(!excluded.some(root => inside(root, file)), 'PROTECTED_ROOT', e.id);
    check(c && c.authorizedBy === owner && c.fileSystem === 'NTFS' && c.isRegularFile === true &&
      c.deletePending === false && c.encrypted === false && c.sparse === false &&
      c.inActiveCompileTree === false && c.failureEvidence === false && c.logOrReceipt === false &&
      c.relinkMaterial === false && c.protectedEvidence === false && c.ancestorReparsePoints === false &&
      c.alreadyCompressed === false && c.streamInventoryComplete === true &&
      Array.isArray(c.namedStreams) && c.namedStreams.length === 0,
    'COMPRESSION_PROTECTED_OR_UNKNOWN', e.id + ': exclusions, file type, streams and compression state must be explicit');
    if (generated) {
      check(c.oldGeneratedCopy === true && c.sourceMaterial === false && c.sourcePackage === false,
        'COMPRESSION_PROTECTED_OR_UNKNOWN', e.id);
    } else {
      check(c.oldGeneratedCopy === false && c.sourceMaterial === true && c.sourcePackage === true &&
        c.toolsOwnedSourceArchive === true && c.archiveCompressionAuthorizedBy === owner,
      'SOURCE_ARCHIVE_AUTHORITY', e.id + ': source archives need separate explicit owner preservation authorization');
      sha(c.sourceArchiveOriginReceiptSha256);
    }
    check(['GetFileInformationByHandleEx(FileCompressionInfo).CompressedFileSize',
      'GetCompressedFileSizeW'].includes(c.allocationMethod), 'COMPRESSION_ALLOCATION_METHOD',
    e.id + ': use the same compression-aware native allocation method before and after');
    return { id: e.id, path: file, fileClass: generated ? 'closed-generated' : 'owner-tools-source-archive',
      byte: e.byte, sha256: e.sha256, beforeAllocatedBytes: e.allocatedBytes,
      allocationMethod: c.allocationMethod, fileIdentity: e.fileIdentity };
  });
  return { decision: 'OWNER_PLAN_REVIEWED_NOT_EXECUTION_PERMISSION', ownerId: owner, leaseEpoch: inv.leaseEpoch,
    candidateCount: candidates.length, candidates,
    potentialCeilingBytes: summary.markedClosedAllocatedBytes,
    guaranteedReclaimBytes: 0, foreignFilesOpened: 0, compressionExecuted: false,
    ownerMustRevalidateBeforeAction: true, productionAdmission: false };
}

function reviewCompletion(plan, done, expectedPlanSha, owner, now = Date.now()) {
  sha(expectedPlanSha);
  check(plan && plan.inventory && done && done.schemaVersion === 1 &&
    done.kind === 'owner-ntfs-compression-complete' && done.ownerId === owner && done.authorizedBy === owner &&
    done.method === METHOD && done.planSha256 === expectedPlanSha && done.leaseEpoch === plan.leaseEpoch &&
    done.closedThroughout === true && done.exclusionsUnchanged === true && done.noFilesMovedOrDeleted === true &&
    done.noActiveCompileDuringOperation === true && done.allFilesAccountedFor === true &&
    Array.isArray(done.entries), 'COMPLETION_AUTHORITY', 'Owner-bound complete preservation receipt required');
  const captured = stamp(plan.inventory.capturedAt, 'plan capture');
  const started = stamp(done.startedAt, 'compression start');
  const ended = stamp(done.completedAt, 'compression completion');
  check(captured <= started && started - captured <= LIMIT.ageMs && started <= ended && ended <= now &&
    now - ended <= LIMIT.ageMs, 'EVIDENCE_STALE', 'Fresh start-bound closed plan and completion required');
  const reviewed = reviewPlan(plan, owner, captured);
  check(done.entries.length === reviewed.candidates.length, 'COMPLETION_COVERAGE', 'All planned files required');
  const byId = new Map(reviewed.candidates.map(e => [e.id, e]));
  const seen = new Set();
  const delta = [];
  for (const r of done.entries) {
    const e = byId.get(r.id);
    check(e && !seen.has(r.id), 'COMPLETION_COVERAGE', 'Unknown or duplicate completion id');
    seen.add(r.id);
    check(r.path === e.path && r.allocatedVolume === 'D' && r.allocationEvidence === 'owner-native-allocated' &&
      r.byteHashVerifiedBy === owner && r.streamsUnchanged === true && r.linksUnchanged === true &&
      r.compressionSucceeded === true && r.beforeCompressed === false && r.afterCompressed === true,
    'COMPLETION_PROOF_UNKNOWN', r.id);
    check(r.beforeAllocationMethod === e.allocationMethod && r.afterAllocationMethod === e.allocationMethod,
      'COMPRESSION_ALLOCATION_METHOD', r.id);
    check(r.beforeByte === e.byte && r.afterByte === e.byte && sha(r.beforeSha256) === e.sha256 &&
      sha(r.afterSha256) === e.sha256 && r.beforeFileIdentity === e.fileIdentity &&
      r.afterFileIdentity === e.fileIdentity, 'CONTENT_OR_IDENTITY_CHANGED', r.id);
    check(bytes(r.beforeAllocatedBytes, r.id) === e.beforeAllocatedBytes,
      'ALLOCATION_BINDING', r.id);
    bytes(r.afterAllocatedBytes, r.id);
    delta.push({ id: r.id, path: e.path, allocationMethod: e.allocationMethod,
      beforeAllocatedBytes: r.beforeAllocatedBytes, afterAllocatedBytes: r.afterAllocatedBytes,
      observedReleasedBytes: r.beforeAllocatedBytes - r.afterAllocatedBytes });
  }
  const before = total(delta.map(r => r.beforeAllocatedBytes), 'before allocation');
  const after = total(delta.map(r => r.afterAllocatedBytes), 'after allocation');
  return { decision: 'OWNER_RECEIPT_VALID_NOT_CAPACITY_ADMISSION', ownerId: owner,
    planSha256: expectedPlanSha, closedFileCount: delta.length, allocationBeforeBytes: before,
    allocationAfterBytes: after, observedNetReleasedBytes: before - after, entries: delta,
    reclaimCreditedBytes: 0, foreignFilesOpened: 0, productionAdmission: false,
    nextStep: 'Owner must sample fresh physical capacity and bind all growth/hold/writer bounds to its write gate' };
}

function recovery(doc, now = Date.now()) {
  check(doc && doc.schemaVersion === 1 && doc.kind === 'storage-recovery-planning-sample' &&
    doc.sample && doc.sample.providedBy === 'lead' && doc.sample.source === 'lead-attested-physical-DriveInfo',
  'CAPACITY_SOURCE', 'This mode only calculates a supplied lead sample; never admission');
  const at = stamp(doc.sample.capturedAt, 'lead sample');
  check(at <= now, 'EVIDENCE_FUTURE', 'Lead sample cannot be future dated');
  const free = {};
  const peak = {};
  const unknown = [];
  for (const v of ['C', 'D']) {
    free[v] = bytes(doc.sample.availableBytes && doc.sample.availableBytes[v], v + ' available bytes');
    const values = [];
    for (const k of ['growthUpperBytes', 'tempPeakUpperBytes', 'outstandingHoldsBytes', 'otherWriterUpperBytes']) {
      const value = doc.demand && doc.demand[v] && doc.demand[v][k];
      if (value === null || value === undefined) unknown.push(v + '.' + k);
      else values.push(bytes(value, v + '.' + k));
    }
    peak[v] = values.length === 4 ? total(values, v + ' peak') : null;
  }
  const floors = { C: RESERVE.incremental, D: RESERVE.game };
  const aggregateReserve = total(Object.values(floors), 'original recovery floors');
  const aggregateFree = total(Object.values(free), 'sample free bytes');
  const required = v => peak[v] === null ? null : total([floors[v], peak[v]], v + ' floor and peak');
  return { decision: 'RECOVERY_ARITHMETIC_NOT_ADMISSION', sampleCapturedAt: doc.sample.capturedAt,
    sampleAgeMs: now - at, originalRecoveryFloorsBytes: floors, availableBytes: free,
    DFloorShortfallBeforeGrowthBytes: Math.max(0, floors.D - free.D),
    CHeadroomBeforeGrowthBytes: free.C - floors.C,
    aggregateMinimumNetRecoveryBeforeGrowthBytes: Math.max(0, aggregateReserve - aggregateFree),
    DRequiredRecoveryWithBoundsBytes: required('D') === null ? null : Math.max(0, required('D') - free.D),
    CHeadroomWithBoundsBytes: required('C') === null ? null : free.C - required('C'),
    aggregateMinimumNetRecoveryWithBoundsBytes: unknown.length ? null :
      Math.max(0, total([required('C'), required('D')], 'floors and peaks') - aggregateFree),
    unknownDemandBounds: unknown, reclaimCreditedBytes: 0, productionAdmission: false,
    policy: 'Lead recovery target C >= 1 GiB and D >= 4 GiB plus their bounds; old reserve() is unchanged',
    warning: 'Relocation conserves aggregate free space only in an ideal equal-allocation model; measure real allocation and fresh capacity' };
}

function sameStat(a, b) {
  return a.dev === b.dev && a.ino === b.ino && a.size === b.size &&
    a.mtimeNs === b.mtimeNs && a.ctimeNs === b.ctimeNs;
}

function requireUnlocked(file) {
  for (let dir = path.dirname(file); ; dir = path.dirname(dir)) {
    const lock = fs.lstatSync(path.join(dir, '.publisher.lock'), { throwIfNoEntry: false });
    check(lock === undefined, 'PUBLISHER_LOCK', file);
    if (dir.toLowerCase() === __dirname.toLowerCase()) break;
    check(dir !== path.dirname(dir), 'PATH_BOUNDARY', file);
  }
}

function readReceipt(name, expectedSha) {
  sha(expectedSha);
  relativeFile(name);
  const file = path.resolve(__dirname, name);
  const before = physicalDNoLinks(__dirname, file, true);
  check(before.size <= BigInt(LIMIT.text) && before.nlink === 1n, 'TEXT_LIMIT', name);
  // A publisher lock at any local ancestor blocks reads, including locks appearing during the read.
  requireUnlocked(file);
  const fd = fs.openSync(file, fs.constants.O_RDONLY);
  try {
    check(sameStat(before, fs.fstatSync(fd, { bigint: true })), 'IDENTITY_CHANGED', name);
    const buffer = Buffer.alloc(LIMIT.text + 1);
    let size = 0;
    for (;;) {
      const n = fs.readSync(fd, buffer, size, buffer.length - size, null);
      if (!n) break;
      size += n;
      check(size <= LIMIT.text, 'TEXT_LIMIT', name);
    }
    check(sameStat(before, fs.fstatSync(fd, { bigint: true })) &&
      sameStat(before, physicalDNoLinks(__dirname, file, true)), 'IDENTITY_CHANGED', name);
    requireUnlocked(file);
    const raw = buffer.subarray(0, size);
    check(hash(raw) === expectedSha, 'RECEIPT_HASH_MISMATCH', name);
    try { return JSON.parse(raw.toString('utf8').replace(/^\uFEFF/, '')); }
    catch (_) { throw new AuditError('JSON_INVALID', name); }
  } finally {
    fs.closeSync(fd);
  }
}

function command(args) {
  const [mode, ...rest] = args;
  check(['locate', 'plan', 'complete', 'recovery'].includes(mode), 'OPERATION_REFUSED',
    'Receipt review only; no compact, cleanup, move, build, process or capacity commands');
  const opts = {};
  for (let i = 0; i < rest.length; i += 2) {
    const key = rest[i];
    check(['--input', '--expected-sha', '--allocation-input', '--allocation-sha', '--plan-input',
      '--plan-sha', '--owner'].includes(key) && typeof rest[i + 1] === 'string' &&
      !rest[i + 1].startsWith('--') && !Object.hasOwn(opts, key), 'ARGUMENT_REFUSED', 'Explicit unique option pairs required');
    opts[key] = rest[i + 1];
  }
  const input = readReceipt(opts['--input'], opts['--expected-sha']);
  if (mode === 'recovery') return recovery(input);
  if (mode === 'locate') return locate(input,
    readReceipt(opts['--allocation-input'], opts['--allocation-sha']), opts['--expected-sha']);
  if (mode === 'plan') return reviewPlan(input, opts['--owner']);
  return reviewCompletion(readReceipt(opts['--plan-input'], opts['--plan-sha']), input,
    opts['--plan-sha'], opts['--owner']);
}

module.exports = { KIND, METHOD, hash, locate, reviewPlan, reviewCompletion, recovery, readReceipt, command };

if (require.main === module) {
  try {
    process.stdout.write(JSON.stringify(command(process.argv.slice(2)), null, 2) + '\n');
  } catch (error) {
    process.stdout.write(JSON.stringify({ decision: 'REFUSED', code: error.code || 'IO_ERROR',
      detail: error.message, productionAdmission: false }, null, 2) + '\n');
    process.exitCode = 2;
  }
}
