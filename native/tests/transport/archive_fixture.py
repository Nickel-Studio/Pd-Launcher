import argparse
import bz2
from compression import zstd
import gzip
import io
import lzma
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tarfile
import zipfile


def zip_data(entries, compression=zipfile.ZIP_DEFLATED):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=compression) as output:
        for name, content in entries:
            output.writestr(name, content)
    return bytearray(stream.getvalue())


def patch_name(data, original, replacement):
    assert len(original) == len(replacement)
    assert data.count(original) == 2
    return data.replace(original, replacement)


def tar_data(entries):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w", format=tarfile.USTAR_FORMAT) as output:
        for name, kind, content in entries:
            entry = tarfile.TarInfo(name)
            entry.type = kind
            if kind == tarfile.REGTYPE:
                entry.size = len(content)
                output.addfile(entry, io.BytesIO(content))
            else:
                if kind in (tarfile.SYMTYPE, tarfile.LNKTYPE):
                    entry.linkname = content
                output.addfile(entry)
    return stream.getvalue()


def fixtures(root):
    payload = b"archive payload\n"
    other = bytes(range(256)) * 4
    (root / "source.txt").write_bytes(payload)
    (root / "source.bin").write_bytes(other)
    (root / "valid.zip").write_bytes(zip_data([
        ("dir/", b""), ("dir/data.txt", payload), ("other.bin", other), ("empty/", b"")]))
    (root / "empty.zip").write_bytes(zip_data([]))
    legacy = zip_data([("META-INF/maven/", b""), ("META-INF/maven/pom.xml", payload)])
    struct.pack_into("<H", legacy, legacy.index(b"PK\x01\x02") + 4, (3 << 8) | 20)
    struct.pack_into("<I", legacy, legacy.index(b"PK\x01\x02") + 38, 0xffff0010)
    (root / "legacy-maven.zip").write_bytes(legacy)
    (root / "runtime.zip").write_bytes(zip_data([
        ("controlled-jre/bin/java.exe", other), ("controlled-jre/release", b"JAVA_VERSION=17.0.1")]))
    (root / "readonly.jar").write_bytes(zip_data([("CON.class", b"class bytes"), ("skin.png", payload)]))
    (root / "duplicate.zip").write_bytes(zip_data([("same", payload), ("same", payload)]))
    unrelated = zip_data([("skin.png", payload), ("other.bin", payload)], zipfile.ZIP_STORED)
    second = unrelated.index(b"PK\x03\x04", 4)
    unrelated[second + 30 + len(b"other.bin")] ^= 1
    (root / "unrelated-crc.zip").write_bytes(unrelated)
    (root / "protected.zip").write_bytes(zip_data([("nickel.instance.json", b"{}")]))
    (root / "traversal.zip").write_bytes(zip_data([("../outside", b"hostile")]))
    (root / "bomb.zip").write_bytes(zip_data([("bomb.bin", b"0" * (4 * 1024 * 1024))]))
    plain = zip_data([("data.bin", payload)], zipfile.ZIP_STORED)
    payload_at = 30 + len(b"data.bin")
    plain[payload_at] ^= 0x01
    (root / "crc.zip").write_bytes(plain)
    corrupt = zip_data([("data.bin", other)])
    corrupt[30 + len(b"data.bin")] ^= 0xFF
    (root / "deflate-corrupt.zip").write_bytes(corrupt)
    (root / "nul-name.zip").write_bytes(patch_name(
        zip_data([("bad-name", b"payload")]), b"bad-name", b"bad\x00name"))
    encrypted = zip_data([("data.bin", payload)])
    for signature, offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
        at = encrypted.index(signature)
        flags = struct.unpack_from("<H", encrypted, at + offset)[0]
        struct.pack_into("<H", encrypted, at + offset, flags | 1)
    (root / "encrypted.zip").write_bytes(encrypted)
    (root / "payload-truncated.zip").write_bytes(zip_data([("data.bin", other)])[:-24])
    info = zipfile.ZipInfo("link")
    info.create_system = 3
    info.external_attr = (stat.S_IFLNK | 0o777) << 16
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as output:
        output.writestr(info, "../outside")
    (root / "symlink.zip").write_bytes(stream.getvalue())
    valid_tar = tar_data([("dir", tarfile.DIRTYPE, ""), ("dir/data.txt", tarfile.REGTYPE, payload)])
    for name, content in (
        ("valid.tar", valid_tar), ("valid.tar.gz", gzip.compress(valid_tar)),
        ("valid.tar.bz2", bz2.compress(valid_tar)), ("valid.tar.xz", lzma.compress(valid_tar)),
        ("valid.tar.zst", zstd.compress(valid_tar))):
        (root / name).write_bytes(content)
    for name, entry in (
        ("symlink.tar", ("link", tarfile.SYMTYPE, "../outside")),
        ("hardlink.tar", ("link", tarfile.LNKTYPE, "../outside")),
        ("fifo.tar", ("fifo", tarfile.FIFOTYPE, "")),
        ("traversal.tar", ("../outside", tarfile.REGTYPE, payload))):
        (root / name).write_bytes(tar_data([entry]))
    (root / "case.tar").write_bytes(tar_data([
        ("Case.txt", tarfile.REGTYPE, payload), ("case.txt", tarfile.REGTYPE, payload)]))
    (root / "valid.jar").write_bytes(zip_data([
        ("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\r\nMain-Class: test.Main\r\n\r\n"),
        ("test/Main.class", b"\xca\xfe\xba\xbe\x00\x00\x00\x34")]))
    (root / "invalid.jar").write_bytes(zip_data([
        ("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\r\nMain-Class: test.Missing\r\n\r\n")]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--readonly", action="store_true")
    args = parser.parse_args()
    root = args.workspace.resolve()
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    fixtures(root)
    executable = str(args.executable.resolve())
    mode = ["--readonly"] if args.readonly else []
    subprocess.run([executable, *mode, str(root)], check=True, timeout=100)
    if not args.readonly:
        subprocess.run([executable, "--crash", str(root)], check=True, timeout=20)
        subprocess.run([executable, "--recover", str(root)], check=True, timeout=20)
    deep = root / "long-path"
    while len(str(deep)) < 420:
        deep /= "segment-" + "x" * 48
    deep.mkdir(parents=True)
    fixtures(deep)
    subprocess.run([executable, *mode, str(deep)], check=True, timeout=100)
    if not args.readonly:
        subprocess.run([executable, "--crash", str(deep)], check=True, timeout=20)
        subprocess.run([executable, "--recover", str(deep)], check=True, timeout=20)
    print(f"PASS archive long logical path characters={len(str(deep))}")
    assert not (root / "outside").exists()
    assert not (root / "crash-extract").exists()
    print("PASS actual read-only archive runtime" if args.readonly else "PASS actual archive runtime and separate crash/recovery processes")


if __name__ == "__main__":
    main()
