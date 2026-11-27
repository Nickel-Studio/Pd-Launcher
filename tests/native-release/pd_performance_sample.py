"""Bounded, same-host startup / idle-memory sampling for legacy Ni 0.1.0 and Pd.

External observation only: the same window-readiness rule and the same process-tree
memory collector (tests/native-release/windows_metrics.py) are used for both
products, so legacy internal markers are never required and Pd-only fast paths are
not used. This is a local performance probe, not the strict release-evidence
protocol (no WGC pixel capture, no equivalence gate, no reduction claim).

Isolation: every launch runs an owned portable fixture whose adjacent
`nickel.portable.json` pins configuration/game data into the fixture. APPDATA /
LOCALAPPDATA / XDG_* / TEMP / TMP / WEBVIEW2_USER_DATA_FOLDER are redirected to
fixture subdirectories as a second layer. Real user accounts are never configured
and the fixture is empty.
"""
from __future__ import annotations

import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from windows_metrics import GpuObserver, WindowsObserver, machine_metadata  # noqa: E402

USER32 = C.WinDLL("user32", use_last_error=True)
KERNEL32 = C.WinDLL("kernel32", use_last_error=True)
DWMAPI = C.WinDLL("dwmapi", use_last_error=True)
WM_NULL = 0x0000
WM_CLOSE = 0x0010
SMTO_ABORTIFHUNG = 0x0002

WNDENUMPROC = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
USER32.EnumWindows.argtypes = [WNDENUMPROC, W.LPARAM]
USER32.EnumWindows.restype = W.BOOL
USER32.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
USER32.GetWindowThreadProcessId.restype = W.DWORD
USER32.IsWindowVisible.argtypes = [W.HWND]
USER32.IsWindowVisible.restype = W.BOOL
USER32.IsIconic.argtypes = [W.HWND]
USER32.IsIconic.restype = W.BOOL
USER32.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
USER32.GetClientRect.restype = W.BOOL
USER32.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
USER32.GetWindowTextW.restype = C.c_int
USER32.SendMessageTimeoutW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT, C.POINTER(C.c_size_t)]
USER32.SendMessageTimeoutW.restype = W.LPARAM
USER32.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
USER32.PostMessageW.restype = W.BOOL
USER32.GetDpiForWindow.argtypes = [W.HWND]
USER32.GetDpiForWindow.restype = W.UINT
DWMAPI.DwmGetWindowAttribute.argtypes = [W.HWND, W.DWORD, C.c_void_p, W.DWORD]
DWMAPI.DwmGetWindowAttribute.restype = C.c_long


def now_ns() -> int:
    return time.perf_counter_ns()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cloaked(hwnd: int) -> bool:
    value = W.DWORD()
    result = DWMAPI.DwmGetWindowAttribute(hwnd, 14, C.byref(value), C.sizeof(value))
    return result == 0 and value.value != 0


def windows_of(pid: int) -> list[dict]:
    rows = []

    @WNDENUMPROC
    def callback(hwnd, _):
        owner = W.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, C.byref(owner))
        if owner.value != pid or not USER32.IsWindowVisible(hwnd):
            return True
        rect = W.RECT()
        if not USER32.GetClientRect(hwnd, C.byref(rect)):
            return True
        title = C.create_unicode_buffer(512)
        USER32.GetWindowTextW(hwnd, title, 512)
        rows.append({"hwnd": int(hwnd), "title": title.value, "clientWidth": rect.right - rect.left,
                     "clientHeight": rect.bottom - rect.top, "minimized": bool(USER32.IsIconic(hwnd)),
                     "cloaked": cloaked(hwnd), "dpi": int(USER32.GetDpiForWindow(hwnd) or 0)})
        return True

    USER32.EnumWindows(callback, 0)
    return rows


def usable(row: dict) -> bool:
    return (row["clientWidth"] >= 200 and row["clientHeight"] >= 120 and not row["minimized"]
            and not row["cloaked"])


def responsive(hwnd: int) -> bool:
    result = C.c_size_t()
    ok = USER32.SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, C.byref(result))
    return bool(ok)


def enumerate_profile(roots: list[Path]) -> dict:
    record = {}
    for root in roots:
        key = str(root)
        if not root.is_dir():
            record[key] = {"present": False}
            continue
        files = 0
        total = 0
        newest = 0
        try:
            for path in root.rglob("*"):
                if path.is_file():
                    files += 1
                    info = path.stat()
                    total += info.st_size
                    newest = max(newest, info.st_mtime_ns)
        except OSError as error:
            record[key] = {"present": True, "error": str(error)}
            continue
        record[key] = {"present": True, "fileCount": files, "bytes": total, "newestMtimeNs": newest}
    return record


def clear_application_cache(fixture: Path) -> list[str]:
    removed = []
    for name in ("cache", "webview", "Nickel/cache"):
        path = fixture / name
        if not path.exists():
            continue
        for item in sorted(path.rglob("*"), reverse=True):
            try:
                if item.is_file() or item.is_symlink():
                    item.unlink()
                elif item.is_dir():
                    item.rmdir()
            except OSError:
                pass
        removed.append(name)
    return removed


def prepare_environment(fixture: Path) -> dict:
    env = os.environ.copy()
    temp = fixture / "temp"
    for path in (temp, fixture / "cache", fixture / "webview", fixture / "appdata-roaming",
                 fixture / "appdata-local", fixture / "home"):
        path.mkdir(parents=True, exist_ok=True)
    env.update({
        "TEMP": str(temp), "TMP": str(temp),
        "APPDATA": str(fixture / "appdata-roaming"),
        "LOCALAPPDATA": str(fixture / "appdata-local"),
        "XDG_CONFIG_HOME": str(fixture / "appdata-roaming"),
        "XDG_DATA_HOME": str(fixture / "appdata-local"),
        "HOME": str(fixture / "home"),
        "WEBVIEW2_USER_DATA_FOLDER": str(fixture / "webview"),
        "PYTHONDONTWRITEBYTECODE": "1",
    })
    return env


def launch(exe: Path, fixture: Path, env: dict, timeout: float) -> dict:
    argv = [str(exe)]
    began = now_ns()
    process = subprocess.Popen(argv, cwd=str(fixture), env=env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    created = now_ns()
    deadline = time.monotonic() + timeout
    ready_window = None
    seen: list[dict] = []
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        for row in windows_of(process.pid):
            seen.append(row)
            if usable(row) and responsive(row["hwnd"]):
                ready_window = row
                break
        if ready_window:
            break
        time.sleep(0.005)
    ready = now_ns()
    result = {"pid": process.pid, "argv": argv, "launchBeginNs": began, "createReturnedNs": created,
              "readyObservedNs": ready, "processHandles": process,
              "startupMs": (ready - began) / 1e6 if ready_window else None,
              "createMs": (created - began) / 1e6,
              "window": ready_window, "windowsSeen": seen[-8:], "exitedEarly": process.poll()}
    return result


def memory_window(observer: WindowsObserver, gpu: GpuObserver, fixture: Path, settle: float, interval: float,
                  count: int) -> dict:
    time.sleep(settle)
    samples = []
    for _ in range(count):
        row = observer.sample(gpu)
        samples.append(row)
        time.sleep(interval)
    totals = [row["totals"] for row in samples if row["totals"]]
    median = {}
    if totals:
        for key in ("privateBytes", "privateWorkingSetBytes", "workingSetBytes", "processCount"):
            median[key] = statistics.median([row[key] for row in totals])
    gpu_ok = all(row.get("gpu", {}).get("status") == "observed" for row in samples if row.get("gpu"))
    gpu_median = None
    if gpu_ok and samples and samples[0].get("gpu", {}).get("totals"):
        gpu_median = {key: statistics.median([row["gpu"]["totals"][key] for row in samples])
                      for key in ("dedicatedBytes", "sharedBytes")}
    children = sorted({row.get("executable", "") for sample in samples for row in sample["processes"]
                       if row.get("pid") != observer.root_pid})
    return {"settleSeconds": settle, "intervalSeconds": interval, "sampleCount": len(samples),
            "samples": samples, "median": median, "gpuMedian": gpu_median,
            "childExecutables": children,
            "treeComplete": all(row["treeCompleteAtObservation"] for row in samples)}


def shutdown(process: subprocess.Popen, window: dict, observer: WindowsObserver) -> dict:
    sent = bool(USER32.PostMessageW(window["hwnd"], WM_CLOSE, 0, 0))
    normal = False
    try:
        code = process.wait(15)
        normal = True
    except subprocess.TimeoutExpired:
        code = None
    cleanup = []
    if not normal:
        cleanup = observer.terminate_owned()
        try:
            code = process.wait(5)
        except subprocess.TimeoutExpired:
            code = None
    time.sleep(1.0)
    leftover = [row for row in observer.tree(require_root=False)[0]]
    return {"wmCloseSent": sent, "normalExit": normal, "exitCode": code, "forcedCleanup": cleanup,
            "processesStillAlive": [{"pid": row["pid"], "image": row.get("executable", "")} for row in leftover]}


def run_once(args, exe: Path, fixture: Path, kind: str, group: str, index: int) -> dict:
    env = prepare_environment(fixture)
    removed = clear_application_cache(fixture) if group == "cold-app-cache" else []
    result = {"role": args.role, "kind": kind, "group": group, "index": index,
              "cacheDirsCleared": removed, "fixture": str(fixture), "createdUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    gpu = GpuObserver()
    launch_info = launch(exe, fixture, env, args.startup_timeout)
    process = launch_info.pop("processHandles")
    result["launch"] = {key: value for key, value in launch_info.items() if key != "windowsSeen"}
    result["windowsSeen"] = launch_info["windowsSeen"]
    observer = None
    if process.poll() is None and launch_info["window"]:
        observer = WindowsObserver(process.pid)
        if kind == "memory":
            result["idle"] = memory_window(observer, gpu, fixture, args.settle_seconds,
                                           args.sample_interval, args.memory_samples)
        result["shutdown"] = shutdown(process, launch_info["window"], observer)
    else:
        result["startupFailed"] = True
        try:
            process.wait(5)
        except subprocess.TimeoutExpired:
            process.kill()
    result["profileAfterRun"] = enumerate_profile([Path(os.environ["LOCALAPPDATA"]) / "studio.nickel.launcher",
                                                   Path(os.environ["APPDATA"]) / "studio.nickel.launcher"])
    gpu.close()
    return result


def stats(values: list[float]) -> dict | None:
    if not values:
        return None
    ordered = sorted(values)
    return {"count": len(values), "min": ordered[0], "median": statistics.median(ordered),
            "p95NearestRank": ordered[max(0, -(-len(ordered) * 95 // 100) - 1)], "max": ordered[-1]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", required=True, choices=("legacy", "native"))
    parser.add_argument("--exe", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--cold-starts", type=int, default=10)
    parser.add_argument("--warm-starts", type=int, default=10)
    parser.add_argument("--memory-runs", type=int, default=10)
    parser.add_argument("--settle-seconds", type=float, default=5.0)
    parser.add_argument("--sample-interval", type=float, default=1.0)
    parser.add_argument("--memory-samples", type=int, default=3)
    parser.add_argument("--startup-timeout", type=float, default=45.0)
    args = parser.parse_args()

    exe = Path(args.exe).resolve()
    if not exe.is_file():
        raise SystemExit(f"missing executable: {exe}")
    output = Path(args.output).resolve()
    if output.exists():
        raise SystemExit("output directory must be unused (evidence is never overwritten)")
    fixture = output / "fixture"
    fixture.mkdir(parents=True)
    deployed = fixture / exe.name
    with exe.open("rb") as source, deployed.open("xb") as target:
        while chunk := source.read(1024 * 1024):
            target.write(chunk)
    if sha256_file(deployed) != sha256_file(exe):
        raise SystemExit("deployed fixture executable does not match the measured source")
    marker = {"schemaVersion": 1, "format": "nickel-portable", "packageId": "0" * 31 + "1",
              "name": "Perf fixture", "version": "0.1.0", "instanceName": "perf-fixture",
              "payload": "Nickel/pack.mrpack", "gameDirectory": ".minecraft", "dataDirectory": "Nickel",
              "launcher": {"path": "Nickel.exe", "os": "windows", "arch": "x64"}, "personalization": None}
    (fixture / "nickel.portable.json").write_text(json.dumps(marker, indent=2) + "\n", encoding="utf-8")
    for name in ("Nickel", ".minecraft", "cache", "temp"):
        (fixture / name).mkdir(exist_ok=True)

    profile_roots = [Path(os.environ["LOCALAPPDATA"]) / "studio.nickel.launcher",
                     Path(os.environ["APPDATA"]) / "studio.nickel.launcher"]
    before = enumerate_profile(profile_roots)
    raw = output / "raw"
    raw.mkdir()
    machine = machine_metadata()
    machine["exe"] = {"path": str(exe), "bytes": exe.stat().st_size, "sha256": sha256_file(exe)}
    machine["fixtureMarker"] = marker
    machine["environmentOverrides"] = {key: value for key, value in prepare_environment(fixture).items()
                                       if key in ("TEMP", "TMP", "APPDATA", "LOCALAPPDATA", "XDG_CONFIG_HOME",
                                                  "XDG_DATA_HOME", "HOME", "WEBVIEW2_USER_DATA_FOLDER")}
    runs = []
    warmup = run_once(args, deployed, fixture, "startup", "hot-app-cache", -1)
    warmup["warmup"] = True
    runs.append(warmup)
    sequence = ([("cold-app-cache", i) for i in range(args.cold_starts)]
                + [("hot-app-cache", i) for i in range(args.warm_starts)])
    for group, index in sequence:
        runs.append(run_once(args, deployed, fixture, "startup", group, index))
    for index in range(args.memory_runs):
        runs.append(run_once(args, deployed, fixture, "memory", "cold-app-cache", index))
    for run in runs:
        name = f"{run['kind']}-{run['group']}-{run['index']:03d}.json"
        (raw / name).write_text(json.dumps(run, indent=2) + "\n", encoding="utf-8")
        print(run["kind"], run["group"], run["index"],
              None if run["launch"]["startupMs"] is None else round(run["launch"]["startupMs"], 1),
              "ms", "idle" if "idle" in run else "", flush=True)

    counted = [row for row in runs if not row.get("warmup")]
    summary = {"role": args.role, "machine": machine, "counts": {"startup": len(counted), "memory": len(counted)},
               "profileBefore": before,
               "profileAfter": enumerate_profile(profile_roots),
               "startupMs": {group: stats([row["launch"]["startupMs"] for row in counted
                                           if row["kind"] == "startup" and row["group"] == group
                                           and row["launch"]["startupMs"] is not None])
                             for group in ("cold-app-cache", "hot-app-cache")},
               "startupAll": stats([row["launch"]["startupMs"] for row in counted
                                    if row["kind"] == "startup" and row["launch"]["startupMs"] is not None]),
               "memory": {key: stats([row["idle"]["median"][key] for row in counted
                                      if row["kind"] == "memory" and row["idle"]["median"]])
                          for key in ("privateBytes", "privateWorkingSetBytes", "workingSetBytes", "processCount")},
               "failures": [{"kind": row["kind"], "group": row["group"], "index": row["index"],
                             "startupFailed": row.get("startupFailed", False),
                             "lint": None} for row in counted
                            if row.get("startupFailed") or (row["launch"]["startupMs"] is None)],
               "limitations": [
                   "External window readiness only: first visible, uncloaked, >=200x120 client window owned by the",
                   "root PID that answers WM_NULL. It is not a compositor-presented pixel or product input receipt.",
                   "Warm/cold refers to the APPLICATION cache directories inside the portable fixture, not an OS reboot.",
                   "Process-tree private working set / private bytes include every descendant (legacy WebView2 included).",
                   "GPU dedicated/shared PDH values are reported only when actually observed; absence is not zero.",
                   "No real accounts, no network business action, no installer and no user profile data are used."]}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: summary[key] for key in ("startupMs", "startupAll", "memory", "failures")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())