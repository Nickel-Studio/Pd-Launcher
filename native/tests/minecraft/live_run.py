"""Serial genuine acceptance coordinator; installation and Java ownership stay native."""

import argparse
import ctypes
from ctypes import wintypes as w
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

C_MIN = 1073741824
D_MIN = 4294967296
VERSION_D_MIN = 536870912
MARKER = b"Nickel isolated Minecraft acceptance cache v1\n"
DIRECTORIES = ("managed", "games", "evidence", "temp", "home")


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": h.hexdigest()}


def write_json_new(path, value):
    with path.open("x", encoding="ascii", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=True, indent=2)
        stream.write("\n")


def operation_capacity(sample, required_d):
    return sample["c"]["availableBytes"] >= C_MIN and sample["d"]["availableBytes"] >= required_d


class FileInfo(ctypes.Structure):
    _fields_ = [("attributes", w.DWORD), ("creation", w.FILETIME),
                ("access", w.FILETIME), ("write", w.FILETIME),
                ("volumeSerial", w.DWORD), ("sizeHigh", w.DWORD),
                ("sizeLow", w.DWORD), ("links", w.DWORD),
                ("indexHigh", w.DWORD), ("indexLow", w.DWORD)]


class Volumes:
    def __init__(self):
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                                      w.DWORD, w.DWORD, w.HANDLE]
        self.k.CreateFileW.restype = w.HANDLE
        self.k.GetFileInformationByHandle.argtypes = [w.HANDLE, ctypes.POINTER(FileInfo)]
        self.k.GetFileInformationByHandle.restype = w.BOOL
        self.k.GetFinalPathNameByHandleW.argtypes = [w.HANDLE, w.LPWSTR, w.DWORD, w.DWORD]
        self.k.GetFinalPathNameByHandleW.restype = w.DWORD
        self.k.GetDiskFreeSpaceExW.argtypes = [w.LPCWSTR, ctypes.POINTER(ctypes.c_ulonglong),
                                              ctypes.POINTER(ctypes.c_ulonglong),
                                              ctypes.POINTER(ctypes.c_ulonglong)]
        self.k.GetDiskFreeSpaceExW.restype = w.BOOL
        self.k.CloseHandle.argtypes = [w.HANDLE]
        self.k.CloseHandle.restype = w.BOOL
        self.k.WriteFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                                    ctypes.POINTER(w.DWORD), ctypes.c_void_p]
        self.k.WriteFile.restype = w.BOOL
        self.k.FlushFileBuffers.argtypes = [w.HANDLE]
        self.k.FlushFileBuffers.restype = w.BOOL
        self.handles = {}
        self.c = self.pin(Path("C:/"))["volumeGuid"]
        self.d = self.pin(Path("D:/"))["volumeGuid"]
        if self.c.casefold() == self.d.casefold():
            raise RuntimeError("C and D are aliases of the same physical volume")

    def close(self):
        for handle in reversed(list(self.handles.values())):
            self.k.CloseHandle(handle)
        self.handles.clear()

    def final(self, handle):
        size = self.k.GetFinalPathNameByHandleW(handle, None, 0, 1)
        if not size:
            raise ctypes.WinError(ctypes.get_last_error())
        buffer = ctypes.create_unicode_buffer(size + 1)
        written = self.k.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 1)
        if not written or written >= len(buffer):
            raise ctypes.WinError(ctypes.get_last_error())
        return buffer.value

    def inspect(self, path, handle):
        info = FileInfo()
        if not self.k.GetFileInformationByHandle(handle, ctypes.byref(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        if info.attributes & 0x400:
            raise RuntimeError("Reparse point rejected: " + str(path))
        if not info.attributes & 0x10 and info.links != 1:
            raise RuntimeError("Hard-linked file rejected: " + str(path))
        final = self.final(handle)
        if not final.startswith("\\\\?\\Volume{") or "}\\" not in final:
            raise RuntimeError("Path does not resolve to a local volume GUID: " + final)
        volume = final[:final.index("}\\") + 2]
        return {"logicalPath": str(path), "finalPath": final, "volumeGuid": volume,
                "directory": bool(info.attributes & 0x10)}

    def pin(self, path):
        path = Path(os.path.abspath(path))
        if path in self.handles:
            return self.inspect(path, self.handles[path])
        for parent in reversed(path.parents):
            if parent not in self.handles:
                self.pin(parent)
        # OPEN_REPARSE_POINT and no DELETE sharing pin every original ancestor.
        handle = self.k.CreateFileW(str(path), 0, 1 | 2, None, 3, 0x02000000 | 0x00200000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            evidence = self.inspect(path, handle)
        except BaseException:
            self.k.CloseHandle(handle)
            raise
        self.handles[path] = handle
        return evidence

    def require_d(self, path):
        evidence = self.pin(path)
        if evidence["volumeGuid"].casefold() != self.d.casefold():
            raise RuntimeError("Owned write directory is not physical D: " + str(path))
        return evidence

    def create_marker(self, path):
        handle = self.k.CreateFileW(str(path), 0x40000000, 1, None, 1, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            written = w.DWORD()
            data = ctypes.create_string_buffer(MARKER)
            if not self.k.WriteFile(handle, data, len(MARKER), ctypes.byref(written), None):
                raise ctypes.WinError(ctypes.get_last_error())
            if written.value != len(MARKER) or not self.k.FlushFileBuffers(handle):
                raise RuntimeError("Ownership marker was not durably written in full")
        finally:
            self.k.CloseHandle(handle)

    def free(self, volume):
        available, total, free = (ctypes.c_ulonglong() for _ in range(3))
        if not self.k.GetDiskFreeSpaceExW(volume, ctypes.byref(available),
                                        ctypes.byref(total), ctypes.byref(free)):
            raise ctypes.WinError(ctypes.get_last_error())
        return {"volumeGuid": volume, "availableBytes": available.value,
                "totalBytes": total.value, "totalFreeBytes": free.value}

    def sample(self, phase):
        c, d = self.free(self.c), self.free(self.d)
        return {"utc": utc(), "phase": phase, "c": c, "d": d,
                "cMinimumBytes": C_MIN, "dMinimumBytes": D_MIN,
                "globalGatePassed": c["availableBytes"] >= C_MIN and d["availableBytes"] >= D_MIN,
                "capacityIsReservation": False}


def provision(volumes, parent, requested):
    volumes.require_d(parent)
    if requested:
        cache = Path(os.path.abspath(requested))
        if cache.parent != parent or not cache.name.startswith(".runtime-d-live-"):
            raise RuntimeError("Existing cache must be this test owner's uniquely marked D cache")
        volumes.require_d(cache)
        marker = cache / ".nickel-minecraft-live-cache"
        volumes.pin(marker)
        if marker.read_bytes() != MARKER:
            raise RuntimeError("Existing cache ownership marker does not match")
        for directory, children, files in os.walk(cache, followlinks=False):
            for name in children:
                volumes.require_d(Path(directory) / name)
            for name in files:
                volumes.require_d(Path(directory) / name)
    else:
        name = ".runtime-d-live-" + dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]
        cache = parent / name
        cache.mkdir(exist_ok=False)
        volumes.require_d(cache)
        volumes.create_marker(cache / ".nickel-minecraft-live-cache")
        for name in DIRECTORIES:
            (cache / name).mkdir(exist_ok=False)
    layout = {"cache": volumes.require_d(cache)}
    for name in DIRECTORIES:
        layout[name] = volumes.require_d(cache / name)
    return cache, layout


def child_environment(cache):
    env = os.environ.copy()
    for name, value in (("TEMP", "temp"), ("TMP", "temp"), ("TMPDIR", "temp"),
                        ("HOME", "home"), ("USERPROFILE", "home"),
                        ("APPDATA", "home"), ("LOCALAPPDATA", "home")):
        env[name] = str(cache / value)
    for name in ("JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS"):
        env.pop(name, None)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["NODE_DISABLE_COMPILE_CACHE"] = "1"
    env["QT_LOGGING_TO_CONSOLE"] = "1"
    return env


def run_native(volumes, cache, executable, java, phase, arguments, required_d=D_MIN):
    token = phase + "-" + dt.datetime.now(dt.timezone.utc).strftime("%H%M%S-") + uuid.uuid4().hex[:6]
    cancellation = cache / "evidence" / (token + ".cancel")
    command = [str(executable), "--cache", str(cache), "--java", str(java),
               "--instance", token, "--cancel-file", str(cancellation)] + arguments
    result = {"phase": phase, "startedUtc": utc(), "command": command,
              "cwd": str(cache / "home"), "started": False, "capacityViolation": False,
              "requiredDBytes": required_d, "gameRequiredDBytes": D_MIN,
              "environment": {key: child_environment(cache)[key] for key in
                              ("TEMP", "TMP", "TMPDIR", "HOME", "USERPROFILE", "APPDATA", "LOCALAPPDATA")}}
    sample_file = cache / "evidence" / (token + ".capacity.jsonl")
    output_file = cache / "evidence" / (token + ".console.log")
    with sample_file.open("x", encoding="ascii", newline="\n") as samples:
        def record(label):
            sampled = volumes.sample(label)
            sampled["operationDMinimumBytes"] = required_d
            sampled["operationGatePassed"] = operation_capacity(sampled, required_d)
            samples.write(json.dumps(sampled, ensure_ascii=True) + "\n")
            samples.flush()
            return sampled["operationGatePassed"]
        if not record(phase + "-before"):
            result["status"] = "blocked-capacity-before-owned-native-spawn"
        else:
            with output_file.open("xb") as output:
                process = subprocess.Popen(command, cwd=cache / "home", env=child_environment(cache),
                                           stdout=output, stderr=subprocess.STDOUT,
                                           creationflags=subprocess.CREATE_NO_WINDOW)
                result["started"] = True
                result["nativePid"] = process.pid
                while process.poll() is None:
                    if not record(phase + "-running"):
                        result["capacityViolation"] = True
                        if not cancellation.exists():
                            with cancellation.open("xb") as signal:
                                signal.write(b"Capacity gate failed; native owner must cancel preparation or explicitly stop its accepted test Game.\n")
                        # Never kill the native owner: it must retain, stop and drain its own Game.
                    time.sleep(0.5)
                result["exitCode"] = process.returncode
                result["status"] = "native-completed" if process.returncode == 0 else "native-failed"
        if not record(phase + "-after"):
            result["capacityViolation"] = True
    result["finishedUtc"] = utc()
    result["capacityEvidence"] = digest(sample_file)
    if output_file.exists():
        result["consoleEvidence"] = digest(output_file)
    native_result = cache / "evidence" / (token + ".json")
    if native_result.exists():
        result["nativeEvidence"] = digest(native_result)
        result["nativeResult"] = json.loads(native_result.read_bytes())
    write_json_new(cache / "evidence" / (token + ".runner.json"), result)
    print(json.dumps(result, ensure_ascii=True), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("version", "proof", "vanilla"), default="proof")
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--java", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--official-only", action="store_true")
    parser.add_argument("--observe-seconds", type=int, default=60)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Actual C/D-volume acceptance requires Windows")
    if not 1 <= args.observe_seconds <= 300:
        parser.error("Observation duration must be between 1 and 300 seconds")
    if args.phase == "version" and not args.cache:
        parser.error("Version-only proof reuses an existing owned cache; it cannot provision a new fixture")
    required_d = VERSION_D_MIN if args.phase == "version" else D_MIN
    parent = Path(os.path.abspath(__file__)).parent
    root = parent.parents[2]
    executable, java, manifest = (Path(os.path.abspath(path)) for path in
                                (args.executable, args.java, args.manifest))
    if not executable.is_file() or not java.is_file() or java.name.casefold() not in ("java.exe", "javaw.exe"):
        parser.error("Explicit existing native and genuine Java executables are required")
    if manifest.parent != parent or not manifest.is_file():
        parser.error("An existing owner-scoped source/dependency manifest is required")
    binding = json.loads(manifest.read_bytes())
    volumes = Volumes()
    try:
        initial = volumes.sample("before-small-cache-provision")
        initial["operationDMinimumBytes"] = required_d
        initial["operationGatePassed"] = operation_capacity(initial, required_d)
        if not initial["operationGatePassed"]:
            print(json.dumps({"status": "blocked-capacity-before-cache-provision", "capacity": initial}), flush=True)
            return 2
        for entry in binding["sourceFiles"]:
            path = root / entry["path"]
            if not any(
                    path.is_relative_to(root / scope) for scope in ("native/core/minecraft", "native/tests/minecraft")):
                raise RuntimeError("Manifest source path escapes owner scope")
            volumes.pin(path)
            actual = digest(path)
            if any(actual[key] != entry["raw"][key] for key in ("bytes", "sha256")):
                raise RuntimeError("Source drift before genuine acceptance: " + entry["path"])
        dependency = root / binding["testedDependencyManifest"]["path"]
        volumes.pin(dependency)
        if digest(dependency)["sha256"] != binding["testedDependencyManifest"]["sha256"]:
            raise RuntimeError("Dependency manifest drift before genuine acceptance")
        dependency_root = dependency.parent
        for entry in json.loads(dependency.read_bytes())["files"]:
            path = dependency_root / entry["path"]
            if not path.is_relative_to(dependency_root / "native"):
                raise RuntimeError("Dependency path escapes captured closure")
            volumes.pin(path)
            actual = digest(path)
            if any(actual[key] != entry[key] for key in ("bytes", "sha256")):
                raise RuntimeError("Dependency source drift: " + entry["path"])
        volumes.pin(executable)
        actual_executable = digest(executable)
        if not any(all(actual_executable[key] == entry[key] for key in ("bytes", "sha256"))
                   for entry in binding["verification"]["executables"]):
            raise RuntimeError("Native executable is not bound to the tested source generation")
        cache, layout = provision(volumes, parent, args.cache)
        authority = {"createdUtc": utc(), "phase": args.phase, "cache": str(cache),
                     "marker": {"name": ".nickel-minecraft-live-cache", "bytesHex": MARKER.hex(),
                                "sha256": hashlib.sha256(MARKER).hexdigest(), "creation": "CREATE_NEW"},
                     "layout": layout, "initialCapacity": initial,
                     "sourceBinding": digest(manifest), "p0Commit": binding["p0Commit"],
                     "publicDependencyCommit": binding["publicDependencyCommit"],
                     "sourceRawHashesVerified": True, "dependencyRawHashesVerified": True,
                     "nativeExecutable": actual_executable, "readOnlyJavaExecutable": digest(java),
                     "javaPathProof": volumes.pin(java), "owner": "minecraft-resume",
                     "installationImplementation": "native C++ services only",
                     "capacityScope": "version/properties only" if args.phase == "version" else "full Java tempfile/Game",
                     "requiredDBytes": required_d, "gameRequiredDBytes": D_MIN,
                     "genuineMinecraftMainMenuVerified": False}
        run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]
        write_json_new(cache / "evidence" / (run_id + ".authority.json"), authority)
        print("OWNED_CACHE=" + str(cache), flush=True)
        if args.phase == "version":
            version = run_native(volumes, cache, executable, java, "java-17-version-readonly",
                                 ["--java-version-only", "--timeout-seconds", "60"], required_d)
            native = version.get("nativeResult", {})
            passed = (version.get("exitCode") == 0 and not version["capacityViolation"] and
                      native.get("javaVersionPropertiesVerified") is True and
                      native.get("javaTempfileVolumeVerified") is False and
                      native.get("installationVerified") is False and native.get("gameLaunchVerified") is False)
            return 0 if passed else 2
        proof = run_native(volumes, cache, executable, java, "java-17-proof", ["--java-proof-only", "--timeout-seconds", "60"])
        passed = proof.get("exitCode") == 0 and not proof["capacityViolation"] and proof.get("nativeResult", {}).get("javaTempfileVolumeVerified") is True
        if not passed:
            return 2
        if args.phase == "vanilla":
            arguments = ["--minecraft", "1.20.1", "--launch", "--observe-seconds", str(args.observe_seconds),
                         "--timeout-seconds", "1200"]
            if args.official_only:
                arguments.append("--official-only")
            vanilla = run_native(volumes, cache, executable, java, "vanilla-1-20-1", arguments)
            if vanilla.get("exitCode") != 0 or vanilla["capacityViolation"]:
                return 3
        return 0
    finally:
        volumes.close()


if __name__ == "__main__":
    sys.exit(main())
