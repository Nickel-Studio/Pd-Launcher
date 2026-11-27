'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { isDeepStrictEqual } = require('node:util');
const { AuditError, LIMIT, absolute, inside, physicalDNoLinks } = require('./storage-audit.cjs');
const { hash } = require('./compression-audit.cjs');

const CLUSTER_METHOD = 'FSCTL_GET_RETRIEVAL_POINTERS: sum non-sparse LCN clusters * GetDiskFreeSpaceW clusterBytes';
const ROOT = __dirname;

function check(ok, code, detail) {
  if (!ok) throw new AuditError(code, detail);
}

function bytes(n, detail) {
  check(Number.isSafeInteger(n) && n >= 0, 'ALLOCATION_INTEGER', detail);
  return n;
}

function total(values) {
  return values.reduce((sum, n) => bytes(sum + bytes(n, 'counter'), 'counter sum'), 0);
}

function sha(value) {
  check(typeof value === 'string' && /^[a-f0-9]{64}$/.test(value), 'SHA_INVALID', 'Full SHA-256 required');
  return value;
}

function stamp(value) {
  const n = typeof value === 'string' ? Date.parse(value) : NaN;
  check(Number.isFinite(n), 'TIME_UNKNOWN', 'Original closed-window timestamp required');
  return n;
}

function volumeGuid(value) {
  check(typeof value === 'string' && /^\\\\\?\\Volume\{[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\}\\$/i.test(value),
    'PHYSICAL_VOLUME_UNKNOWN', 'Explicit original volume GUID required');
  return value.toLowerCase();
}

function fileId128(value) {
  check(typeof value === 'string' && /^[0-9a-f]{32}$/i.test(value), 'FILE_ID_UNKNOWN', 'Original FILE_ID_128 bytes required');
  return value.toLowerCase();
}

function unsignedSerial(value) {
  check((typeof value === 'number' && Number.isSafeInteger(value) && value >= 0) ||
    (typeof value === 'string' && /^(?:0|[1-9][0-9]*)$/.test(value)),
  'SERIAL_PRECISION', 'Do not compare rounded JSON numbers as native identities');
  const n = BigInt(value);
  check(n >= 0n && n <= 0xffffffffffffffffn, 'SERIAL_PRECISION', 'Native unsigned serial is out of range');
  return n.toString();
}

function parseJson(text) {
  try {
    return JSON.parse(text, (key, value, context) => {
      if (typeof value === 'number' && !Number.isSafeInteger(value)) {
        // Keep native unsigned identity tokens exact; unsafe byte counters still fail bytes().
        check(context && /^(?:0|[1-9][0-9]*)$/.test(context.source), 'JSON_INTEGER_PRECISION',
          'Lossless integer token support is required for unsafe JSON numbers');
        return context.source;
      }
      return value;
    });
  } catch (error) {
    if (error instanceof AuditError) throw error;
    throw new AuditError('JSON_INVALID', 'Bounded owner receipt must be valid JSON');
  }
}

function sameStat(a, b) {
  return ['dev', 'ino', 'size', 'mtimeNs', 'ctimeNs'].every(key => a[key] === b[key]);
}

function unlocked(file) {
  for (let dir = path.dirname(file); ; dir = path.dirname(dir)) {
    check(fs.lstatSync(path.join(dir, '.publisher.lock'), { throwIfNoEntry: false }) === undefined,
      'PUBLISHER_LOCK', 'Do not read an unfinished owner publication');
    if (dir.toLowerCase() === ROOT.toLowerCase()) break;
    check(dir !== path.dirname(dir), 'PATH_BOUNDARY', file);
  }
}

function readRaw(name, expectedSha) {
  sha(expectedSha);
  check(typeof name === 'string' && name.length > 0 && name.length < 512 &&
    !path.win32.isAbsolute(name) && !/[:\x00-\x1f]/.test(name) &&
    name.split(/[\\/]/).every(part => part && part !== '.' && part !== '..') && /\.(json|jsonl)$/i.test(name),
  'PATH_BOUNDARY', 'An explicit local owned JSON/JSONL receipt is required');
  const file = path.resolve(ROOT, name);
  const before = physicalDNoLinks(ROOT, file, true);
  check(before.nlink === 1n && before.size <= BigInt(LIMIT.text), 'TEXT_LIMIT', name);
  unlocked(file);
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
      sameStat(before, physicalDNoLinks(ROOT, file, true)), 'IDENTITY_CHANGED', name);
    unlocked(file);
    const raw = buffer.subarray(0, size);
    check(hash(raw) === expectedSha, 'RECEIPT_HASH_MISMATCH', name);
    return raw;
  } finally {
    fs.closeSync(fd);
  }
}

function readJson(name, expectedSha) {
  return parseJson(readRaw(name, expectedSha).toString('utf8').replace(/^\uFEFF/, ''));
}

function readRows(name, expectedSha) {
  const lines = readRaw(name, expectedSha).toString('utf8').replace(/^\uFEFF/, '').trim().split(/\r?\n/);
  check(lines.length > 0 && lines.length <= LIMIT.entries && lines.every(line => line.trim()),
    'ENTRY_LIMIT', 'A bounded complete JSONL set is required');
  return lines.map(parseJson);
}

function identity(volume, id) {
  return volumeGuid(volume) + '|file-id-128-raw|' + fileId128(id);
}

function reviewTransport(plan, rows, done, ownerRoot, bindings) {
  const root = absolute(ownerRoot);
  check(root.startsWith('D:') && /\\transport$/i.test(root), 'OWNER_ROOT_UNKNOWN', 'Explicit transport physical owner root required');
  check(plan && done && plan.schemaVersion === 1 && done.schemaVersion === 1 &&
    plan.owner === 'transport' && done.owner === 'transport' &&
    typeof plan.tag === 'string' && plan.tag.length > 0 && plan.tag === done.tag &&
    typeof plan.sourceCommit === 'string' && /^[a-f0-9]{40}$/.test(plan.sourceCommit) &&
    plan.sourceCommit === done.sourceCommit && plan.sourceCompletionSha256 === done.sourceCompletionSha256,
  'OWNER_BINDING', 'Original transport plan and completion must bind the same owner/generation');
  sha(plan.sourceCompletionSha256);
  check(bindings && done.manifest && done.perFileReceipts &&
    sha(bindings.planSha256) === done.manifest.sha256 &&
    sha(bindings.rowsSha256) === done.perFileReceipts.sha256 && sha(bindings.summarySha256),
  'RECEIPT_BINDING', 'Original completion must bind both exact raw receipts');
  check(inside(root, done.manifest.path) && inside(root, done.perFileReceipts.path),
    'OWNER_ROOT_UNKNOWN', 'Referenced receipt paths must remain in the supplied owner root');
  check(plan.allocationMethod === CLUSTER_METHOD && done.allocationMethod === CLUSTER_METHOD,
    'NATIVE_CLUSTER_METHOD', 'Compressed-file-size or FileStandard labels are not a cluster runlist');
  check(plan.uncompressedOnly === true && plan.deletions === false && plan.directoryCompression === false &&
    done.directoryCompression === false && done.filesMovedOrDeleted === false && done.sharedPrefixModified === false &&
    done.exclusiveHandlesClosed === true && done.allFileIdsUnchanged === true &&
    done.allOriginalPathsRetained === true && done.allSha256Unchanged === true && done.frozenSourcesZipsLogsUnchanged === true,
  'PRESERVATION_UNKNOWN', 'Original closed-file preservation evidence must be complete');
  check(plan.volume && done.volume && plan.volume.fileSystem === 'NTFS' && done.volume.fileSystem === 'NTFS' &&
    plan.volume.physicalRoot === 'D:\\' && done.volume.physicalRoot === 'D:\\',
  'PHYSICAL_VOLUME_UNKNOWN', 'Original native NTFS D-volume evidence required');
  const volume = volumeGuid(plan.volume.volumeName);
  check(volume === volumeGuid(done.volume.volumeName), 'PHYSICAL_VOLUME_UNKNOWN', 'Physical volume changed');
  const cluster = bytes(plan.volume.clusterBytes, 'cluster geometry');
  check(cluster >= 512 && cluster <= 65536 && (cluster & (cluster - 1)) === 0 && cluster === done.volume.clusterBytes,
    'CLUSTER_GEOMETRY', 'Original before/after native cluster geometry must agree');
  check(Array.isArray(plan.explicitAllowlist) && plan.explicitAllowlist.length > 0 &&
    plan.explicitAllowlist.length <= LIMIT.entries && Array.isArray(rows) && rows.length === plan.explicitAllowlist.length &&
    rows.length === done.successFileCount, 'ENTRY_COVERAGE', 'Each exact planned file requires one successful paired row');
  const captured = stamp(plan.createdUtc);
  const ended = stamp(done.completedUtc);
  check(captured <= ended && ended - captured <= LIMIT.ageMs, 'CLOSED_WINDOW', 'Original operation must fit its bounded window');
  const paths = new Map();
  const plannedIdentities = new Set();
  for (const entry of plan.explicitAllowlist) {
    check(entry && entry.before && entry.exclusiveReadWriteHandle === true && entry.shareMode === 0,
      'EXCLUSIVE_HANDLE_UNKNOWN', 'Original per-file retained exclusive handle required');
    const p = absolute(entry.physicalPath);
    check(inside(root, p) && /\.(obj|lib|pch|pdb|exe)$/i.test(p), 'FILE_SCOPE', 'Only exact generated physical owner files');
    const key = identity(volume, entry.before.fileId);
    check(!paths.has(p.toLowerCase()) && !plannedIdentities.has(key), 'PHYSICAL_DUPLICATE', p);
    paths.set(p.toLowerCase(), entry);
    plannedIdentities.add(key);
  }
  const seen = new Set();
  const entries = rows.map(row => {
    check(row && row.before && row.after, 'ENTRY_COVERAGE', 'Paired snapshots required');
    const p = absolute(row.physicalPath);
    const original = paths.get(p.toLowerCase());
    check(original && row.physicalPath === original.physicalPath && row.path === original.path && !seen.has(p.toLowerCase()),
      'ENTRY_COVERAGE', 'Unknown, changed or duplicated paired path');
    seen.add(p.toLowerCase());
    check(row.exclusiveReadWriteHandle === true && row.shareMode === 0 && row.setCompressionSucceeded === true &&
      row.nativeError === 0 && row.byteExact === true && row.identityUnchanged === true,
    'EXCLUSIVE_HANDLE_UNKNOWN', p);
    const before = row.before;
    const after = row.after;
    const start = stamp(row.startedUtc);
    const end = stamp(row.completedUtc);
    check(captured <= start && start <= end && end <= ended, 'CLOSED_WINDOW', p);
    check(isDeepStrictEqual(before, original.before), 'BEFORE_BINDING', p);
    check(before.links === 1 && after.links === 1 && before.compressionFormat === 0 && after.compressionFormat === 2,
      'FILE_STATE_UNKNOWN', p);
    check(fileId128(before.fileId) === fileId128(after.fileId) &&
      unsignedSerial(before.volumeSerial) === unsignedSerial(after.volumeSerial), 'IDENTITY_CHANGED', p);
    check(bytes(before.logicalBytes, p) === bytes(after.logicalBytes, p) && sha(before.sha256) === sha(after.sha256),
      'CONTENT_CHANGED', p);
    for (const snapshot of [before, after]) {
      check(bytes(snapshot.allocatedClusters, p) * cluster === bytes(snapshot.allocatedBytes, p) &&
        Number.isSafeInteger(snapshot.allocatedClusters * cluster) && bytes(snapshot.retrievalExtentCount, p) > 0,
      'CLUSTER_COUNTER_MISMATCH', p);
    }
    const released = before.allocatedBytes - after.allocatedBytes;
    check(Number.isSafeInteger(released) && released === row.allocatedBytesReleased, 'ALLOCATION_BINDING', p);
    return { owner: 'transport', physicalPath: p, physicalIdentityKey: identity(volume, before.fileId),
      allocationMethod: CLUSTER_METHOD, beforeAllocatedBytes: before.allocatedBytes,
      afterAllocatedBytes: after.allocatedBytes, observedReleasedBytes: released,
      byteSha256: before.sha256, logicalBytes: before.logicalBytes };
  });
  const before = total(entries.map(entry => entry.beforeAllocatedBytes));
  const after = total(entries.map(entry => entry.afterAllocatedBytes));
  check(before === done.allocatedBytesBefore && after === done.allocatedBytesAfter &&
    before - after === done.allocatedBytesReleased && total(entries.map(entry => entry.logicalBytes)) === done.logicalBytesBefore &&
    done.logicalBytesBefore === done.logicalBytesAfter, 'SUMMARY_BINDING', 'Per-file physical measurements must reproduce the small summary');
  return { schemaVersion: 1, decision: 'ORIGINAL_OWNER_PAIRED_CLUSTER_RECEIPTS_REVIEWED_NOT_ADMISSION',
    owner: 'transport', originalStartedUtc: plan.createdUtc, originalCompletedUtc: done.completedUtc,
    closedFileCount: entries.length, observedAllocationBeforeBytes: before, observedAllocationAfterBytes: after,
    ownerObservedNativeClusterReleaseBytes: before - after, uniqueWithinOwnerPhysicalIdentities: entries.length,
    crossOwnerDeduplicationComplete: false, completeCrossOwnerTotalReleasedBytes: null,
    bindings, entries, foreignArtifactReads: 0, compressionExecuted: false, capacityProbeExecuted: false,
    currentLeaseAbsenceProven: false, globalFreeSpaceLockAcquired: false, reclaimCreditedToCapacityBytes: 0,
    productionAdmission: false,
    nextStep: 'Aggregate only qualified unique physical identities; use fresh physical capacity and all phase bounds. Do not recompress to reconstruct a missing baseline.' };
}

function command(args) {
  const [mode, ...rest] = args;
  check(mode === 'transport', 'OPERATION_REFUSED', 'Original owner receipt review only');
  const allowed = ['--plan', '--plan-sha', '--rows', '--rows-sha', '--summary', '--summary-sha', '--owner-root'];
  const opts = {};
  check(rest.length === allowed.length * 2, 'ARGUMENT_REFUSED', 'All exact receipt names, digests and owner root required');
  for (let n = 0; n < rest.length; n += 2) {
    check(allowed.includes(rest[n]) && typeof rest[n + 1] === 'string' && !Object.hasOwn(opts, rest[n]),
      'ARGUMENT_REFUSED', 'Unique explicit option pairs required');
    opts[rest[n]] = rest[n + 1];
  }
  return reviewTransport(readJson(opts['--plan'], opts['--plan-sha']), readRows(opts['--rows'], opts['--rows-sha']),
    readJson(opts['--summary'], opts['--summary-sha']), opts['--owner-root'],
    { planSha256: opts['--plan-sha'], rowsSha256: opts['--rows-sha'], summarySha256: opts['--summary-sha'] });
}

module.exports = { CLUSTER_METHOD, parseJson, readRaw, readJson, readRows, identity, reviewTransport, command };

if (require.main === module) {
  try {
    process.stdout.write(JSON.stringify(command(process.argv.slice(2)), null, 2) + '\n');
  } catch (error) {
    process.stderr.write(JSON.stringify({ code: error.code || 'READ_FAILED', detail: error.message }) + '\n');
    process.exitCode = 2;
  }
}
