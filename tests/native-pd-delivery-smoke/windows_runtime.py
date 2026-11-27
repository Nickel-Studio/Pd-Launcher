"""Owned HWND/birth observation; PE/modules parsing remains in existing acceptance."""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import os
from pathlib import Path, PureWindowsPath
import struct
import subprocess
import time

from delivery_smoke import (MAX_PACKAGE, NotRun, SmokeError, digest, file_ref, now, plain,
                            tree_files, write_json)

PROTECTED_PIDS = {54396, os.getpid(), os.getppid()}


class GUID(C.Structure):
    _fields_ = [("data1", W.DWORD), ("data2", W.WORD), ("data3", W.WORD), ("data4", C.c_ubyte * 8)]


def windows_root() -> Path:
    kernel = C.WinDLL("kernel32", use_last_error=True)
    kernel.GetWindowsDirectoryW.argtypes = [W.LPWSTR, W.UINT]
    kernel.GetWindowsDirectoryW.restype = W.UINT
    name = C.create_unicode_buffer(32768)
    count = kernel.GetWindowsDirectoryW(name, len(name))
    if not count or count >= len(name):
        raise NotRun("Actual Windows system directory is unavailable")
    return Path(name.value)


def empty_known_folder_guard() -> dict:
    shell = C.WinDLL("shell32", use_last_error=True)
    shell.SHGetKnownFolderPath.argtypes = [C.POINTER(GUID), W.DWORD, W.HANDLE, C.POINTER(C.c_void_p)]
    shell.SHGetKnownFolderPath.restype = C.c_long
    ole = C.WinDLL("ole32")
    ole.CoTaskMemFree.argtypes = [C.c_void_p]
    folder = GUID(0x3EB685DB, 0x65F9, 0x4CF6, (C.c_ubyte * 8)(0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D))
    pointer = C.c_void_p()
    result = shell.SHGetKnownFolderPath(C.byref(folder), 0x4000, None, C.byref(pointer))
    if result != 0 or not pointer.value:
        raise NotRun("Actual Windows RoamingAppData known-folder identity is unavailable")
    try:
        roaming = Path(C.wstring_at(pointer))
    finally:
        ole.CoTaskMemFree(pointer)
    # Existence metadata only: never open/enumerate a user's existing launcher data.
    config = roaming / "studio.nickel.launcher"
    plain(roaming)
    plain(config, exists=False)
    if config.exists():
        raise NotRun("Actual Windows launcher known-folder root already exists; no user data is read or changed")
    return {"mode": "actual-empty-known-folder-and-fresh-adjacent-data", "knownFolderApi": "SHGetKnownFolderPath",
            "configRoot": str(config), "checkedAbsentUtc": now(), "userDataOpened": False,
            "fixtureOverlay": False, "limitation": "No injected portable/config fixture; this is not a dedicated-account global write sandbox"}


def clean_environment(output: Path, system: Path) -> dict:
    profile = output / "profile"
    temporary = output / "temp"
    for directory in (profile, temporary, profile / "roaming", profile / "local"):
        plain(directory, exists=False)
        directory.mkdir(parents=True, exist_ok=False)
    return {"SystemRoot": str(system), "WINDIR": str(system), "OS": "Windows_NT",
            "PATH": ";".join(str(path) for path in (system / "System32", system, system / "System32/Wbem")),
            "ComSpec": str(system / "System32/cmd.exe"), "TEMP": str(temporary), "TMP": str(temporary),
            "USERPROFILE": str(profile), "HOME": str(profile), "APPDATA": str(profile / "roaming"),
            "LOCALAPPDATA": str(profile / "local"), "HOMEDRIVE": PureWindowsPath(str(profile)).drive,
            "HOMEPATH": str(profile)[2:], "PROCESSOR_ARCHITECTURE": "AMD64"}


def version_info(executable: Path) -> dict:
    api = C.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [W.LPCWSTR, C.POINTER(W.DWORD)]
    api.GetFileVersionInfoSizeW.restype = W.DWORD
    api.GetFileVersionInfoW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p]
    api.GetFileVersionInfoW.restype = W.BOOL
    api.VerQueryValueW.argtypes = [C.c_void_p, W.LPCWSTR, C.POINTER(C.c_void_p), C.POINTER(W.UINT)]
    api.VerQueryValueW.restype = W.BOOL
    unused = W.DWORD()
    size = api.GetFileVersionInfoSizeW(str(executable), C.byref(unused))
    if not 1 <= size <= 1024 * 1024:
        raise SmokeError("Actual Pd EXE has no bounded Windows version resource")
    buffer = C.create_string_buffer(size)
    if not api.GetFileVersionInfoW(str(executable), 0, size, buffer):
        raise SmokeError("Actual version resource could not be read")
    pointer, count = C.c_void_p(), W.UINT()
    if (not api.VerQueryValueW(buffer, "\\VarFileInfo\\Translation", C.byref(pointer), C.byref(count)) or
            not 4 <= count.value <= 64 or count.value % 4):
        raise SmokeError("Actual EXE lacks concrete version-resource translations")
    data = C.string_at(pointer, count.value)
    result = {}
    for offset in range(0, len(data), 4):
        language, codepage = struct.unpack_from("<HH", data, offset)
        strings = {}
        for key in ("ProductName", "FileDescription", "InternalName", "OriginalFilename", "ProductVersion", "FileVersion"):
            query = f"\\StringFileInfo\\{language:04x}{codepage:04x}\\{key}"
            if api.VerQueryValueW(buffer, query, C.byref(pointer), C.byref(count)) and 1 <= count.value <= 512:
                strings[key] = C.wstring_at(pointer, count.value).rstrip("\x00")
        result[f"{language:04x}{codepage:04x}"] = strings
    if not result:
        raise SmokeError("Actual EXE has no usable product-name resource")
    return result


class Windows:
    def __init__(self, observer, process):
        self.observer = observer
        self.process = process
        self.pid, self.birth = observer.root_identity
        self.kernel = observer.kernel
        self.user = observer.user
        self.callback_type = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
        self.user.EnumWindows.argtypes = [self.callback_type, W.LPARAM]
        self.user.EnumWindows.restype = W.BOOL
        self.user.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
        self.user.GetWindowThreadProcessId.restype = W.DWORD
        self.user.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
        self.user.GetWindowTextW.restype = C.c_int
        self.user.GetClassNameW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
        self.user.GetClassNameW.restype = C.c_int
        self.user.IsWindowVisible.argtypes = [W.HWND]
        self.user.IsWindowVisible.restype = W.BOOL
        self.user.IsWindow.argtypes = [W.HWND]
        self.user.IsWindow.restype = W.BOOL
        self.user.GetWindow.argtypes = [W.HWND, W.UINT]
        self.user.GetWindow.restype = W.HWND
        self.user.GetWindowRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
        self.user.GetWindowRect.restype = W.BOOL
        self.user.SendMessageTimeoutW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT, C.POINTER(C.c_size_t)]
        self.user.SendMessageTimeoutW.restype = C.c_ssize_t
        self.user.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
        self.user.PostMessageW.restype = W.BOOL

    def live_identity(self) -> dict:
        if self.pid in PROTECTED_PIDS or self.process.poll() is not None:
            raise SmokeError("Protected/exited process is not an owned live close target")
        creation_handle = W.HANDLE(int(self.process.process._handle))
        original = self.observer.identity(self.pid, creation_handle)
        current = self.observer.identity(self.pid)
        if original["creationTime100ns"] != self.birth or current["creationTime100ns"] != self.birth:
            raise SmokeError("Kernel birth changed; never act on a reused PID")
        if Path(current["executable"]) != Path(original["executable"]):
            raise SmokeError("Live executable image identity changed")
        return current

    def record(self, hwnd: int) -> dict | None:
        owner = W.DWORD()
        self.user.GetWindowThreadProcessId(hwnd, C.byref(owner))
        if owner.value != self.pid or not self.user.IsWindow(hwnd):
            return None
        title, class_name, rectangle = C.create_unicode_buffer(512), C.create_unicode_buffer(256), W.RECT()
        self.user.GetWindowTextW(hwnd, title, len(title))
        self.user.GetClassNameW(hwnd, class_name, len(class_name))
        if not self.user.GetWindowRect(hwnd, C.byref(rectangle)):
            raise SmokeError("Owned window geometry is unavailable")
        response = C.c_size_t()
        responsive = bool(self.user.SendMessageTimeoutW(hwnd, 0, 0, 0, 0x2, 500, C.byref(response)))
        self.user.GetWindowThreadProcessId(hwnd, C.byref(owner))
        if owner.value != self.pid or not self.user.IsWindow(hwnd):
            raise SmokeError("Window owner changed during actual HWND observation")
        return {"hwnd": int(hwnd), "pid": self.pid, "creationTime100ns": self.birth, "title": title.value,
                "class": class_name.value, "visible": bool(self.user.IsWindowVisible(hwnd)),
                "windowOwner": int(self.user.GetWindow(hwnd, 4) or 0), "responsive": responsive,
                "rect": [rectangle.left, rectangle.top, rectangle.right, rectangle.bottom], "capturedUtc": now()}

    def snapshot(self) -> list[dict]:
        self.live_identity()
        result, errors, count = [], [], 0

        def collect(hwnd, _):
            nonlocal count
            count += 1
            if count > 8192:
                errors.append("Window enumeration exceeded bounded host quota")
                return False
            try:
                row = self.record(hwnd)
                if row is not None:
                    result.append(row)
            except (OSError, ValueError) as error:
                errors.append(str(error))
            return True

        callback = self.callback_type(collect)
        if not self.user.EnumWindows(callback, 0) or errors:
            raise SmokeError("Actual owned HWND enumeration was incomplete: " + "; ".join(errors))
        self.live_identity()
        return result

    def close(self, row: dict) -> dict:
        self.live_identity()
        if row.get("pid") != self.pid or row.get("creationTime100ns") != self.birth:
            raise SmokeError("WM_CLOSE record does not bind this exact owned PID/birth")
        current = self.record(row["hwnd"])
        if current is None or current["class"] != row["class"] or current["title"] != row["title"]:
            raise SmokeError("Owned HWND was recycled or changed; WM_CLOSE is refused")
        self.live_identity()
        if not self.user.PostMessageW(row["hwnd"], 0x0010, 0, 0):
            raise SmokeError("WM_CLOSE delivery failed for owned HWND")
        return {"method": "PostMessageW.WM_CLOSE", "pid": self.pid, "creationTime100ns": self.birth,
                "hwnd": row["hwnd"], "postedUtc": now(), "forcedTermination": False}


def native_main(row: dict) -> bool:
    rectangle = row.get("rect", [0, 0, 0, 0])
    return bool(row.get("title") == "Palladium" and row.get("class", "").startswith("Qt") and
                "QWindow" in row.get("class", "") and row.get("visible") and row.get("responsive") and
                row.get("windowOwner") == 0 and rectangle[2] > rectangle[0] and rectangle[3] > rectangle[1])


def observe(executable: Path, artifact: dict, output: Path, dependencies: dict, seconds: float, capacity, *, worker=None) -> dict:
    capacity(24 * 1024 * 1024)
    guard = empty_known_folder_guard()
    system = windows_root()
    environment = clean_environment(output, system)
    write_json(output / "launch-environment.json", {"kind": "actual-clean-dll-search-environment", "environment": environment,
                                                   "knownFolderGuard": guard, "inheritedDeveloperVariables": [],
                                                   "dllSearch": "Unmodified Windows standard loader search; PATH is OS directories only"})
    versions = version_info(executable)
    expected_versions = {"ProductName": "Palladium", "FileDescription": "Palladium", "InternalName": "pd-launcher",
                         "OriginalFilename": "pd-launcher.exe", "ProductVersion": artifact["version"], "FileVersion": artifact["version"]}
    if any(any(values.get(key) != value for key, value in expected_versions.items()) for values in versions.values()):
        raise SmokeError("Actual EXE resource names/version are old Ni or differ from the supplied Pd generation")
    write_json(output / "windows-version-resource.json", versions)
    trace_directory = output / "trace"
    trace_directory.mkdir(exist_ok=False)
    trace = dependencies["event_trace"].EventTrace(trace_directory, int(seconds) + 30)
    kernel = C.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
    kernel.CreateFileW.restype = W.HANDLE
    kernel.CloseHandle.argtypes = [W.HANDLE]
    pinned = kernel.CreateFileW(str(executable), 0x80000000, 0x1, None, 3, 0x80, None)
    if pinned == W.HANDLE(-1).value:
        raise NotRun("Actual EXE cannot be pinned against write/delete during the startup run")
    process = observer = windows = None
    snapshots, window_records, known, errors, native = [], [], set(), [], None
    result = {"productionExecutableObserved": False, "startupGate": {"status": "NOT_RUN"},
              "closeGate": {"status": "NOT_RUN"}, "residualOwnedProcess": [], "knownFolderGuard": guard,
              "observationSeconds": seconds, "pollIntervalSeconds": 0.25, "normalArguments": []}
    close_result = None
    try:
        if digest(executable) != artifact["executable"]["sha256"]:
            raise SmokeError("Pinned actual EXE SHA changed before native process creation")
        capacity(24 * 1024 * 1024)
        trace.start()
        empty_known_folder_guard()
        capacity(24 * 1024 * 1024)
        if worker is not None:
            worker.keep = True
        process = dependencies["bounded_process"].BoundedProcess([str(executable)], cwd=executable.parent,
                                                               environment=environment, log=output / "product-output.log")
        if process.pid in PROTECTED_PIDS:
            raise SmokeError("Native creation returned a protected PID; no close action is permitted")
        observer = dependencies["windows_metrics"].WindowsObserver(process.pid)
        windows = Windows(observer, process)
        actual = windows.live_identity()
        if Path(actual["executable"]) != executable:
            raise SmokeError("Process creation image is not the exact independently extracted Pd EXE")
        result.update(productionExecutableObserved=True, rootIdentity=actual,
                      processCreationBeginNs=process.creation_begin_ns, processCreationEndNs=process.creation_end_ns)
        known.add(observer.root_identity)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and len(snapshots) < 80:
            capacity()
            if process.poll() is not None:
                errors.append("Actual Pd process exited before bounded startup observation finished")
                break
            try:
                records = windows.snapshot()
                window_records.append({"capturedUtc": now(), "windows": records})
                candidates = [row for row in records if native_main(row)]
                if len(candidates) == 1:
                    native = candidates[0]
                snapshot = observer.module_snapshot("clean-deployment-startup")
                snapshots.append(snapshot)
                if not snapshot["treeComplete"]:
                    errors.append("Actual birth-bound process/module snapshot was incomplete")
                if len(snapshot["processTree"]) > 256 or len(snapshot["modules"]) > 2048:
                    raise SmokeError("Actual process/module evidence exceeded bounded smoke quota")
                known.update((row["pid"], row["creationTime100ns"]) for row in snapshot["processTree"])
                if snapshot["children"]:
                    errors.append("Actual child processes appeared without an explicitly invoked startup user action")
                if process.error:
                    errors.append("Bounded native stdout evidence is incomplete: " + process.error)
                    break
            except (OSError, ValueError) as error:
                errors.append(str(error))
            time.sleep(0.25)
        if native is None:
            errors.append("No responsive visible Palladium native Qt top-level main window was actually observed")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        errors.append(str(error))
    finally:
        try:
            if process is not None and process.poll() is None:
                try:
                    if windows is None:
                        raise SmokeError("No verified kernel-birth/window identity exists; never force-terminate by PID")
                    targets = [row for row in windows.snapshot() if row["visible"]]
                    target = next((row for row in targets if native is not None and row["hwnd"] == native["hwnd"]), None)
                    if target is None and len(targets) == 1:
                        target = targets[0]
                    if target is None:
                        raise SmokeError("No single owned close target was identified; leave process evidence, never kill by name")
                    close_result = windows.close(target)
                    exit_code = process.wait(timeout=5)
                    close_result.update(exitCode=exit_code, exitedUtc=now())
                    result["closeGate"] = {"status": "PASS" if exit_code == 0 else "FAIL", **close_result}
                except (OSError, ValueError, subprocess.SubprocessError) as error:
                    result["closeGate"] = {"status": "FAIL", "detail": str(error), "wmClose": close_result,
                                           "forcedTermination": False}
            elif process is not None:
                result["closeGate"] = {"status": "FAIL", "detail": "Actual EXE exited before owned WM_CLOSE", "exitCode": process.poll()}
            trace_receipt = trace.finish()
        finally:
            kernel.CloseHandle(pinned)
    if observer is not None:
        residual, residual_errors = observer.tree(require_root=False)
        result["residualOwnedProcess"] = [{"pid": row["pid"], "creationTime100ns": row["creationTime100ns"],
                                          "executable": row["executable"]} for row in residual]
        result["residualIdentityIncomplete"] = bool(residual_errors)
        if residual or residual_errors:
            result["closeGate"] = {"status": "FAIL", "detail": "Bounded observation retains live owned descendants or incomplete residual identity",
                                   "wmClose": close_result, "errors": residual_errors, "forcedTermination": False}
    elif process is not None:
        result["residualIdentityIncomplete"] = True
        if process.poll() is None:
            result["residualOwnedProcess"] = [{"pid": process.pid, "creationTime100ns": None,
                                              "detail": "Unverified birth; never act on this PID"}]
    if worker is not None and observer is not None:
        worker.keep = bool(result["residualOwnedProcess"] or result.get("residualIdentityIncomplete"))
    module_evidence = {"schemaVersion": 1, "executableSha256": artifact["executable"]["sha256"], "snapshots": snapshots,
                       "origin": "actual-production-executable" if result["productionExecutableObserved"] else "not-observed",
                       "sourceCommit": artifact["sourceCommit"]}
    write_json(output / "observed-modules.json", module_evidence)
    write_json(output / "observed-windows.json", {"rootIdentity": result.get("rootIdentity"), "snapshots": window_records})
    result["trace"] = trace_receipt
    if observer is not None:
        lifetime = dependencies["event_trace"].trace_descendants(trace.read_events(), observer.root_pid, observer.root_identity[1],
                                                                 known, trace_observed=trace_receipt["status"] == "observed")
        result["boundedLifecycle"] = lifetime
        if lifetime["allTracedDescendantsCount"]:
            errors.append("Process events recorded actual startup descendants; this no-user-action scenario does not explain them")
    else:
        lifetime = {"completeObservedLifetime": False}
    acceptance = dependencies["acceptance"].audit(executable, output / "observed-modules.json", str(system))
    write_json(output / "runtime-module-audit.json", acceptance)
    errors.extend(acceptance["failures"])
    if digest(executable) != artifact["executable"]["sha256"]:
        errors.append("Actual deployed EXE changed during observation")
    changes = tree_files(output, hashes=False, limit=MAX_PACKAGE + 48 * 1024 * 1024)
    extracted_runtimes = [row["distributionPath"] for row in changes if PureWindowsPath(row["distributionPath"]).suffix.casefold()
                          in {".dll", ".node", ".qml", ".js", ".mjs"}]
    extra_executables = [row["distributionPath"] for row in changes if PureWindowsPath(row["distributionPath"]).suffix.casefold() == ".exe"
                         and row["distributionPath"] != "deployed/pd-launcher.exe"]
    if extracted_runtimes or extra_executables:
        errors.append("Runtime/extra EXE files appeared in the owned deployment/profile/temp scope")
    try:
        guard_after = empty_known_folder_guard()
    except NotRun as error:
        guard_after = {"status": "FAIL", "detail": str(error)}
        errors.append("Known-folder state changed during the actual run; no user file is opened or cleaned")
    result.update(extractedRuntimeFiles=extracted_runtimes, extraExecutableFiles=extra_executables, errors=errors,
                  observedModules=file_ref(output / "observed-modules.json"), observedWindows=file_ref(output / "observed-windows.json"),
                  runtimeModuleAudit=file_ref(output / "runtime-module-audit.json"), knownFolderGuardAfter=guard_after)
    gate_status = "FAIL" if errors else "PASS" if lifetime["completeObservedLifetime"] else "NOT_RUN"
    result["startupGate"] = {"status": gate_status, "actualMainWindow": native, "errors": errors,
                             "detail": "Bounded actual native startup/modules/lifecycle; not feature/noGame/global extraction/visual acceptance"}
    write_json(output / "runtime-receipt.json", result)
    return result
