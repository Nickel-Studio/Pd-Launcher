'use strict';

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const audit = require('./storage-audit.cjs');

const root = __dirname;
const fixtureRoot = path.join(root, 'fixtures');
const owner = 'build-storage';
const epoch = 'small-d-fixture-epoch';
const now = Date.now();
const stamp = new Date(now).toISOString();
const payload = path.join(fixtureRoot, 'payload.txt');
const runtime = path.join(fixtureRoot, 'runtime-' + process.pid);
const resultName = process.argv[2] || 'results-final.json';
const digest = data => crypto.createHash('sha256').update(data).digest('hex');
const clone = value => JSON.parse(JSON.stringify(value));

function confined(file) {
  return file.startsWith(root + path.sep) && !file.slice(root.length + 1).split(path.sep).includes('..');
}

function newText(file, value) {
  assert.ok(confined(file));
  audit.physicalDNoLinks(root, path.dirname(file), false);
  fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n', { flag: 'wx' });
}

function refusal(code, run) {
  assert.throws(run, error => error.code === code, 'Expected fail-closed ' + code);
}

function inventory(file = payload) {
  audit.physicalDNoLinks(root, file, true);
  const data = fs.readFileSync(file);
  return {
    schemaVersion: 1, ownerId: owner, capturedAt: stamp,
    authority: { providedBy: owner, scope: 'owner-attested', foreignRecursiveRead: false },
    readerLeaseInventoryComplete: true, writerInventoryComplete: true, leaseEpoch: epoch,
    sourceOwnedRoots: [fixtureRoot], targetOwnedRoot: fixtureRoot,
    transferVerificationConsent: true, coverageId: 'tiny-files-only', allocationInventoryComplete: true,
    entries: [{
      id: path.basename(file).replace(/\./g, '_'), kind: 'build', ownerId: owner,
      closedAttestedBy: owner, leaseEpoch: epoch, markedClosed: true, protected: false,
      sourcePath: file, targetPath: file, sourceIsReparsePoint: false, targetIsReparsePoint: false,
      byte: data.length, allocatedBytes: 4096, allocatedVolume: 'D',
      allocationEvidence: 'owner-native-allocated', fileIdentity: 'fixture-owner-attested-1',
      hardlinkCount: 1, sha256: digest(data), activeWriterPids: [], readerLeases: []
    }]
  };
}

function capacity(c = audit.RESERVE.game, d = audit.RESERVE.game) {
  return { capturedAt: stamp, source: 'DriveInfo.AvailableFreeSpace',
    drives: { C: { availableBytes: c }, D: { availableBytes: d } } };
}

function plan() {
  const demand = { bounded: true, growthUpperBytes: 0, temporaryPeakBytes: 0,
    outstandingHoldBytes: 0, otherWriterUpperBytes: 0 };
  return { demand: { C: clone(demand), D: clone(demand) },
    wsl: { inventoryComplete: true, disks: [] } };
}

function allFiles(dir) {
  audit.physicalDNoLinks(root, dir, false);
  const result = [];
  for (const name of fs.readdirSync(dir)) {
    const file = path.join(dir, name);
    const stat = fs.lstatSync(file);
    assert.ok(!stat.isSymbolicLink(), 'No live links retained in fixture bundle');
    if (stat.isDirectory()) result.push(...allFiles(file));
    else { audit.physicalDNoLinks(root, file, true); result.push(file); }
  }
  return result;
}

function liveProbeFixture() {
  const inv = inventory();
  const p = plan();
  p.ownerId = owner;
  p.leaseEpoch = epoch;
  p.capturedAt = stamp;
  p.demand.C.growthUpperBytes = Number.MAX_SAFE_INTEGER - audit.RESERVE.game;
  const optimisticCached = capacity(Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER);
  const packet = { inventory: inv, plan: p, capacity: optimisticCached };
  const file = path.join(runtime, 'live-reserve.json');
  newText(file, packet);
  const result = audit.command(['reserve', '--input', path.relative(root, file), '--owner', owner,
    '--profile', 'game']);
  assert.equal(result.realTimeDriveInfoProbe, true);
  assert.equal(result.cachedCapacityIgnored, true);
  assert.equal(result.decision, 'BLOCKED');
  assert.notEqual(result.volumes.C.availableBytes, Number.MAX_SAFE_INTEGER);
  assert.equal(result.productionAdmission, false);
  return result;
}

function main() {
  assert.equal(process.platform, 'win32');
  assert.match(resultName, /^results(?:-[a-z0-9-]+)?\.json$/i);
  audit.physicalDNoLinks(root, payload, true);
  assert.equal(fs.existsSync(runtime), false, 'Each run needs a new exclusively created D runtime');
  fs.mkdirSync(runtime);
  const fixtures = allFiles(fixtureRoot).filter(file => /^\d\d-.*\.json$/.test(path.basename(file)));
  const cases = fixtures.sort().map(file => {
    const text = fs.readFileSync(file, 'utf8');
    return { file, definition: JSON.parse(text), sha256: digest(text) };
  });
  assert.equal(cases.length, 12);
  const result = [];
  for (const item of cases) {
    const test = item.definition;
    const started = process.hrtime.bigint();
    try {
      const inv = inventory();
      switch (test.id) {
        case 'closed-stats-growth': {
          assert.equal(audit.validateInventory(inv, owner, now).markedClosedBuildAllocatedBytes.D, 4096);
          const current = clone(inv);
          current.entries[0].allocatedBytes = 8192;
          const g = audit.growth(inv, current, owner, now);
          assert.equal(g.positiveObservedGrowthBytes.D, 4096);
          assert.equal(g.growthUpperBoundEstablished, false);
          current.coverageId = 'partial-other-scope';
          refusal('GROWTH_COVERAGE', () => audit.growth(inv, current, owner, now));
          const invalid = clone(inv);
          invalid.entries[0].allocatedBytes = Number.MAX_SAFE_INTEGER + 1;
          refusal('BYTE_INVALID', () => audit.validateInventory(invalid, owner, now));
          break;
        }
        case 'byte-sha-ok': {
          const verified = audit.verify(inv, fixtureRoot, owner, now);
          assert.equal(verified.sourceFilesRead, 0);
          assert.equal(verified.targetFiles[0].sha256, inv.entries[0].sha256);
          assert.equal(audit.verify(inventory(path.join(fixtureRoot, 'empty.txt')), fixtureRoot,
            owner, now).targetFiles[0].byte, 0);
          newText(path.join(runtime, 'inventory.json'), inv);
          const input = path.relative(root, path.join(runtime, 'inventory.json'));
          const cli = spawnSync(process.execPath, [path.join(root, 'storage-audit.cjs'), 'verify',
            '--input', input, '--owner', owner, '--owned-root', fixtureRoot],
          { cwd: root, encoding: 'utf8' });
          assert.equal(cli.status, 0, cli.stdout + cli.stderr);
          assert.equal(JSON.parse(cli.stdout).decision, 'VERIFIED_NOT_CLEANUP_PERMISSION');
          break;
        }
        case 'path-boundary': {
          inv.entries[0].targetPath = path.join(root, 'storage-audit.cjs');
          refusal('PATH_BOUNDARY', () => audit.verify(inv, fixtureRoot, owner, now));
          for (const value of ['D:\\owned\\..\\foreign\\file.txt',
            'D:\\owned\\file.txt:stream', 'D:\\owned\\NUL', 'D:relative', '\\\\server\\share\\file']) {
            assert.throws(() => audit.absolute(value));
          }
          assert.equal(audit.inside('D:\\owned', 'D:\\owned-other\\file'), false);
          refusal('PATH_BOUNDARY', () => audit.command(['inventory', '--input', '../foreign.json', '--owner', owner]));
          const external = inventory();
          external.entries[0].sourcePath = 'C:\\foreign\\file.txt';
          refusal('PATH_BOUNDARY', () => audit.validateInventory(external, owner, now));
          break;
        }
        case 'real-junction': {
          const link = path.join(runtime, 'junction');
          const target = path.join(fixtureRoot, 'link-target');
          const protectedLeaf = path.join(target, 'leaf.txt');
          const before = digest(fs.readFileSync(protectedLeaf));
          fs.symlinkSync(target, link, 'junction');
          try {
            inv.entries[0].targetPath = path.join(link, 'leaf.txt');
            refusal('LINK_REFUSED', () => audit.verify(inv, fixtureRoot, owner, now));
          } finally {
            assert.ok(fs.lstatSync(link).isSymbolicLink());
            fs.unlinkSync(link);
          }
          assert.equal(digest(fs.readFileSync(protectedLeaf)), before);
          break;
        }
        case 'active-writer': {
          inv.entries[0].activeWriterPids = [process.pid];
          refusal('ACTIVE_WRITER', () => audit.verify(inv, fixtureRoot, owner, now));
          inv.entries[0].activeWriterPids = [];
          inv.entries[0].markedClosed = false;
          refusal('NOT_MARKED_CLOSED', () => audit.validateInventory(inv, owner, now));
          break;
        }
        case 'reader-lease': {
          inv.entries[0].readerLeases = [{ ownerId: 'acceptance-resume', token: 'reader-open' }];
          refusal('READER_LEASE', () => audit.verify(inv, fixtureRoot, owner, now));
          inv.entries[0].readerLeases = [];
          inv.readerLeaseInventoryComplete = false;
          refusal('LEASE_UNKNOWN', () => audit.validateInventory(inv, owner, now));
          break;
        }
        case 'hash-byte-bad': {
          inv.entries[0].sha256 = '0'.repeat(64);
          refusal('HASH_MISMATCH', () => audit.verify(inv, fixtureRoot, owner, now));
          inv.entries[0].sha256 = inventory().entries[0].sha256;
          inv.entries[0].byte += 1;
          refusal('BYTE_MISMATCH', () => audit.verify(inv, fixtureRoot, owner, now));
          break;
        }
        case 'foreign-cleanup-refused': {
          const before = digest(fs.readFileSync(payload));
          refusal('OPERATION_REFUSED', () => audit.command(['cleanup', '--input', 'C:\\foreign\\file']));
          refusal('OPERATION_REFUSED', () => audit.command(['move', '--input', 'C:\\foreign\\file']));
          inv.entries[0].ownerId = 'foreign-owner';
          refusal('OWNER_FOREIGN', () => audit.verify(inv, fixtureRoot, owner, now));
          assert.equal(digest(fs.readFileSync(payload)), before);
          break;
        }
        case 'reserve-original-boundary': {
          for (const profile of ['incremental', 'game']) {
            const n = audit.RESERVE[profile];
            assert.equal(audit.reserve(capacity(n, n), plan(), profile, now).decision, 'ADMITTED');
            const blocked = audit.reserve(capacity(n - 1, n), plan(), profile, now);
            assert.equal(blocked.decision, 'BLOCKED');
            assert.equal(blocked.volumes.C.shortfallBytes, 1);
            assert.equal(audit.reserve(capacity(n, n - 1), plan(), profile, now).decision, 'BLOCKED');
          }
          const highPeak = plan();
          highPeak.demand.C.temporaryPeakBytes = 4096;
          assert.equal(audit.reserve(capacity(), highPeak, 'game', now).decision, 'BLOCKED');
          break;
        }
        case 'reserve-downgrade-stale': {
          const p = plan();
          p.reserveBytes = 128 * 1024 * 1024;
          refusal('RESERVE_DOWNGRADE', () => audit.reserve(capacity(), p, 'incremental', now));
          const old = capacity();
          old.capturedAt = new Date(now - 120001).toISOString();
          refusal('EVIDENCE_STALE', () => audit.reserve(old, plan(), 'game', now));
          const p2 = plan();
          p2.demand.C.bounded = false;
          refusal('GROWTH_UNBOUNDED', () => audit.reserve(capacity(), p2, 'game', now));
          break;
        }
        case 'junction-target': {
          const record = { path: 'D:\\owned\\logical', linkType: 'Junction', isReparsePoint: true,
            targets: ['C:\\owned\\physical'], expectedTarget: 'C:\\owned\\physical' };
          assert.equal(audit.junction(record).physicalVolume, 'C');
          refusal('JUNCTION_C_BACKED', () => audit.junction(record, true));
          record.expectedTarget = 'D:\\owned\\physical';
          refusal('JUNCTION_MISMATCH', () => audit.junction(record));
          record.targets = ['\\??\\D:\\owned\\physical'];
          assert.equal(audit.junction(record, true).physicalVolume, 'D');
          break;
        }
        case 'vhd-host-accounting': {
          const p = plan();
          p.wsl.disks = [{ physicalPath: 'C:\\owned\\ext4.vhdx', hostAllocatedBytes: 8192,
            allocationEvidence: 'owner-native-allocated', growthBounded: true, hostGrowthUpperBytes: 4096 }];
          refusal('VHD_GROWTH_NOT_RESERVED', () => audit.reserve(capacity(), p, 'game', now));
          p.demand.C.otherWriterUpperBytes = 4096;
          const r = audit.reserve(capacity(), p, 'game', now);
          assert.equal(r.volumes.C.shortfallBytes, 4096);
          p.wsl.disks[0].growthBounded = false;
          refusal('VHD_GROWTH_UNBOUNDED', () => audit.reserve(capacity(), p, 'game', now));
          p.wsl.disks[0].growthBounded = true;
          p.wsl.disks[0].allocationEvidence = 'guest-df';
          refusal('VHD_ALLOCATION_UNKNOWN', () => audit.reserve(capacity(), p, 'game', now));
          break;
        }
        default: throw new Error('Unknown fixture ' + test.id);
      }
      result.push({ id: test.id, decision: 'PASS', fixtureSha256: item.sha256,
        elapsedMs: Number(process.hrtime.bigint() - started) / 1e6 });
    } catch (error) {
      result.push({ id: test.id, decision: 'FAIL', fixtureSha256: item.sha256,
        error: error.stack || error.message });
    }
  }
  const bundle = allFiles(root);
  const bytes = bundle.reduce((n, file) => n + fs.statSync(file).size, 0);
  assert.ok(bytes <= 2 * 1024 * 1024, 'Owned tool and all evidence exceed the 2 MiB cap');
  const frozen = bundle.sort().map(file => ({ path: path.relative(root, file).replace(/\\/g, '/'),
    byte: fs.statSync(file).size, sha256: digest(fs.readFileSync(file)) }));
  const output = { schemaVersion: 1, runAt: new Date().toISOString(), host: {
    platform: process.platform, node: process.version, pid: process.pid, ownedRoot: root },
    fixtures: result, pass: result.filter(r => r.decision === 'PASS').length,
    fail: result.filter(r => r.decision === 'FAIL').length, bundleBytesBeforeResult: bytes,
    capBytes: 2097152, sourceArtifactReads: 0, foreignRecursiveReads: 0,
    builds: 0, installs: 0, wslInvocations: 0, foreignWrites: 0, foreignProcessStops: 0,
    allocationNumbersInFixturesAreSynthetic: true, frozenFiles: frozen };
  const resultPath = path.join(root, resultName);
  newText(resultPath, output);
  assert.ok(bytes + fs.statSync(resultPath).size <= 2097152);
  process.stdout.write(JSON.stringify(output, null, 2) + '\n');
  process.exitCode = output.fail ? 1 : 0;
}

try {
  main();
  if (!process.exitCode) {
    const live = liveProbeFixture();
    const result = JSON.parse(fs.readFileSync(path.join(root, resultName), 'utf8'));
    result.liveBeforeWriteReserveFixture = { ...live, fixtureDecision: 'PASS' };
    const probePath = path.join(root, resultName.replace('.json', '-probe.json'));
    newText(probePath, result);
    const total = allFiles(root).reduce((n, file) => n + fs.statSync(file).size, 0);
    assert.ok(total <= 2097152, 'Full D tool/fixture/receipt tree must remain <=2 MiB');
    process.stdout.write(JSON.stringify({ liveBeforeWriteReserveFixture: 'PASS',
      liveCapacityCapturedAt: live.capturedAt, cachedCapacityIgnored: true, fullTreeBytes: total }) + '\n');
  }
} catch (error) {
  process.stderr.write((error.stack || error.message) + '\n');
  process.exitCode = 1;
}
