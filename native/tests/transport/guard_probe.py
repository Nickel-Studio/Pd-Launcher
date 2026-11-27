import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import tempfile
import uuid


INVALID = ctypes.c_void_p(-1).value
DIRECTORY_FLAGS = 0x02000000 | 0x00200000
DIRECTORY_ACCESS = 0x00000001 | 0x00000080 | 0x00100000
SET_REPARSE = 0x000900A4
DELETE_REPARSE = 0x000900AC


def windows_api():
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                               ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    api.CreateFileW.restype = wintypes.HANDLE
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    api.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                   ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    api.GetFileAttributesW.argtypes = [wintypes.LPCWSTR]
    api.GetFileAttributesW.restype = wintypes.DWORD
    return api


def open_handle(api, path, access, share, flags=DIRECTORY_FLAGS, disposition=3):
    ctypes.set_last_error(0)
    handle = api.CreateFileW("\\\\?\\" + str(path), access, share, None, disposition, flags, None)
    return handle, ctypes.get_last_error() if handle == INVALID else 0


def control(api, handle, code, data):
    buffer = ctypes.create_string_buffer(data)
    returned = wintypes.DWORD()
    ctypes.set_last_error(0)
    success = bool(api.DeviceIoControl(handle, code, buffer, len(data), None, 0,
                                      ctypes.byref(returned), None))
    return {"success": success, "error": 0 if success else ctypes.get_last_error()}


def reparse_data(kind, outside):
    substitute = ("\\??\\" + str(outside)).encode("utf-16-le")
    display = str(outside).encode("utf-16-le")
    paths = substitute + b"\0\0" + display + b"\0\0"
    offsets = struct.pack("<HHHH", 0, len(substitute), len(substitute) + 2, len(display))
    guid = uuid.UUID("4194cda1-8d85-4dc3-beb3-fddfa4d790f3").bytes_le
    if kind == "mount_point":
        tag, payload = 0xA0000003, offsets + paths
    elif kind == "symlink":
        tag, payload = 0xA000000C, offsets + struct.pack("<I", 0) + paths
    elif kind == "custom_guid":
        tag, payload = 0x00000123, b"transport-probe"
        return tag, struct.pack("<IHH", tag, len(payload), 0) + guid + payload, struct.pack("<IHH", tag, 0, 0) + guid
    elif kind == "custom_name_surrogate_guid":
        tag, payload = 0x20000123, b"transport-probe"
        return tag, struct.pack("<IHH", tag, len(payload), 0) + guid + payload, struct.pack("<IHH", tag, 0, 0) + guid
    else:
        raise ValueError(kind)
    return tag, struct.pack("<IHH", tag, len(payload), 0) + payload, struct.pack("<IHH", tag, 0, 0)


def rename_attempt(source, target):
    try:
        source.rename(target)
    except OSError as error:
        return {"success": False, "error": error.winerror}
    target.rename(source)
    return {"success": True, "error": 0}


def guarded_case(api, root, outside, mode, kind):
    parent = root / "parent"
    target = parent / "target"
    target.mkdir(parents=True)
    chain = [root, parent, target]
    pins = []
    anchor = INVALID
    writer = INVALID
    cleanup_data = None
    result = {"mode": mode, "tag_kind": kind}
    try:
        for path in chain:
            handle, error = open_handle(api, path, DIRECTORY_ACCESS, 1 if mode != "rw_empty" else 3)
            if error:
                raise OSError(error, "cannot establish raw directory guard")
            pins.append(handle)
        if mode == "anchor":
            anchor, error = open_handle(api, target / ".pin", 0xC0010000, 1,
                                        0x00000002 | 0x04000000 | 0x00200000, 1)
            if error:
                raise OSError(error, "cannot establish delete-on-close anchor")
            for at, path in enumerate(chain):
                handle, error = open_handle(api, path, DIRECTORY_ACCESS, 3)
                if error:
                    raise OSError(error, "cannot transition to mutation sharing")
                api.CloseHandle(pins[at])
                pins[at] = handle
        # The anchor is the only child during tag attempts. Inner rename is independent.
        inner = target / "inner"
        inner.write_bytes(b"inside")
        result["inner_rename"] = rename_attempt(inner, target / "renamed")
        inner.unlink()
        result["root_rename"] = rename_attempt(root, root.parent / (root.name + "-moved"))
        result["parent_rename"] = rename_attempt(parent, root / "parent-moved")
        writer, error = open_handle(api, target, 0x40000000, 7)
        result["writer_open"] = {"success": not error, "error": error}
        tag, data, delete_data = reparse_data(kind, outside)
        result["tag"] = hex(tag)
        if not error:
            result["set_reparse"] = control(api, writer, SET_REPARSE, data)
            if result["set_reparse"]["success"]:
                cleanup_data = delete_data
                result["attributes_after_set"] = hex(api.GetFileAttributesW("\\\\?\\" + str(target)))
                try:
                    result["full_path_followed_outside"] = (target / "canary").read_bytes() == b"outside canary"
                except OSError as read_error:
                    result["full_path_read_error"] = read_error.winerror
                result["delete_reparse"] = control(api, writer, DELETE_REPARSE, delete_data)
                if result["delete_reparse"]["success"]:
                    cleanup_data = None
        return result
    finally:
        if cleanup_data is not None and writer != INVALID:
            if not control(api, writer, DELETE_REPARSE, cleanup_data)["success"]:
                raise RuntimeError("probe reparse tag retained; refusing tree cleanup")
        if writer != INVALID:
            api.CloseHandle(writer)
        if anchor != INVALID:
            api.CloseHandle(anchor)
        for handle in reversed(pins):
            api.CloseHandle(handle)
        if target.exists():
            target.rmdir()
        parent.rmdir()
        root.rmdir()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        raise RuntimeError("this is a Windows-only raw API probe")
    if args.output.exists():
        raise RuntimeError("probe evidence output already exists")
    workspace = args.workspace.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="guard-raw-", dir=workspace))
    outside = root / "outside"
    outside.mkdir()
    (outside / "canary").write_bytes(b"outside canary")
    results = []
    api = windows_api()
    try:
        for mode in ("strict", "rw_empty", "anchor"):
            for kind in ("mount_point", "symlink", "custom_guid", "custom_name_surrogate_guid"):
                results.append(guarded_case(api, root / (mode + "-" + kind), outside, mode, kind))
        document = {"platform": platform.platform(), "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    "claims": "Raw sharing/reparse observations only; not production guard equivalence.",
                    "results": results, "outside_unchanged": (outside / "canary").read_bytes() == b"outside canary"}
        args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(document, indent=2))
    finally:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
