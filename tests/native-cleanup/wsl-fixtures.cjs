'use strict';

const prefix = 'palladium-cleanup-';
const budget = 1024 * 1024;

function prepare(tool, owner) {
  if (!/^[A-Za-z0-9_-]{1,96}$/.test(owner)) throw new Error('Invalid native fixture owner');
  const filesystem = tool('stat', ['-f', '-c', '%T', '/dev/shm']).stdout.trim();
  if (filesystem !== 'tmpfs') throw new Error('Native WSL fixtures require the explicit tmpfs filesystem');
  const script = `import json, os, pathlib, stat, sys, tempfile
owner = sys.argv[1]
base = pathlib.Path('/dev/shm')
assert base.is_dir() and not base.is_symlink() and base.resolve() == base
volume = os.statvfs(base)
free = volume.f_bavail * volume.f_frsize
assert free >= 256 * 1024 * 1024 + ${budget}, 'beforewrite: insufficient native fixture reserve'
root = pathlib.Path(tempfile.mkdtemp(prefix='${prefix}' + owner + '-', dir=base))
s = root.lstat()
assert stat.S_ISDIR(s.st_mode) and s.st_uid == os.geteuid() and (s.st_mode & 0o77) == 0
marker = root / '.palladium-cleanup-owner'
fd = os.open(marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
with os.fdopen(fd, 'wb') as output:
    output.write(owner.encode('ascii'))
print('FIXTURE_READY ' + json.dumps({'path': str(root), 'owner': owner, 'mode': oct(stat.S_IMODE(s.st_mode)), 'uid': s.st_uid, 'freeBytesBefore': free, 'budgetBytes': ${budget}}))
`;
  const result = tool('python3', ['-c', script, owner]);
  const line = result.stdout.split(/\r?\n/).find(value => value.startsWith('FIXTURE_READY '));
  if (!line) throw new Error('Native fixture publication did not complete');
  const fixture = JSON.parse(line.slice('FIXTURE_READY '.length));
  if (!fixture.path.startsWith(`/dev/shm/${prefix}${owner}-`) || fixture.path.includes('/../') || fixture.owner !== owner) {
    throw new Error('Native fixture path or ownership mismatch');
  }
  return { ...fixture, filesystem, scope: 'Explicit runtime-only memory fixture exception; build, source, logs and TEMP remain in the owned D worktree' };
}

function auditAndRelease(tool, fixture) {
  const script = `import hashlib, json, os, pathlib, stat, sys
root = pathlib.Path(sys.argv[1])
owner = sys.argv[2]
assert root.parent == pathlib.Path('/dev/shm') and root.name.startswith('${prefix}' + owner + '-')
s = root.lstat()
assert stat.S_ISDIR(s.st_mode) and s.st_uid == os.geteuid() and (s.st_mode & 0o77) == 0
marker = root / '.palladium-cleanup-owner'
fd = os.open(marker, os.O_RDONLY | os.O_NOFOLLOW)
with os.fdopen(fd, 'rb') as content:
    assert content.read(97) == owner.encode('ascii'), 'native fixture ownership changed'
entries = list(root.iterdir())
empty = len(entries) == 1 and entries[0] == marker
result = {'path': str(root), 'removed': False, 'retained': not empty, 'bytes': 0, 'files': 0, 'links': 0, 'directories': 0, 'entries': 0, 'budgetBytes': ${budget}, 'fingerprints': []}
pending = [root]
while pending:
    directory = pending.pop()
    with os.scandir(directory) as children:
        for entry in children:
            if pathlib.Path(entry.path) == marker:
                continue
            result['entries'] += 1
            assert result['entries'] <= 10000, 'native fixture entry budget exceeded'
            metadata = entry.stat(follow_symlinks=False)
            if stat.S_ISLNK(metadata.st_mode):
                result['links'] += 1
            elif stat.S_ISDIR(metadata.st_mode):
                result['directories'] += 1
                pending.append(pathlib.Path(entry.path))
            elif stat.S_ISREG(metadata.st_mode):
                result['bytes'] += metadata.st_size
                result['files'] += 1
                assert result['bytes'] <= ${budget}, 'retained native fixture byte budget exceeded'
                file_fd = os.open(entry.path, os.O_RDONLY | os.O_NOFOLLOW)
                with os.fdopen(file_fd, 'rb') as content:
                    opened = os.fstat(content.fileno())
                    assert (opened.st_dev, opened.st_ino) == (metadata.st_dev, metadata.st_ino)
                    data = content.read(metadata.st_size + 1)
                    assert len(data) == metadata.st_size
                if len(result['fingerprints']) < 128:
                    result['fingerprints'].append({'path': str(pathlib.Path(entry.path).relative_to(root)), 'bytes': metadata.st_size, 'sha256': hashlib.sha256(data).hexdigest()})
            else:
                raise RuntimeError('unexpected native fixture entry type')
if empty:
    marker.unlink()
    root.rmdir()
    result['removed'] = True
print('FIXTURE_AUDIT ' + json.dumps(result))
`;
  const result = tool('python3', ['-c', script, fixture.path, fixture.owner]);
  const line = result.stdout.split(/\r?\n/).find(value => value.startsWith('FIXTURE_AUDIT '));
  if (!line) throw new Error('Native fixture audit did not complete');
  const audit = JSON.parse(line.slice('FIXTURE_AUDIT '.length));
  if (audit.path !== fixture.path || audit.bytes > budget || (!audit.removed && !audit.retained)) throw new Error('Native fixture audit mismatch');
  return audit;
}

module.exports = { prepare, auditAndRelease };
