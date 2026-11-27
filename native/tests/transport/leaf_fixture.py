import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def rejected(action, message):
    try:
        action()
    except OSError:
        return
    raise RuntimeError(message)


def external_windows(executable, parent):
    from ctypes import wintypes
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                               ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    api.CreateFileW.restype = wintypes.HANDLE
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    root = parent / "external-root"
    nested = root / "nested"
    nested.mkdir(parents=True)
    leaf = nested / "authorized"
    leaf.write_bytes(b"authorized bytes")
    outside = parent / "outside"
    outside.mkdir()
    (outside / "authorized").write_bytes(b"foreign outside bytes")
    process = subprocess.Popen([str(executable), "--hold", str(root), str(leaf)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, encoding="utf-8")
    try:
        if process.stdout.readline().strip() != "READY":
            raise RuntimeError("fixed-handle fixture did not become ready")
        rejected(lambda: root.rename(parent / "moved-root"), "pinned root was renamed externally")
        rejected(lambda: nested.rename(root / "moved-parent"), "pinned parent was renamed externally")
        rejected(lambda: leaf.rename(nested / "moved-leaf"), "fixed DELETE leaf was renamed externally")
        rejected(lambda: leaf.write_bytes(b"foreign writer"), "fixed leaf allowed external writer")
        writer = api.CreateFileW(str(nested), 0x40000000, 7, None, 3, 0x02000000 | 0x00200000, None)
        if writer != ctypes.c_void_p(-1).value:
            api.CloseHandle(writer)
            raise RuntimeError("strict parent allowed a junction-writing handle")
        if ctypes.get_last_error() != 32:
            raise RuntimeError("directory writer was rejected for a reason other than sharing protection")
        process.stdin.write("continue\n")
        process.stdin.flush()
        output, error = process.communicate(timeout=20)
        if process.returncode or "PASS external guarded deletion" not in output:
            raise RuntimeError(f"guarded deletion failed: {output} {error}")
        if leaf.exists() or (outside / "authorized").read_bytes() != b"foreign outside bytes":
            raise RuntimeError("guarded deletion affected foreign content")
        junction = root / "junction"
        subprocess.run(["cmd", "/c", "mklink", "/J", str(junction), str(outside)],
                       check=True, capture_output=True, timeout=10)
        rejected_process = subprocess.run([str(executable), "--hold", str(root), str(junction / "authorized")],
                                          input="continue\n", capture_output=True, text=True, timeout=15)
        if rejected_process.returncode == 0 or (outside / "authorized").read_bytes() != b"foreign outside bytes":
            raise RuntimeError("preexisting junction was followed by capture/deletion")
        junction.rmdir()
        print("PASS external root/parent/leaf rename + writer/junction handle blocking and preexisting junction rejection")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--hooks", action="store_true")
    args = parser.parse_args()
    parent = args.workspace.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="leaf-process-", dir=parent))
    executable = args.executable.resolve()
    try:
        subprocess.run([str(executable), str(root)], check=True, timeout=40)
        if os.name == "nt" and args.hooks:
            external_windows(executable, root)
        deep = root / "deep"
        while len(str(deep)) < 420:
            deep /= "segment-" + "x" * 48
        deep.mkdir(parents=True)
        subprocess.run([str(executable), str(deep)], check=True, timeout=40)
        print(f"PASS real guarded leaf deletion deep path characters={len(str(deep))}")
    finally:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
