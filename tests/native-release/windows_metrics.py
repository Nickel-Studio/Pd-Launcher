"""Actual Windows process-tree/PWS/private bytes/modules and separate PDH GPU telemetry."""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import os
from pathlib import Path
import platform
import re
import subprocess
import time

from evidence_common import EvidenceError, utc_now

SIZE_T = C.c_size_t
ULONG_PTR = C.c_size_t
MAX_WORKING_SET_ENTRIES = 2 * 1024 * 1024
PROTECTED_PIDS = {47516, 52148, os.getpid(), os.getppid()}


class FILETIME(C.Structure):
    _fields_ = [("low", W.DWORD), ("high", W.DWORD)]


class PROCESSENTRY32W(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ProcessID", W.DWORD),
                ("th32DefaultHeapID", ULONG_PTR), ("th32ModuleID", W.DWORD), ("cntThreads", W.DWORD),
                ("th32ParentProcessID", W.DWORD), ("pcPriClassBase", W.LONG), ("dwFlags", W.DWORD),
                ("szExeFile", W.WCHAR * 260)]


class MEMORY_COUNTERS(C.Structure):
    _fields_ = [("cb", W.DWORD), ("PageFaultCount", W.DWORD), ("PeakWorkingSetSize", SIZE_T),
                ("WorkingSetSize", SIZE_T), ("QuotaPeakPagedPoolUsage", SIZE_T), ("QuotaPagedPoolUsage", SIZE_T),
                ("QuotaPeakNonPagedPoolUsage", SIZE_T), ("QuotaNonPagedPoolUsage", SIZE_T),
                ("PagefileUsage", SIZE_T), ("PeakPagefileUsage", SIZE_T), ("PrivateUsage", SIZE_T)]


class MEMORYSTATUSEX(C.Structure):
    _fields_ = [("dwLength", W.DWORD), ("dwMemoryLoad", W.DWORD),
                ("ullTotalPhys", C.c_ulonglong), ("ullAvailPhys", C.c_ulonglong),
                ("ullTotalPageFile", C.c_ulonglong), ("ullAvailPageFile", C.c_ulonglong),
                ("ullTotalVirtual", C.c_ulonglong), ("ullAvailVirtual", C.c_ulonglong),
                ("ullAvailExtendedVirtual", C.c_ulonglong)]


class PDH_VALUE_UNION(C.Union):
    _fields_ = [("longValue", W.LONG), ("doubleValue", C.c_double), ("largeValue", C.c_longlong),
                ("ansiStringValue", C.c_char_p), ("wideStringValue", C.c_wchar_p)]


class PDH_VALUE(C.Structure):
    _fields_ = [("CStatus", W.DWORD), ("value", PDH_VALUE_UNION)]


class PDH_ITEM(C.Structure):
    _fields_ = [("name", C.c_wchar_p), ("formatted", PDH_VALUE)]


def filetime(value: FILETIME) -> int:
    return (value.high << 32) | value.low


class WindowsObserver:
    def __init__(self, root_pid: int, *, root_creation: int | None = None):
        if os.name != "nt" or C.sizeof(C.c_void_p) != 8:
            raise EvidenceError("Actual Windows x64 Python host is required for these metrics")
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        self.psapi = C.WinDLL("psapi", use_last_error=True)
        self.user = C.WinDLL("user32", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
        self.kernel.OpenProcess.restype = W.HANDLE
        self.kernel.CloseHandle.argtypes = [W.HANDLE]
        self.kernel.CloseHandle.restype = W.BOOL
        self.kernel.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(FILETIME)] * 4
        self.kernel.GetProcessTimes.restype = W.BOOL
        self.kernel.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
        self.kernel.QueryFullProcessImageNameW.restype = W.BOOL
        self.kernel.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
        self.kernel.CreateToolhelp32Snapshot.restype = W.HANDLE
        self.kernel.Process32FirstW.argtypes = [W.HANDLE, C.POINTER(PROCESSENTRY32W)]
        self.kernel.Process32FirstW.restype = W.BOOL
        self.kernel.Process32NextW.argtypes = [W.HANDLE, C.POINTER(PROCESSENTRY32W)]
        self.kernel.Process32NextW.restype = W.BOOL
        self.kernel.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
        self.kernel.TerminateProcess.restype = W.BOOL
        self.psapi.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(MEMORY_COUNTERS), W.DWORD]
        self.psapi.GetProcessMemoryInfo.restype = W.BOOL
        self.psapi.QueryWorkingSet.argtypes = [W.HANDLE, C.c_void_p, W.DWORD]
        self.psapi.QueryWorkingSet.restype = W.BOOL
        self.psapi.EnumProcessModulesEx.argtypes = [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), W.DWORD]
        self.psapi.EnumProcessModulesEx.restype = W.BOOL
        self.psapi.GetModuleFileNameExW.argtypes = [W.HANDLE, W.HMODULE, W.LPWSTR, W.DWORD]
        self.psapi.GetModuleFileNameExW.restype = W.DWORD
        self.root_pid = root_pid
        identity = self.identity(root_pid)
        if root_creation is not None and identity["creationTime100ns"] != root_creation:
            raise EvidenceError("Root PID has been reused")
        self.root_identity = (root_pid, identity["creationTime100ns"])
        self.known = {self.root_identity}
        self.page_size = self._page_size()

    def _page_size(self) -> int:
        # The page-size DWORD follows the processor union in SYSTEM_INFO.
        buffer = C.create_string_buffer(64)
        self.kernel.GetSystemInfo(C.byref(buffer))
        return C.cast(C.byref(buffer, 4), C.POINTER(W.DWORD)).contents.value

    def _handle(self, pid: int, access: int = 0x0400 | 0x0010):
        handle = self.kernel.OpenProcess(access, False, pid)
        if not handle:
            raise OSError(C.get_last_error(), f"OpenProcess({pid}) failed")
        return handle

    def identity(self, pid: int, handle=None) -> dict:
        own = handle is None
        if own:
            handle = self._handle(pid, 0x1000)
        try:
            created, exited, kernel, user = (FILETIME() for _ in range(4))
            if not self.kernel.GetProcessTimes(handle, C.byref(created), C.byref(exited), C.byref(kernel), C.byref(user)):
                raise OSError(C.get_last_error(), "GetProcessTimes failed")
            name, length = C.create_unicode_buffer(32768), W.DWORD(32768)
            if not self.kernel.QueryFullProcessImageNameW(handle, 0, name, C.byref(length)):
                raise OSError(C.get_last_error(), "QueryFullProcessImageName failed")
            return {"pid": pid, "creationTime100ns": filetime(created), "executable": name.value,
                    "cpuKernel100ns": filetime(kernel), "cpuUser100ns": filetime(user)}
        finally:
            if own:
                self.kernel.CloseHandle(handle)

    def process_table(self) -> dict[int, dict]:
        snapshot = self.kernel.CreateToolhelp32Snapshot(0x00000002, 0)
        if snapshot == W.HANDLE(-1).value:
            raise OSError(C.get_last_error(), "Process snapshot failed")
        rows = {}
        try:
            entry = PROCESSENTRY32W()
            entry.dwSize = C.sizeof(entry)
            valid = self.kernel.Process32FirstW(snapshot, C.byref(entry))
            while valid:
                rows[entry.th32ProcessID] = {"pid": entry.th32ProcessID, "parentPid": entry.th32ParentProcessID,
                                            "observedImageName": entry.szExeFile}
                valid = self.kernel.Process32NextW(snapshot, C.byref(entry))
            error = C.get_last_error()
            if error not in (0, 18):
                raise OSError(error, "Process enumeration was incomplete")
            return rows
        finally:
            self.kernel.CloseHandle(snapshot)

    def tree(self, *, require_root: bool = True) -> tuple[list[dict], list[dict]]:
        table = self.process_table()
        selected, errors, identities = {}, [], {}
        # Retain birth-identified descendants after reparenting, never reuse a PID as ownership.
        for pid, birth in self.known:
            if pid not in table:
                continue
            try:
                info = self.identity(pid)
                identities[pid] = info
                if info["creationTime100ns"] == birth:
                    selected[pid] = {**table[pid], **info}
            except OSError as error:
                errors.append({"pid": pid, "error": str(error)})
        expanded = True
        while expanded:
            expanded = False
            for pid, row in table.items():
                if pid in selected or row["parentPid"] not in selected:
                    continue
                try:
                    info = identities.get(pid) or self.identity(pid)
                    if info["creationTime100ns"] < selected[row["parentPid"]]["creationTime100ns"]:
                        continue
                    selected[pid] = {**row, **info}
                    self.known.add((pid, info["creationTime100ns"]))
                    expanded = True
                except OSError as error:
                    errors.append({"pid": pid, "error": str(error)})
        if require_root and self.root_pid not in selected:
            errors.append({"pid": self.root_pid, "error": "Root process missing or identity changed"})
        return sorted(selected.values(), key=lambda row: row["pid"]), errors

    def memory(self, process: dict) -> dict:
        handle = self._handle(process["pid"])
        try:
            info = self.identity(process["pid"], handle)
            if info["creationTime100ns"] != process["creationTime100ns"]:
                raise EvidenceError("Process identity changed before memory collection")
            counters = MEMORY_COUNTERS()
            counters.cb = C.sizeof(counters)
            if not self.psapi.GetProcessMemoryInfo(handle, C.byref(counters), counters.cb):
                raise OSError(C.get_last_error(), "GetProcessMemoryInfo failed")
            capacity = max(4096, counters.WorkingSetSize // self.page_size + 4096)
            while capacity <= MAX_WORKING_SET_ENTRIES:
                entries = (ULONG_PTR * (capacity + 1))()
                if self.psapi.QueryWorkingSet(handle, C.byref(entries), C.sizeof(entries)):
                    count = int(entries[0])
                    if count > capacity:
                        raise EvidenceError("Working-set count exceeds allocated buffer")
                    private_pages = sum(not (entries[index] & 0x100) for index in range(1, count + 1))
                    return {**process, **info, "status": "observed", "privateBytes": int(counters.PrivateUsage),
                            "workingSetBytes": int(counters.WorkingSetSize),
                            "privateWorkingSetBytes": private_pages * self.page_size,
                            "workingSetPagesObserved": count, "pageSize": self.page_size}
                error = C.get_last_error()
                if error not in (24, 122):
                    raise OSError(error, "QueryWorkingSet failed")
                capacity *= 2
            raise EvidenceError("Working-set observation exceeded bounded buffer; not reported as zero")
        finally:
            self.kernel.CloseHandle(handle)

    def modules(self, process: dict) -> dict:
        handle = self._handle(process["pid"])
        retries = []
        try:
            deadline = time.monotonic() + 0.5
            while True:
                if self.identity(process["pid"], handle)["creationTime100ns"] != process["creationTime100ns"]:
                    raise EvidenceError("Process identity changed before module observation")
                capacity, needed, paths = 256, W.DWORD(), []
                retry_error = None
                while capacity <= 32768:
                    handles = (W.HMODULE * capacity)()
                    if not self.psapi.EnumProcessModulesEx(handle, C.byref(handles), C.sizeof(handles), C.byref(needed), 3):
                        retry_error = C.get_last_error()
                        break
                    if needed.value > C.sizeof(handles):
                        capacity *= 2
                        continue
                    for index in range(needed.value // C.sizeof(W.HMODULE)):
                        name = C.create_unicode_buffer(32768)
                        count = self.psapi.GetModuleFileNameExW(handle, handles[index], name, 32768)
                        if not count or count >= 32767:
                            retry_error = C.get_last_error() or 299
                            break
                        paths.append(name.value)
                    break
                if capacity > 32768:
                    raise EvidenceError("Module enumeration exceeded its bounded buffer")
                if retry_error is None and paths:
                    return {**process, "moduleStatus": "observed", "modules": sorted(set(paths)), "transientRetries": retries}
                # A new process loader can still be populating its module list (ERROR_PARTIAL_COPY).
                error = retry_error or 299
                retries.append({"win32Error": error, "capturedUtc": utc_now()})
                if error not in {24, 299} or time.monotonic() >= deadline:
                    raise OSError(error, "Module path enumeration unavailable after bounded identity-checked retry")
                time.sleep(0.02)
        finally:
            self.kernel.CloseHandle(handle)

    def sample(self, gpu=None) -> dict:
        begin = time.perf_counter_ns()
        tree, errors = self.tree()
        processes = []
        for process in tree:
            try:
                processes.append(self.memory(process))
            except (OSError, EvidenceError) as error:
                errors.append({"pid": process["pid"], "error": str(error)})
                processes.append({**process, "status": "unavailable"})
        totals = None
        if not errors and processes:
            totals = {key: sum(row[key] for row in processes) for key in
                      ("privateBytes", "privateWorkingSetBytes", "workingSetBytes", "cpuKernel100ns", "cpuUser100ns")}
            totals["processCount"] = len(processes)
        return {"capturedUtc": utc_now(), "monotonicNs": begin, "observationEndNs": time.perf_counter_ns(),
                "treeCompleteAtObservation": not errors, "processes": processes, "totals": totals, "errors": errors,
                "gpu": gpu.sample({row["pid"] for row in tree}) if gpu else {"status": "unavailable", "reason": "No GPU collector"}}

    def module_snapshot(self, scenario: str, purposes: dict | None = None) -> dict:
        tree, errors = self.tree()
        observed, children = [], []
        purposes = purposes or {}
        for process in tree:
            purpose = "launcher" if process["pid"] == self.root_pid else purposes.get(process["executable"].casefold(), "unexplained")
            try:
                row = {**self.modules(process), "purpose": purpose}
            except (OSError, EvidenceError) as error:
                errors.append({"pid": process["pid"], "error": str(error)})
                row = {**process, "purpose": purpose, "moduleStatus": "unavailable", "modules": []}
            observed.append(row)
            if process["pid"] != self.root_pid:
                children.append({**process, "purpose": purpose})
        root = next((row for row in observed if row["pid"] == self.root_pid), {})
        return {"scenario": scenario, "pid": self.root_pid, "creationTime100ns": self.root_identity[1],
                "capturedUtc": utc_now(), "modules": root.get("modules", []), "children": children,
                "processTree": observed, "treeComplete": not errors, "errors": errors}

    def terminate_owned(self) -> list[dict]:
        rows, errors = self.tree(require_root=False)
        for row in reversed(rows):
            pid = row["pid"]
            if pid in PROTECTED_PIDS or (pid, row["creationTime100ns"]) not in self.known:
                errors.append({"pid": pid, "error": "Protected or unowned PID cannot be terminated"})
                continue
            try:
                handle = self._handle(pid, 0x0001 | 0x1000)
                try:
                    if self.identity(pid, handle)["creationTime100ns"] != row["creationTime100ns"]:
                        raise EvidenceError("PID birth identity changed; cleanup stops")
                    if not self.kernel.TerminateProcess(handle, 125):
                        raise OSError(C.get_last_error(), "Owned-process termination failed")
                finally:
                    self.kernel.CloseHandle(handle)
            except (OSError, EvidenceError) as error:
                errors.append({"pid": pid, "error": str(error)})
        return errors


class GpuObserver:
    def __init__(self):
        self.query, self.counters, self.reason = W.HANDLE(), {}, None
        if os.name != "nt":
            self.reason = "Windows PDH GPU telemetry unavailable on this host"
            return
        self.pdh = C.WinDLL("pdh")
        self.pdh.PdhOpenQueryW.argtypes = [W.LPCWSTR, ULONG_PTR, C.POINTER(W.HANDLE)]
        self.pdh.PdhOpenQueryW.restype = W.DWORD
        self.pdh.PdhAddEnglishCounterW.argtypes = [W.HANDLE, W.LPCWSTR, ULONG_PTR, C.POINTER(W.HANDLE)]
        self.pdh.PdhAddEnglishCounterW.restype = W.DWORD
        self.pdh.PdhCollectQueryData.argtypes = [W.HANDLE]
        self.pdh.PdhCollectQueryData.restype = W.DWORD
        self.pdh.PdhGetFormattedCounterArrayW.argtypes = [W.HANDLE, W.DWORD, C.POINTER(W.DWORD), C.POINTER(W.DWORD), C.c_void_p]
        self.pdh.PdhGetFormattedCounterArrayW.restype = W.DWORD
        self.pdh.PdhCloseQuery.argtypes = [W.HANDLE]
        status = self.pdh.PdhOpenQueryW(None, 0, C.byref(self.query))
        if status:
            self.reason = f"PDH open failed: 0x{status:08x}"
            return
        for label, suffix in (("dedicatedBytes", "Dedicated Usage"), ("sharedBytes", "Shared Usage")):
            handle = W.HANDLE()
            counter = "\\GPU Process Memory(*)\\" + suffix
            status = self.pdh.PdhAddEnglishCounterW(self.query, counter, 0, C.byref(handle))
            if status:
                self.reason = f"{counter} unavailable: 0x{status:08x}"
                break
            self.counters[label] = (handle, counter)
        if not self.reason:
            self.pdh.PdhCollectQueryData(self.query)

    def sample(self, pids: set[int]) -> dict:
        if self.reason:
            return {"status": "unavailable", "reason": self.reason, "bytes": None}
        status = self.pdh.PdhCollectQueryData(self.query)
        if status:
            return {"status": "unavailable", "reason": f"PDH collection failed: 0x{status:08x}", "bytes": None}
        records, totals = [], {key: 0 for key in self.counters}
        for label, (handle, counter) in self.counters.items():
            size, count = W.DWORD(), W.DWORD()
            status = self.pdh.PdhGetFormattedCounterArrayW(handle, 0x400, C.byref(size), C.byref(count), None)
            if status != 0x800007D2 or size.value > 16 * 1024 * 1024:
                return {"status": "unavailable", "reason": f"GPU array unavailable: 0x{status:08x}", "bytes": None}
            buffer = C.create_string_buffer(size.value)
            status = self.pdh.PdhGetFormattedCounterArrayW(handle, 0x400, C.byref(size), C.byref(count), C.byref(buffer))
            if status:
                return {"status": "unavailable", "reason": f"GPU array race: 0x{status:08x}", "bytes": None}
            items = C.cast(buffer, C.POINTER(PDH_ITEM))
            matched = False
            for index in range(count.value):
                match = re.search(r"(?:^|_)pid_(\d+)(?:_|$)", items[index].name)
                if not match or int(match.group(1)) not in pids:
                    continue
                matched = True
                if items[index].formatted.CStatus not in (0, 1):
                    return {"status": "unavailable", "reason": "GPU PID counter status invalid", "bytes": None}
                value = int(items[index].formatted.value.largeValue)
                if value < 0:
                    return {"status": "unavailable", "reason": "Negative GPU observation", "bytes": None}
                totals[label] += value
                records.append({"pid": int(match.group(1)), "instance": items[index].name, "counter": counter, "bytes": value})
            if not matched:
                return {"status": "unavailable", "reason": "No matching GPU process-memory counter; absence is not zero", "bytes": None}
        return {"status": "observed", "totals": totals, "instances": records,
                "basis": "PDH GPU Process Memory dedicated/shared usage, adapters summed; separate from CPU memory"}

    def close(self):
        if self.query and hasattr(self, "pdh"):
            self.pdh.PdhCloseQuery(self.query)
            self.query = W.HANDLE()


def machine_metadata() -> dict:
    result = {"platform": platform.platform(), "system": platform.system(), "release": platform.release(),
              "architecture": platform.machine(), "pythonVersion": platform.python_version(),
              "logicalCpuCount": os.cpu_count(), "capturedUtc": utc_now(),
              "performanceClock": time.get_clock_info("perf_counter").__dict__}
    if os.name != "nt":
        return result
    kernel = C.WinDLL("kernel32")
    kernel.GetTickCount64.restype = C.c_ulonglong
    memory = MEMORYSTATUSEX()
    memory.dwLength = C.sizeof(memory)
    kernel.GlobalMemoryStatusEx(C.byref(memory))
    result.update({"uptimeMs": int(kernel.GetTickCount64()), "totalPhysicalBytes": int(memory.ullTotalPhys),
                   "availablePhysicalBytes": int(memory.ullAvailPhys)})
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            result["processorModel"] = winreg.QueryValueEx(key, "ProcessorNameString")[0]
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion") as key:
            for label in ("ProductName", "DisplayVersion", "CurrentBuild", "UBR"):
                try:
                    result[label] = winreg.QueryValueEx(key, label)[0]
                except OSError:
                    result[label] = None
        power = subprocess.run(["powercfg", "/getactivescheme"], capture_output=True, timeout=5, check=False)
        result["powerScheme"] = power.stdout.decode("utf-8", errors="replace").strip()
    except (OSError, subprocess.SubprocessError) as error:
        result["machineMetadataError"] = str(error)
    return result
