"""Measured size report for Palladium program EXEs (stdlib only).

For each EXE it records: file bytes + SHA-256, the deterministic single-member
program-ZIP size at the packaging tool's *current effective* compression
(zlib default level, which is what `zipfile.ZipInfo` members get when
`compresslevel` is not passed to `writestr`) and at explicit deflate level 9,
plus a PE section table with per-section deflate-9 and xz-9 sizes so section
compressibility can be compared.

This is a local measurement tool, not a release gate. It never writes into the
source tree.

Usage:
  python -B scripts/native/pd_size_report.py \
      --exe .snow/size/baseline/native/app/pd-launcher.exe \
      --exe .snow/size/ipo/native/app/pd-launcher.exe \
      --label baseline --label ipo \
      --output .snow/size/size-report.json
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import lzma
import struct
import sys
import zipfile
import zlib
from pathlib import Path

ZIP_TOP = "pd-launcher-windows-x64-0.1.0"
ZIP_DATETIME = (1980, 1, 1, 0, 0, 0)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def program_zip(data: bytes, compresslevel: int | None) -> int:
    """Deterministic single-file ZIP, mirroring scripts/native/release_package.py."""
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        directory = zipfile.ZipInfo(ZIP_TOP + "/", date_time=ZIP_DATETIME)
        archive.writestr(directory, b"")
        member = zipfile.ZipInfo(f"{ZIP_TOP}/pd-launcher.exe", date_time=ZIP_DATETIME)
        member.compress_type = zipfile.ZIP_DEFLATED
        if compresslevel is None:
            archive.writestr(member, data)  # what the current packaging tool does
        else:
            archive.writestr(member, data, compresslevel=compresslevel)
    return len(buffer.getvalue())


def sections(data: bytes) -> list[dict]:
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Not a PE image")
    coff = pe + 4
    count, = struct.unpack_from("<H", data, coff + 2)
    optional_size, = struct.unpack_from("<H", data, coff + 16)
    table = coff + 20 + optional_size
    rows = []
    for index in range(count):
        offset = table + index * 40
        name = data[offset:offset + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, _, rsize, roff = struct.unpack_from("<IIII", data, offset + 8)
        raw = data[roff:roff + rsize]
        rows.append({
            "name": name,
            "virtualSize": vsize,
            "rawBytes": rsize,
            "deflate9": len(zlib.compress(raw, 9)) if raw else 0,
            "xz9": len(lzma.compress(raw, preset=9)) if raw else 0,
        })
    return rows


def measure(path: Path) -> dict:
    data = path.read_bytes()
    section_rows = sections(data)
    return {
        "path": str(path),
        "bytes": len(data),
        "sha256": sha256(data),
        "programZipDefault": program_zip(data, None),
        "programZipDeflate9": program_zip(data, 9),
        "xz9WholeFile": len(lzma.compress(data, preset=9)),
        "sections": section_rows,
        "sectionRawSum": sum(row["rawBytes"] for row in section_rows),
        "sectionDeflate9Sum": sum(row["deflate9"] for row in section_rows),
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", action="append", required=True)
    parser.add_argument("--label", action="append", default=[])
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)

    if args.label and len(args.label) != len(args.exe):
        print("--label must be repeated once per --exe", file=sys.stderr)
        return 2

    results = []
    for index, exe in enumerate(args.exe):
        row = measure(Path(exe))
        row["label"] = args.label[index] if args.label else Path(exe).parent.parent.name
        results.append(row)
        print(f"{row['label']}: exe={row['bytes']:,} "
              f"zipDefault={row['programZipDefault']:,} "
              f"zipDeflate9={row['programZipDeflate9']:,} "
              f"xz9={row['xz9WholeFile']:,} sha256={row['sha256'][:16]}…")
        for section in row["sections"]:
            ratio = section["deflate9"] / section["rawBytes"] if section["rawBytes"] else 0.0
            print(f"  {section['name']:8} raw={section['rawBytes']:>10,} "
                  f"deflate9={section['deflate9']:>10,} ratio={ratio:.3f}")

    Path(args.output).write_text(json.dumps(results, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))