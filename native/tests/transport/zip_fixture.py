import argparse
import io
import os
import pathlib
import struct
import subprocess
import sys
import uuid
import zipfile


def make(entries, compression=zipfile.ZIP_STORED):
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w', compression=compression) as archive:
        for name, body in entries:
            archive.writestr(name, body)
    return bytearray(data.getvalue())


def host_volume_folds_case(root):
    """True when the volume behind `root` treats case-distinct names as one entry."""
    if sys.platform == 'win32':
        return True
    tag = uuid.uuid4().hex
    lower = root / ('nickel-case-probe-' + tag + '-a')
    upper = root / ('nickel-case-probe-' + tag + '-A')
    try:
        first = os.open(lower, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except OSError:
        return True  # probe unavailable: conservative byte-exact decision, like the C++ fallback
    try:
        try:
            second = os.open(upper, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            os.close(second)
            sensitive = True
        except FileExistsError:
            sensitive = False
        except OSError:
            return True  # unexpected probe failure: keep the byte-exact decision
        return not sensitive
    finally:
        for leftover in (lower, upper):
            try:
                leftover.unlink()
            except OSError:
                pass


def run(executable, root):
    root.mkdir(parents=True, exist_ok=True)
    cases = []
    cases.append(('stored', True, make([('mods/a.jar', b'abc'), ('config/file', b'hello')])))
    cases.append(('deflated', True, make([('mods/a.jar', b'abcd' * 1024)], zipfile.ZIP_DEFLATED)))
    cases.append(('empty', True, make([])))
    cases.append(('deflated-empty-directory', True, make([('empty/', b'')], zipfile.ZIP_DEFLATED)))
    cases.append(('directory-payload', False, make([('empty/', b'hostile')], zipfile.ZIP_DEFLATED)))
    for name, path, contents, attributes, accepted in (
        ('legacy-unix-empty-dir', 'legacy/', b'', 0xffff0010, True),
        ('legacy-unix-dir-payload', 'legacy/', b'hostile', 0xffff0010, False),
        ('legacy-unix-dir-without-slash', 'legacy', b'', 0xffff0010, False),
        ('legacy-unix-dir-without-dos-bit', 'legacy/', b'', 0xffff0000, False),
        ('symlink-directory', 'legacy/', b'', (0o120777 << 16) | 0x10, False),
        ('device-directory', 'legacy/', b'', (0o020777 << 16) | 0x10, False)):
        value = make([(path, contents)])
        central = value.index(b'PK\x01\x02')
        struct.pack_into('<H', value, central + 4, (3 << 8) | 20)
        struct.pack_into('<I', value, central + 38, attributes)
        cases.append((name, accepted, value))
    cases.append(('unicode', True, make([('config/\u914d\u7f6e.txt', b'ok')])))
    for name in ['../outside', '/absolute', '//server/share', 'C:/drive', 'NUL.txt', 'a//b', 'a./b', 'a /b', 'a:stream']:
        cases.append(('path-' + str(len(cases)), False, make([(name, b'x')])))
    # Python's Windows ZIP writer normalizes backslashes; patch both headers to keep the attack real.
    bad = make([('a/b', b'x')]); bad = bad.replace(b'a/b', b'a\\b')
    cases.append(('path-backslash', False, bad))
    # Case-distinct members are distinct entries on byte-exact volumes and a conflict
    # on case-folding volumes; probe the actual host volume instead of assuming it.
    cases.append(('case', not host_volume_folds_case(root), make([('mods/a', b'a'), ('mods/A', b'b')])))
    cases.append(('duplicate', False, make([('mods/a', b'a'), ('mods/a', b'b')])))
    bad = make([('mods/a', b'a')])
    central = bad.index(b'PK\x01\x02')
    struct.pack_into('<I', bad, central + 38, 0o120777 << 16)
    cases.append(('symlink', False, bad))
    bad = make([('mods/a', b'a')])
    central = bad.index(b'PK\x01\x02')
    struct.pack_into('<H', bad, 6, 1)
    struct.pack_into('<H', bad, central + 8, 1)
    cases.append(('encrypted', False, bad))
    bad = make([('mods/a', b'a')]); bad[30] = ord('M')
    cases.append(('local-path-mismatch', False, bad))
    bad = make([('mods/a', b'a')]); central = bad.index(b'PK\x01\x02')
    struct.pack_into('<I', bad, central + 24, 600 * 1024 * 1024)
    cases.append(('size-quota', False, bad))
    bad = make([('mods/a', b'a'), ('mods/b', b'b')]); central = bad.index(b'PK\x01\x02')
    second_central = bad.index(b'PK\x01\x02', central + 4)
    offset = struct.unpack_from('<I', bad, second_central + 42)[0]
    struct.pack_into('<I', bad, second_central + 42, 0)
    cases.append(('overlap', False, bad))
    bad = make([('mods/a', b'a')]); end = bad.index(b'PK\x05\x06')
    struct.pack_into('<H', bad, end + 8, 0); struct.pack_into('<H', bad, end + 10, 0)
    cases.append(('hidden-central', False, bad))
    bad = make([('mods/a', b'a')]); bad.extend(b'junk')
    cases.append(('trailing-data', False, bad))
    cases.append(('bomb', False, make([('config/bomb', b'a' * (4 * 1024 * 1024))], zipfile.ZIP_DEFLATED)))
    for name, accepted, data in cases:
        path = root / (name + '.zip'); path.write_bytes(data)
        result = subprocess.run([str(executable), 'accept' if accepted else 'reject', str(path)], capture_output=True, text=True, timeout=20)
        print(name + ': ' + result.stdout.strip())
        if result.returncode:
            print(result.stderr)
            return result.returncode
    print('PASS ZIP local/central/hidden/overlap/path/link/encryption/size/bomb cases=' + str(len(cases)))
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True, type=pathlib.Path)
    parser.add_argument('--workspace', required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    raise SystemExit(run(arguments.executable.resolve(), arguments.workspace.resolve()))
