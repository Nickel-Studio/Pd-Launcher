'use strict';

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');

const W = path.win32;
const MiB = 1024 * 1024;
const RESERVE = Object.freeze({ incremental: 1024 * MiB, game: 4096 * MiB });
const LIMIT = Object.freeze({ text: 512 * 1024, entries: 2048, ageMs: 120000 });

class AuditError extends Error {
  constructor(code, detail) {
    super(detail);
    this.code = code;
  }
}

function requireThat(value, code, detail) {
  if (!value) throw new AuditError(code, detail);
}

function byte(value, name) {
  requireThat(Number.isSafeInteger(value) && value >= 0, 'BYTE_INVALID', name);
  return value;
}

function sum(values, name) {
  return byte(values.reduce((a, b) => a + byte(b, name), 0), name);
}

function absolute(value) {
  requireThat(typeof value === 'string' && /^[CD]:[\\/]/i.test(value),
    'PATH_ABSOLUTE', 'Only explicit local C/D drive paths are accepted');
  requireThat(!/[\x00-\x1f]/.test(value), 'PATH_INVALID', value);
  const parts = value.slice(3).split(/[\\/]/);
  for (const part of parts) {
    requireThat(part !== '..' && part !== '.' && !/[<>:"|?*]/.test(part) &&
      !/[ .]$/.test(part) && !/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(part),
    'PATH_BOUNDARY', value);
  }
  return W.normalize(value).replace(/\\$/, '').replace(/^([cd]):/i, (_, d) => d.toUpperCase() + ':');
}

function volume(value) {
  return absolute(value).slice(0, 1);
}

function inside(root, candidate) {
  const r = absolute(root).toLowerCase();
  const p = absolute(candidate).toLowerCase();
  return p === r || p.startsWith(r + '\\');
}

function age(stamp, now, name) {
  const at = Date.parse(stamp);
  requireThat(Number.isFinite(at) && at <= now && now - at <= LIMIT.ageMs,
    'EVIDENCE_STALE', name + ' must be at most 120 seconds old, not future dated');
}

function sha(value) {
  requireThat(typeof value === 'string' && /^[a-f0-9]{64}$/.test(value),
    'SHA_INVALID', 'Expected lower-case SHA-256');
  return value;
}

function authority(inv, owner, now) {
  requireThat(typeof owner === 'string' && /^[a-z0-9_-]{1,96}$/i.test(owner) &&
    inv && inv.schemaVersion === 1 && inv.ownerId === owner &&
    inv.authority && inv.authority.providedBy === owner &&
    inv.authority.scope === 'owner-attested' && inv.authority.foreignRecursiveRead === false,
  'OWNER_AUTHORITY', 'An explicit owner inventory is required; this is not an OS authorization grant');
  requireThat(inv.leaseEpoch && inv.readerLeaseInventoryComplete === true &&
    inv.writerInventoryComplete === true, 'LEASE_UNKNOWN', 'A complete lease/writer epoch is required');
  requireThat(Array.isArray(inv.sourceOwnedRoots) && inv.sourceOwnedRoots.length > 0 &&
    inv.sourceOwnedRoots.length <= 16 && typeof inv.targetOwnedRoot === 'string',
  'OWNER_ROOT_UNKNOWN', 'Explicit physical source roots and a pure-D target root are required');
  inv.sourceOwnedRoots.forEach(absolute);
  requireThat(volume(inv.targetOwnedRoot) === 'D', 'TARGET_C_BACKED', inv.targetOwnedRoot);
  age(inv.capturedAt, now, 'inventory');
}

function closed(entry, owner, epoch) {
  requireThat(entry.ownerId === owner && entry.leaseEpoch === epoch &&
    entry.closedAttestedBy === owner, 'OWNER_FOREIGN', entry.id);
  requireThat(entry.markedClosed === true && entry.protected !== true &&
    !['static-prefix', 'wsl-vhd', 'reader-output'].includes(entry.kind), 'NOT_MARKED_CLOSED', entry.id);
  requireThat(Array.isArray(entry.activeWriterPids) && entry.activeWriterPids.length === 0,
    'ACTIVE_WRITER', entry.id);
  requireThat(Array.isArray(entry.readerLeases) && entry.readerLeases.length === 0,
    'READER_LEASE', entry.id);
  requireThat(entry.hardlinkCount === 1, 'HARDLINK_UNKNOWN', entry.id);
}

function entryShape(entry) {
  requireThat(typeof entry.id === 'string' && /^[a-z0-9_-]{1,96}$/i.test(entry.id),
    'ENTRY_INVALID', 'Invalid stable inventory id');
  requireThat(['build', 'source', 'cache', 'static-prefix', 'wsl-vhd', 'reader-output'].includes(entry.kind),
    'ENTRY_INVALID', entry.id);
  absolute(entry.sourcePath);
  absolute(entry.targetPath);
  requireThat(volume(entry.targetPath) === 'D', 'TARGET_C_BACKED', entry.id);
  byte(entry.byte, 'byte');
  byte(entry.allocatedBytes, 'allocatedBytes');
  requireThat(entry.allocationEvidence === 'owner-native-allocated' &&
    entry.allocatedVolume === volume(entry.sourcePath) &&
    typeof entry.fileIdentity === 'string' && entry.fileIdentity.length > 0,
  'ALLOCATION_UNKNOWN', entry.id + ': logical length/guest df/rounded clusters are not allocation evidence');
  sha(entry.sha256);
}

function validateInventory(inv, owner, now = Date.now()) {
  authority(inv, owner, now);
  requireThat(Array.isArray(inv.entries) && inv.entries.length <= LIMIT.entries,
    'INVENTORY_LIMIT', 'At most 2048 explicitly enumerated closed files, no directory recursion');
  const ids = new Set();
  const physical = new Map();
  const totals = { C: 0, D: 0 };
  const build = { C: 0, D: 0 };
  const logical = { C: 0, D: 0 };
  for (const entry of inv.entries) {
    requireThat(inv.sourceOwnedRoots.some(root => inside(root, entry.sourcePath)) &&
      inside(inv.targetOwnedRoot, entry.targetPath), 'PATH_BOUNDARY', entry.id);
    entryShape(entry);
    closed(entry, owner, inv.leaseEpoch);
    requireThat(entry.sourceIsReparsePoint === false && entry.targetIsReparsePoint === false,
      'LINK_REFUSED', entry.id + ': link/reparse evidence must explicitly be false');
    requireThat(!ids.has(entry.id), 'ENTRY_DUPLICATE', entry.id);
    ids.add(entry.id);
    const key = entry.allocatedVolume + ':' + entry.fileIdentity;
    requireThat(!physical.has(key), 'IDENTITY_DUPLICATE', entry.id + ': no double counting hardlinks');
    physical.set(key, entry.id);
    const v = entry.allocatedVolume;
    totals[v] = sum([totals[v], entry.allocatedBytes], 'closed allocated total');
    logical[v] = sum([logical[v], entry.byte], 'logical total');
    if (entry.kind === 'build') build[v] = sum([build[v], entry.allocatedBytes], 'buildallocated');
  }
  return {
    ownerId: owner, closedFiles: inv.entries.length,
    markedClosedAllocatedBytes: totals, markedClosedBuildAllocatedBytes: build,
    logicalBytes: logical, allocationSource: 'owner inventory; no foreign filesystem traversal',
    reclaimCreditedBytes: 0,
    decision: 'INVENTORY_VALID_NOT_TRANSFER_OR_RESERVE_PERMISSION'
  };
}

function growth(previous, current, owner, now = Date.now()) {
  const a = validateInventory(previous, owner, now);
  const b = validateInventory(current, owner, now);
  requireThat(Date.parse(previous.capturedAt) <= Date.parse(current.capturedAt),
    'GROWTH_ORDER', 'Baseline must precede current sample');
  requireThat(previous.coverageId && previous.coverageId === current.coverageId &&
    previous.allocationInventoryComplete === true && current.allocationInventoryComplete === true,
  'GROWTH_COVERAGE', 'Growth needs identical complete marked-closed scope; two partial lists are not a bound');
  const delta = { C: 0, D: 0 };
  const positive = { C: 0, D: 0 };
  const buildDelta = { C: 0, D: 0 };
  for (const v of ['C', 'D']) {
    delta[v] = b.markedClosedAllocatedBytes[v] - a.markedClosedAllocatedBytes[v];
    positive[v] = Math.max(0, delta[v]);
    buildDelta[v] = b.markedClosedBuildAllocatedBytes[v] - a.markedClosedBuildAllocatedBytes[v];
  }
  return {
    observedAllocatedDeltaBytes: delta, positiveObservedGrowthBytes: positive,
    observedBuildAllocatedDeltaBytes: buildDelta,
    growthUpperBoundEstablished: false,
    note: 'Observed growth is not a forecast, and an inventory disappearance is not credited C free space'
  };
}

function windowsLinkTarget(value) {
  requireThat(typeof value === 'string', 'JUNCTION_TARGET', 'Missing link target');
  return absolute(value.replace(/^\\\\\?\\/, '').replace(/^\\\?\?\\/, ''));
}

function junction(record, requireD = false) {
  absolute(record.path);
  requireThat(record.linkType === 'Junction' && record.isReparsePoint === true &&
    Array.isArray(record.targets) && record.targets.length === 1,
  'JUNCTION_INVALID', 'Expected one junction target without following it');
  const actual = windowsLinkTarget(record.targets[0]);
  const expected = absolute(record.expectedTarget);
  requireThat(actual.toLowerCase() === expected.toLowerCase(), 'JUNCTION_MISMATCH', record.path);
  if (requireD) requireThat(volume(actual) === 'D', 'JUNCTION_C_BACKED', record.path);
  return { path: absolute(record.path), target: actual, physicalVolume: volume(actual),
    status: 'METADATA_ONLY_TARGET_MATCH_NO_TARGET_CONTENT_READ' };
}

function demandFor(plan, v) {
  const d = plan.demand && plan.demand[v];
  requireThat(d && d.bounded === true, 'GROWTH_UNBOUNDED', v + ': an owner upper bound is mandatory');
  return sum([d.growthUpperBytes, d.temporaryPeakBytes, d.outstandingHoldBytes,
    d.otherWriterUpperBytes], v + ' peak demand');
}

function reserve(capacity, plan, profile, now = Date.now()) {
  requireThat(Object.hasOwn(RESERVE, profile), 'RESERVE_PROFILE', 'Only original incremental/game profiles exist');
  requireThat(!Object.hasOwn(plan, 'reserveBytes') || plan.reserveBytes === RESERVE[profile],
    'RESERVE_DOWNGRADE', 'The original 1 GiB / 4 GiB reserve cannot be changed');
  age(capacity.capturedAt, now, 'DriveInfo');
  requireThat(capacity.source === 'DriveInfo.AvailableFreeSpace',
    'CAPACITY_SOURCE', 'Guest df and TotalFreeSpace are not current user host headroom');
  const floor = RESERVE[profile];
  const decisions = {};
  for (const v of ['C', 'D']) {
    const free = byte(capacity.drives && capacity.drives[v] && capacity.drives[v].availableBytes,
      v + ' AvailableFreeSpace');
    const peak = demandFor(plan, v);
    const required = sum([floor, peak], v + ' reserve plus peak demand');
    decisions[v] = { availableBytes: free, originalReserveBytes: floor, demandUpperBytes: peak,
      requiredBytes: required, shortfallBytes: Math.max(0, required - free), admitted: free >= required };
  }
  requireThat(plan.wsl && plan.wsl.inventoryComplete === true && Array.isArray(plan.wsl.disks),
    'WSL_BOUNDARY_UNKNOWN', 'All C-backed VHD writers must be owner-inventoried or physically D-bounded');
  let vhdC = 0;
  let vhdD = 0;
  for (const disk of plan.wsl.disks) {
    const v = volume(disk.physicalPath);
    byte(disk.hostAllocatedBytes, 'host VHD allocated bytes');
    requireThat(disk.allocationEvidence === 'owner-native-allocated',
      'VHD_ALLOCATION_UNKNOWN', 'Virtual size, logical VHD length and guest free are not host allocation');
    requireThat(disk.growthBounded === true, 'VHD_GROWTH_UNBOUNDED', disk.physicalPath);
    byte(disk.hostGrowthUpperBytes, 'host VHD growth upper bound');
    if (v === 'C') vhdC = sum([vhdC, disk.hostGrowthUpperBytes], 'C VHD growth');
    else vhdD = sum([vhdD, disk.hostGrowthUpperBytes], 'D VHD growth');
  }
  for (const [v, n] of [['C', vhdC], ['D', vhdD]]) {
    requireThat(plan.demand[v].otherWriterUpperBytes >= n, 'VHD_GROWTH_NOT_RESERVED', v);
  }
  return { profile, decision: decisions.C.admitted && decisions.D.admitted ? 'ADMITTED' : 'BLOCKED',
    volumes: decisions, vhdHostGrowthIncludedBytes: { C: vhdC, D: vhdD },
    scope: 'capacity-only; owner leases, publication approval and original build guards remain mandatory' };
}

function physicalDNoLinks(root, candidate, expectFile) {
  requireThat(process.platform === 'win32', 'HOST_UNSUPPORTED', 'Physical verification requires Windows');
  const r = absolute(root);
  const p = absolute(candidate);
  requireThat(volume(r) === 'D' && volume(p) === 'D' && inside(r, p) &&
    (!expectFile || p.toLowerCase() !== r.toLowerCase()),
    'PATH_BOUNDARY', 'Verification is confined to the explicitly owned pure-D root');
  const worktreePattern = /^(.*\\\.snow\\worktrees\\[^\\]+\\[^\\]+)(?:\\|$)/i;
  const targetWorktree = worktreePattern.exec(p);
  const toolWorktree = worktreePattern.exec(absolute(__dirname));
  requireThat(!targetWorktree || (toolWorktree &&
    targetWorktree[1].toLowerCase() === toolWorktree[1].toLowerCase()),
    'FOREIGN_WORKTREE', 'Only the executing owner worktree may be verified, never a foreign mirror');
  let partial = 'D:\\';
  const parts = p.slice(3).split('\\');
  for (let index = 0; index < parts.length; ++index) {
    partial = W.join(partial, parts[index]);
    const s = fs.lstatSync(partial, { bigint: true });
    requireThat(!s.isSymbolicLink(), 'LINK_REFUSED', partial);
    const last = index === parts.length - 1;
    requireThat(last && expectFile ? s.isFile() : s.isDirectory(), 'FILE_TYPE', partial);
  }
  const real = absolute(fs.realpathSync.native(p));
  requireThat(real.toLowerCase() === p.toLowerCase() && inside(r, real), 'LINK_REFUSED', p);
  return fs.lstatSync(p, { bigint: true });
}

function sameStat(a, b) {
  return a.dev === b.dev && a.ino === b.ino && a.size === b.size &&
    a.mtimeNs === b.mtimeNs && a.ctimeNs === b.ctimeNs;
}

function hashClosedD(entry, root) {
  const before = physicalDNoLinks(root, entry.targetPath, true);
  requireThat(before.size === BigInt(entry.byte), 'BYTE_MISMATCH', entry.id);
  requireThat(before.nlink === 1n, 'HARDLINK_UNKNOWN', entry.id);
  const fd = fs.openSync(entry.targetPath, fs.constants.O_RDONLY);
  try {
    const opened = fs.fstatSync(fd, { bigint: true });
    requireThat(sameStat(before, opened), 'IDENTITY_CHANGED', entry.id);
    physicalDNoLinks(root, entry.targetPath, true);
    const h = crypto.createHash('sha256');
    const buffer = Buffer.alloc(64 * 1024);
    let count = 0;
    for (;;) {
      const n = fs.readSync(fd, buffer, 0, buffer.length, null);
      if (n === 0) break;
      count = sum([count, n], 'hashed bytes');
      requireThat(count <= entry.byte, 'BYTE_MISMATCH', entry.id);
      h.update(buffer.subarray(0, n));
    }
    requireThat(count === entry.byte, 'BYTE_MISMATCH', entry.id);
    requireThat(h.digest('hex') === entry.sha256, 'HASH_MISMATCH', entry.id);
    const after = physicalDNoLinks(root, entry.targetPath, true);
    requireThat(sameStat(opened, fs.fstatSync(fd, { bigint: true })) && sameStat(before, after),
      'IDENTITY_CHANGED', entry.id);
    return { id: entry.id, byte: count, sha256: entry.sha256, status: 'VERIFIED_D_TARGET_ONLY' };
  } finally {
    fs.closeSync(fd);
  }
}

function verify(inv, root, owner, now = Date.now()) {
  validateInventory(inv, owner, now);
  requireThat(inv.transferVerificationConsent === true &&
    inv.targetOwnedRoot && absolute(inv.targetOwnedRoot).toLowerCase() === absolute(root).toLowerCase(),
  'VERIFY_AUTHORITY', 'Owner must explicitly authorize a pure-D target capability root');
  const results = inv.entries.map(entry => hashClosedD(entry, root));
  return { decision: 'VERIFIED_NOT_CLEANUP_PERMISSION', leaseEpoch: inv.leaseEpoch,
    sourceFilesRead: 0, targetFiles: results };
}

function readText(name) {
  requireThat(typeof name === 'string' && !path.isAbsolute(name) && !W.isAbsolute(name) &&
    !name.split(/[\\/]/).some(part => part === '..'),
    'PATH_BOUNDARY', 'JSON inputs must be relative to the tool directory, without parent traversal');
  const p = path.resolve(__dirname, name);
  const before = physicalDNoLinks(__dirname, p, true);
  requireThat(before.size <= BigInt(LIMIT.text), 'TEXT_LIMIT', name);
  const fd = fs.openSync(p, fs.constants.O_RDONLY);
  try {
    const opened = fs.fstatSync(fd, { bigint: true });
    requireThat(sameStat(before, opened), 'IDENTITY_CHANGED', name);
    const buffer = Buffer.alloc(LIMIT.text + 1);
    let size = 0;
    for (;;) {
      const n = fs.readSync(fd, buffer, size, buffer.length - size, null);
      if (!n) break;
      size += n;
      requireThat(size <= LIMIT.text, 'TEXT_LIMIT', name);
    }
    const after = physicalDNoLinks(__dirname, p, true);
    requireThat(sameStat(opened, fs.fstatSync(fd, { bigint: true })) && sameStat(before, after),
      'IDENTITY_CHANGED', name);
    try { return JSON.parse(buffer.subarray(0, size).toString('utf8').replace(/^\uFEFF/, '')); }
    catch (_) { throw new AuditError('JSON_INVALID', name); }
  } finally {
    fs.closeSync(fd);
  }
}

function liveCapacity() {
  const script = W.join(__dirname, 'capacity.ps1');
  physicalDNoLinks(__dirname, script, true);
  const powershell = W.join(process.env.SystemRoot || 'C:\\Windows',
    'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
  const processResult = spawnSync(powershell, ['-NoLogo', '-NoProfile', '-NonInteractive',
    '-File', script], { cwd: __dirname, encoding: 'utf8', timeout: 15000, maxBuffer: 64 * 1024 });
  requireThat(processResult.status === 0 && !processResult.error,
    'LIVE_CAPACITY_FAILED', 'The immediate read-only DriveInfo probe must succeed');
  let capacity;
  try { capacity = JSON.parse(processResult.stdout.replace(/^\uFEFF/, '')); }
  catch (_) { throw new AuditError('LIVE_CAPACITY_FAILED', 'Invalid DriveInfo JSON'); }
  requireThat(capacity.recursiveReads === 0 && capacity.writes === 0,
    'LIVE_CAPACITY_FAILED', 'Metadata-only probe contract');
  return capacity;
}

function beforeWriteReserve(inv, plan, profile) {
  const owner = inv.ownerId;
  validateInventory(inv, owner);
  requireThat(plan.ownerId === owner && plan.leaseEpoch === inv.leaseEpoch,
    'PLAN_OWNER_LEASE', 'The bounded demand plan must bind the same owner/lease epoch');
  age(plan.capturedAt, Date.now(), 'owner demand plan');
  const capacity = liveCapacity();
  const now = Date.now();
  age(plan.capturedAt, now, 'owner demand plan after probe');
  requireThat(now - Date.parse(capacity.capturedAt) <= 5000,
    'LIVE_CAPACITY_FAILED', 'Probe must return within five seconds of sampling');
  validateInventory(inv, owner, now);
  const result = reserve(capacity, plan, profile, now);
  return { ...result, realTimeDriveInfoProbe: true, capturedAt: capacity.capturedAt,
    cachedCapacityIgnored: true, leaseEpoch: inv.leaseEpoch, productionAdmission: false,
    nextStep: 'Owner must bind this sample to its immediate write gate; no global free-space lock is acquired' };
}

function command(args) {
  const [mode, ...rest] = args;
  requireThat(['inventory', 'growth', 'junction', 'reserve', 'verify'].includes(mode),
    'OPERATION_REFUSED', 'Read-only inventory/growth/junction/reserve/verify only; never cleanup/move/compile');
  const opts = {};
  for (let i = 0; i < rest.length; i += 2) {
    const key = rest[i];
    requireThat(['--input', '--baseline', '--owner', '--profile', '--owned-root'].includes(key) &&
      typeof rest[i + 1] === 'string' && !rest[i + 1].startsWith('--') && !Object.hasOwn(opts, key),
    'ARGUMENT_REFUSED', 'Unknown, duplicate, incomplete or quota-changing argument');
    opts[key] = rest[i + 1];
  }
  const input = readText(opts['--input']);
  if (mode === 'inventory') return validateInventory(input, opts['--owner']);
  if (mode === 'growth') return growth(readText(opts['--baseline']), input, opts['--owner']);
  if (mode === 'junction') return junction(input.record, input.requireDTarget === true);
  if (mode === 'reserve') {
    requireThat(input.inventory.ownerId === opts['--owner'], 'OWNER_AUTHORITY', 'Explicit owner mismatch');
    return beforeWriteReserve(input.inventory, input.plan, opts['--profile']);
  }
  return verify(input, opts['--owned-root'], opts['--owner']);
}

module.exports = { AuditError, RESERVE, LIMIT, absolute, inside, validateInventory, growth,
  junction, reserve, verify, physicalDNoLinks, liveCapacity, beforeWriteReserve, command };

if (require.main === module) {
  try {
    const result = command(process.argv.slice(2));
    process.stdout.write(JSON.stringify(result, null, 2) + '\n');
    if (result.decision === 'BLOCKED') process.exitCode = 3;
  } catch (error) {
    process.stdout.write(JSON.stringify({ decision: 'REFUSED', code: error.code || 'IO_ERROR',
      detail: error.message }, null, 2) + '\n');
    process.exitCode = 2;
  }
}
