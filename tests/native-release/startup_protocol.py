"""Strict interleaved A/B startup measurement protocol (Pd vs legacy Ni 0.1.0).

This is a local, pre-registered, noise-controlled startup probe with a
content-paint approximation.  It measures startup cost; it is not a release
performance gate, not the WGC driver protocol, and it does not by itself
upgrade any number into a release claim.

Pre-declared protocol (fixed before any run):

Readiness levels (external observation only, identical rules for both products)
  L0  first visible, non-minimized, non-DWM-cloaked top-level window owned by
      the root PID with client area >= 200x120. At L0 the window is pinned
      TOPMOST (no move/size/activate) so screen capture cannot be occluded.
      The intervention is applied identically to both products and recorded.
  L1  window-ready: the L0 rule plus the window answers WM_NULL within 2 s.
      Timer: immediately before CreateProcess -> that observation. This is the
      same external rule pd_performance_sample.py used, so L1 numbers stay
      comparable with the previous report's window-ready numbers.
  L2  content-settled approximation: every poll (default 12 ms) the client area
      is GDI-captured (screen StretchBlt) downsampled to 128x80 32bpp BGRA and
      hashed (blake2b-16). L2 = first poll at/after L1 where the last
      --settle-k (default 5) captured frames are byte-identical AND the frame
      is "structured": >= --distinct-threshold (default 12) distinct byte
      values at capture resolution. The frame stream, hash transitions and
      first-structured time are recorded per run. Declared limits: L2 measures
      "client-area pixels stopped changing"; it is NOT a compositor-presented
      pixel or an input receipt. A shell whose first captured frame already
      looks final is credited with the earliest observable settle; an animated
      UI never settles (L2 censored at --startup-timeout, excluded from L2
      stats, counted and reported).

Cold / warm definitions
  cold  fixture application-cache directories (cache, webview, Nickel/cache)
        deleted + best-effort standby aging (sequential non-recursive read of
        --cold-flood-mib MiB from --flood-source, 8 s budget) + fresh process
        (new PID). OS file-cache/standby residency of EXE pages, the Windows
        prefetcher and antivirus state remain uncontrolled; declared, not
        hidden.
  warm  none of the above: the previous run's fixture cache is kept; fresh
        process (new PID).

Sampling
  Interleaved A/B: rounds r = 0..R-1 (R = --rounds, default 15); every provided
  role runs exactly once per round; the starter alternates per round (even
  rounds run the roles in listed order, odd rounds reversed). The warm phase
  follows the cold phase with the same interleaving. One uncounted warmup
  launch per role precedes counting. Every run records phase/round/slot/
  orderIndex plus UTC and monotonic timestamps and host memory state, so time
  drift can be audited afterwards.

Statistics (fixed before the run; no post-hoc selection)
  median, IQR (inclusive linear-interpolation quantiles), MAD x 1.4826,
  95% percentile bootstrap CI of the median (B = --bootstrap, seed = --seed).
  Outlier rule: Iglewicz-Hoban modified z |0.6745 (x - median) / MAD| >
  --madz-threshold (3.5) when MAD > 0; when MAD == 0, Tukey extreme fences
  (Q1 - 3 IQR, Q3 + 3 IQR); groups with fewer than 4 samples are never
  trimmed. Excluded samples keep their raw values in the JSON, listed with
  reason, and statistics are recomputed on the kept set. Paired per-round
  differences (pd - legacy) per phase use the same bootstrap, so time drift
  within a phase cancels pairwise.

Failure rule: a launch that does not reach L1 within --startup-timeout is a
failure (listed, excluded from all timing stats, never re-run to improve).
L2-censored runs keep their L1 value and are excluded only from L2 stats.

Output: --output directory (must not exist): summary.json (machine-readable,
includes every raw sample and the frame log of the first 256 frames per run),
summary.txt (human-readable), raw/sample-NNN.json per sample.

Isolation: identical to pd_performance_sample.py - owned portable fixture,
nickel.portable.json pins config/game data into the fixture, APPDATA /
LOCALAPPDATA / XDG_* / TEMP / TMP / WEBVIEW2_USER_DATA_FOLDER are redirected,
the real user profile directories are counted before/after, no accounts, no
network business action.
"""
from __future__ import annotations

import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import time
from collections import deque

sys.path.insert(0, str(Path(__file__).resolve().parent))
from windows_metrics import WindowsObserver, machine_metadata  # noqa: E402
from evidence_common import EvidenceError  # noqa: E402

PROTOCOL_SCHEMA_VERSION = 1
CAPTURE_WIDTH = 128
CAPTURE_HEIGHT = 80
FLOOD_SOURCE_DEFAULT = r"C:\Windows\System32"
FLOOD_BUDGET_SECONDS = 8.0
OUTLIER_MINIMUM_GROUP = 4

WM_NULL = 0x0000
WM_CLOSE = 0x0010
SMTO_ABORTIFHUNG = 0x0002
SRCCOPY = 0x00CC0020
HALFTONE = 4
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010

USER32 = C.WinDLL("user32", use_last_error=True)
GDI32 = C.WinDLL("gdi32", use_last_error=True)
DWMAPI = C.WinDLL("dwmapi", use_last_error=True)

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
USER32.ClientToScreen.argtypes = [W.HWND, C.POINTER(W.POINT)]
USER32.ClientToScreen.restype = W.BOOL
USER32.SetWindowPos.argtypes = [W.HWND, W.HWND, C.c_int, C.c_int, C.c_int, C.c_int, W.UINT]
USER32.SetWindowPos.restype = W.BOOL
USER32.GetDC.argtypes = [W.HWND]
USER32.GetDC.restype = W.HDC
USER32.ReleaseDC.argtypes = [W.HWND, W.HDC]
USER32.ReleaseDC.restype = C.c_int

DWMAPI.DwmGetWindowAttribute.argtypes = [W.HWND, W.DWORD, C.c_void_p, W.DWORD]
DWMAPI.DwmGetWindowAttribute.restype = C.c_long

class BITMAPINFOHEADER(C.Structure):
    _fields_ = [("biSize", W.DWORD), ("biWidth", C.c_long), ("biHeight", C.c_long),
                ("biPlanes", W.WORD), ("biBitCount", W.WORD), ("biCompression", W.DWORD),
                ("biSizeImage", W.DWORD), ("biXPelsPerMeter", C.c_long), ("biYPelsPerMeter", C.c_long),
                ("biClrUsed", W.DWORD), ("biClrImportant", W.DWORD)]


class BITMAPINFO(C.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", W.DWORD * 3)]


class MEMORYSTATUSEX(C.Structure):
    _fields_ = [("dwLength", W.DWORD), ("dwMemoryLoad", W.DWORD),
                ("ullTotalPhys", C.c_ulonglong), ("ullAvailPhys", C.c_ulonglong),
                ("ullTotalPageFile", C.c_ulonglong), ("ullAvailPageFile", C.c_ulonglong),
                ("ullTotalVirtual", C.c_ulonglong), ("ullAvailVirtual", C.c_ulonglong),
                ("ullAvailExtendedVirtual", C.c_ulonglong)]


GDI32.CreateCompatibleDC.argtypes = [W.HDC]
GDI32.CreateCompatibleDC.restype = W.HDC
GDI32.CreateDIBSection.argtypes = [W.HDC, C.POINTER(BITMAPINFO), W.UINT,
                                   C.POINTER(C.c_void_p), W.HANDLE, W.DWORD]
GDI32.CreateDIBSection.restype = W.HBITMAP
GDI32.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
GDI32.SelectObject.restype = W.HGDIOBJ
GDI32.SetStretchBltMode.argtypes = [W.HDC, C.c_int]
GDI32.SetStretchBltMode.restype = C.c_int
GDI32.StretchBlt.argtypes = [W.HDC, C.c_int, C.c_int, C.c_int, C.c_int, W.HDC,
                             C.c_int, C.c_int, C.c_int, C.c_int, W.DWORD]
GDI32.StretchBlt.restype = W.BOOL
GDI32.DeleteObject.argtypes = [W.HGDIOBJ]
GDI32.DeleteObject.restype = W.BOOL
GDI32.DeleteDC.argtypes = [W.HDC]
GDI32.DeleteDC.restype = W.BOOL


# --------------------------------------------------------------------------
# Pure, testable protocol pieces (no GUI, no process launching).
# --------------------------------------------------------------------------

def quantile_inclusive(values: list[float], p: float) -> float:
    """Linear-interpolation quantile (same family as NumPy's default)."""
    ordered = sorted(values)
    if not ordered:
        raise ValueError("quantile of empty list")
    if len(ordered) == 1:
        return ordered[0]
    position = p * (len(ordered) - 1)
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    fraction = position - low
    return ordered[low] + (ordered[high] - ordered[low]) * fraction


def median_iqr_mad(values: list[float]) -> dict | None:
    if not values:
        return None
    ordered = sorted(values)
    median = statistics.median(ordered)
    mad = statistics.median([abs(value - median) for value in ordered])
    q1 = quantile_inclusive(ordered, 0.25)
    q3 = quantile_inclusive(ordered, 0.75)
    return {"count": len(ordered), "min": ordered[0], "max": ordered[-1],
            "mean": statistics.fmean(ordered), "median": median,
            "q1": q1, "q3": q3, "iqr": q3 - q1, "mad": mad, "madSigma": 1.4826 * mad}


def bootstrap_ci_median(values: list[float], *, samples: int = 10000, seed: int = 20261124,
                        level: float = 0.95) -> list[float] | None:
    """Deterministic percentile bootstrap CI of the median (pre-declared seed)."""
    if not values:
        return None
    if len(values) == 1:
        return [values[0], values[0]]
    rng = random.Random(seed)
    count = len(values)
    medians = []
    for _ in range(samples):
        medians.append(statistics.median([values[rng.randrange(count)] for _ in range(count)]))
    alpha = (1.0 - level) / 2.0
    return [quantile_inclusive(medians, alpha), quantile_inclusive(medians, 1.0 - alpha)]


def outlier_rule(values: list[float], *, madz_threshold: float = 3.5) -> dict:
    """Pre-declared trimming rule; returns kept values plus audited exclusions."""
    if len(values) < OUTLIER_MINIMUM_GROUP:
        return {"kept": list(values), "excluded": [], "rule": "none(group<4)"}
    base = median_iqr_mad(values)
    excluded: list[dict] = []
    if base["mad"] > 0:
        for index, value in enumerate(values):
            score = 0.6745 * (value - base["median"]) / base["mad"]
            if abs(score) > madz_threshold:
                excluded.append({"index": index, "value": value,
                                 "reason": f"modifiedZ={score:.3f}>{madz_threshold}"})
        rule = f"iglewicz-hoban modifiedZ>{madz_threshold}"
    else:
        low = base["q1"] - 3.0 * base["iqr"]
        high = base["q3"] + 3.0 * base["iqr"]
        for index, value in enumerate(values):
            if value < low or value > high:
                excluded.append({"index": index, "value": value,
                                 "reason": f"tukey-extreme-fence[{low:.1f},{high:.1f}]"})
        rule = f"tukey-extreme-fences(mad==0)[{low:.1f},{high:.1f}]"
    kept = [value for index, value in enumerate(values)
            if index not in {entry["index"] for entry in excluded}]
    return {"kept": kept, "excluded": excluded, "rule": rule}


def summarize_group(values: list[float], *, bootstrap: int = 10000, seed: int = 20261124,
                    madz_threshold: float = 3.5) -> dict:
    rule = outlier_rule(values, madz_threshold=madz_threshold)
    kept = rule["kept"]
    stats = median_iqr_mad(kept)
    ci = bootstrap_ci_median(kept, samples=bootstrap, seed=seed) if kept else None
    return {"nRaw": len(values), "nKept": len(kept), "outlierRule": rule["rule"],
            "excluded": rule["excluded"], "stats": stats, "ci95Median": ci}


def build_ab_order(rounds: int, roles: list[str]) -> list[list[str]]:
    """Each round contains every role exactly once; the starter alternates."""
    roles = list(roles)
    order = []
    for index in range(max(0, rounds)):
        order.append(list(roles) if index % 2 == 0 else list(reversed(roles)))
    return order


def paired_differences(first: list[float], second: list[float]) -> list[float]:
    if len(first) != len(second):
        raise ValueError("paired inputs must be equal length")
    return [a - b for a, b in zip(first, second)]


def frame_is_structured(data: bytes, distinct_threshold: int) -> bool:
    """A captured frame is "structured" when it carries enough distinct values."""
    return len(set(data)) >= distinct_threshold


def settle_index(frame_structured: list[bool], frame_hashes: list[str], k: int,
                 first_index: int = 0) -> int | None:
    """Pre-declared L2 rule on a recorded frame stream.

    Returns the first index i >= first_index where the k frames ending at i are
    all byte-identical AND the frame at i is structured, else None.
    """
    if k <= 0 or len(frame_hashes) < k:
        return None
    for i in range(max(first_index, k - 1), len(frame_hashes)):
        if not frame_structured[i]:
            continue
        if all(frame_hashes[j] == frame_hashes[i] for j in range(i - k + 1, i + 1)):
            return i
    return None


def assemble_stats(samples: list[dict], *, bootstrap: int = 10000, seed: int = 20261124,
                   madz_threshold: float = 3.5, phases: tuple[str, ...] = ("cold", "warm"),
                   metrics: tuple[str, ...] = ("l1Ms", "l2Ms")) -> dict:
    """Build the machine-readable statistics block from raw sample records."""
    counted = [s for s in samples if not s.get("warmup") and not s["readiness"].get("failed")]
    failures = [{"orderIndex": s["orderIndex"], "role": s["role"], "phase": s["phase"],
                 "round": s["round"], "reason": s["readiness"].get("failReason")}
                for s in samples if not s.get("warmup") and s["readiness"].get("failed")]
    warmup_failures = [{"orderIndex": s["orderIndex"], "role": s["role"],
                        "reason": s["readiness"].get("failReason")}
                       for s in samples if s.get("warmup") and s["readiness"].get("failed")]
    roles = sorted({s["role"] for s in counted})
    stats: dict = {"byMetric": {}, "paired": {}, "failures": failures,
                   "warmupFailures": warmup_failures}
    for metric in metrics:
        table: dict = {}
        for role in roles:
            table[role] = {}
            for phase in phases:
                rows = [(s["orderIndex"], s["readiness"][metric]) for s in counted
                        if s["role"] == role and s["phase"] == phase
                        and s["readiness"].get(metric) is not None]
                values = [value for _, value in rows]
                group = summarize_group(values, bootstrap=bootstrap, seed=seed,
                                        madz_threshold=madz_threshold)
                group["samplesRaw"] = [order for order, _ in rows]
                for entry in group["excluded"]:
                    entry["orderIndex"] = rows[entry["index"]][0]
                dropped = {entry["orderIndex"] for entry in group["excluded"]}
                group["samplesKept"] = [order for order, _ in rows if order not in dropped]
                phase_rows = [s for s in counted if s["role"] == role and s["phase"] == phase]
                group["nSamples"] = len(phase_rows)
                group["nMissing"] = sum(1 for s in phase_rows if s["readiness"].get(metric) is None)
                table[role][phase] = group
        stats["byMetric"][metric] = table
    for metric in metrics:
        stats["paired"][metric] = {}
        for phase in phases:
            if not {"pd", "legacy"} <= set(roles):
                stats["paired"][metric][phase] = {"available": False,
                                                  "reason": "fewer than two roles sampled"}
                continue
            by_round: dict = {}
            for s in counted:
                if s["phase"] != phase or s["readiness"].get(metric) is None:
                    continue
                by_round.setdefault(s["round"], {})[s["role"]] = s["readiness"][metric]
            pairs = [(r, by_round[r]["pd"], by_round[r]["legacy"])
                     for r in sorted(by_round) if {"pd", "legacy"} <= set(by_round[r])]
            diffs = [a - b for _, a, b in pairs]
            entry = {"available": True, "rounds": [r for r, _, _ in pairs],
                     "diffsPdMinusLegacy": diffs,
                     "median": statistics.median(diffs) if diffs else None,
                     "ci95Median": bootstrap_ci_median(diffs, samples=bootstrap, seed=seed)
                     if len(diffs) >= 2 else None}
            stats["paired"][metric][phase] = entry
    return stats


# --------------------------------------------------------------------------
# Windows host runtime (external observation only).
# --------------------------------------------------------------------------

def set_process_dpi_aware() -> str:
    try:
        setter = USER32.SetProcessDpiAwarenessContext
        setter.restype = W.BOOL
        setter.argtypes = [C.c_void_p]
        if setter(C.c_void_p(-4)):  # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            return "per-monitor-v2"
    except AttributeError:
        pass
    if USER32.SetProcessDPIAware():
        return "system"
    return "unaware"


def cloaked(hwnd: int) -> bool:
    value = W.DWORD()
    result = DWMAPI.DwmGetWindowAttribute(hwnd, 14, C.byref(value), C.sizeof(value))
    return result == 0 and value.value != 0


def windows_of(pid: int) -> list[dict]:
    rows: list[dict] = []

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
        rows.append({"hwnd": int(hwnd), "title": title.value,
                     "clientWidth": rect.right - rect.left,
                     "clientHeight": rect.bottom - rect.top,
                     "minimized": bool(USER32.IsIconic(hwnd)),
                     "cloaked": cloaked(hwnd),
                     "dpi": int(USER32.GetDpiForWindow(hwnd) or 0)})
        return True

    USER32.EnumWindows(callback, 0)
    return rows


def static_usable(row: dict) -> bool:
    return (row["clientWidth"] >= 200 and row["clientHeight"] >= 120
            and not row["minimized"] and not row["cloaked"])


def responsive(hwnd: int) -> bool:
    result = C.c_size_t()
    ok = USER32.SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, C.byref(result))
    return bool(ok)


def set_topmost(hwnd: int, topmost: bool) -> bool:
    return bool(USER32.SetWindowPos(hwnd, HWND_TOPMOST if topmost else HWND_NOTOPMOST,
                                    0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))


def screen_client_origin(hwnd: int) -> tuple[int, int] | None:
    point = W.POINT(0, 0)
    if not USER32.ClientToScreen(hwnd, C.byref(point)):
        return None
    return point.x, point.y


class ClientCapturer:
    """Downsamples a screen region into a fixed 128x80 32bpp buffer via GDI."""

    def __init__(self, width: int = CAPTURE_WIDTH, height: int = CAPTURE_HEIGHT):
        self.width = width
        self.height = height
        self.buffer_size = width * height * 4
        self.screen_dc = USER32.GetDC(0)
        self.mem_dc = GDI32.CreateCompatibleDC(self.screen_dc)
        bmi = BITMAPINFO()
        bmi.bmiHeader.biSize = C.sizeof(BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = width
        bmi.bmiHeader.biHeight = -height  # top-down
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bmi.bmiHeader.biCompression = 0  # BI_RGB
        self.bits = C.c_void_p()
        self.bitmap = GDI32.CreateDIBSection(self.mem_dc, C.byref(bmi), 0,
                                             C.byref(self.bits), None, 0)
        self.old_bitmap = GDI32.SelectObject(self.mem_dc, self.bitmap)
        GDI32.SetStretchBltMode(self.mem_dc, HALFTONE)

    def capture(self, x: int, y: int, width: int, height: int) -> bytes | None:
        if width <= 0 or height <= 0 or not self.bits:
            return None
        if not GDI32.StretchBlt(self.mem_dc, 0, 0, self.width, self.height, self.screen_dc,
                                x, y, width, height, SRCCOPY):
            return None
        return C.string_at(self.bits, self.buffer_size)

    def close(self) -> None:
        if self.old_bitmap:
            GDI32.SelectObject(self.mem_dc, self.old_bitmap)
            self.old_bitmap = None
        if self.bitmap:
            GDI32.DeleteObject(self.bitmap)
            self.bitmap = None
        if self.mem_dc:
            GDI32.DeleteDC(self.mem_dc)
            self.mem_dc = None
        if self.screen_dc:
            USER32.ReleaseDC(0, self.screen_dc)
            self.screen_dc = None


def host_memory() -> dict:
    memory = MEMORYSTATUSEX()
    memory.dwLength = C.sizeof(memory)
    if not C.WinDLL("kernel32").GlobalMemoryStatusEx(C.byref(memory)):
        return {}
    return {"memoryLoadPercent": int(memory.dwMemoryLoad),
            "availablePhysicalBytes": int(memory.ullAvailPhys)}


def cold_flood(mib: int, source_root: str = FLOOD_SOURCE_DEFAULT) -> dict:
    """Best-effort standby aging: sequential read of largest top-level files."""
    began = time.monotonic()
    read = 0
    files = 0
    record = {"enabled": True, "sourceRoot": source_root, "targetMib": mib}
    if mib <= 0:
        record.update({"enabled": False, "bytesRead": 0, "files": 0, "seconds": 0.0})
        return record
    root = Path(source_root)
    try:
        candidates = []
        for entry in os.scandir(root):
            try:
                if entry.is_file():
                    candidates.append((entry.stat().st_size, entry.path))
            except OSError:
                continue
        candidates.sort(key=lambda item: (-item[0], item[1]))
        target_bytes = mib * 1024 * 1024
        for _size, path in candidates:
            if read >= target_bytes or time.monotonic() - began > FLOOD_BUDGET_SECONDS:
                break
            try:
                with open(path, "rb") as stream:
                    while chunk := stream.read(1 << 20):
                        read += len(chunk)
                files += 1
            except OSError:
                continue
    except OSError as error:
        record["error"] = str(error)
    record.update({"bytesRead": read, "files": files,
                   "seconds": round(time.monotonic() - began, 3)})
    return record


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


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


def enumerate_profile(roots: list[Path]) -> dict:
    record = {}
    for root in roots:
        key = str(root)
        if not root.is_dir():
            record[key] = {"present": False}
            continue
        files = 0
        total = 0
        try:
            for path in root.rglob("*"):
                if path.is_file():
                    files += 1
                    total += path.stat().st_size
        except OSError as error:
            record[key] = {"present": True, "error": str(error)}
            continue
        record[key] = {"present": True, "fileCount": files, "bytes": total}
    return record


def choose_capture_target(rows: list[dict], locked_hwnd: int) -> dict | None:
    candidates = [row for row in rows if static_usable(row)]
    if not candidates:
        return None
    chosen = next((row for row in candidates if row["hwnd"] == locked_hwnd), None)
    if chosen is None:
        chosen = max(candidates, key=lambda row: row["clientWidth"] * row["clientHeight"])
    origin = screen_client_origin(chosen["hwnd"])
    if origin is None:
        return None
    return {**chosen, "x": origin[0], "y": origin[1]}


def do_shutdown(process: subprocess.Popen, hwnd: int, observer: WindowsObserver | None) -> dict:
    shutdown = {"wmCloseSent": False, "normalExit": None, "exitCode": None,
                "forcedCleanup": [], "processesStillAlive": []}
    if hwnd and process.poll() is None:
        shutdown["wmCloseSent"] = bool(USER32.PostMessageW(hwnd, WM_CLOSE, 0, 0))
        try:
            shutdown["exitCode"] = process.wait(15)
            shutdown["normalExit"] = True
        except subprocess.TimeoutExpired:
            shutdown["normalExit"] = False
    if not shutdown["normalExit"] and process.poll() is None:
        if observer is not None:
            shutdown["forcedCleanup"] = observer.terminate_owned()
        else:
            process.kill()
        try:
            shutdown["exitCode"] = process.wait(5)
        except subprocess.TimeoutExpired:
            pass
    time.sleep(1.0)
    if observer is not None:
        rows, _errors = observer.tree(require_root=False)
        shutdown["processesStillAlive"] = [{"pid": row["pid"], "image": row.get("executable", "")}
                                           for row in rows]
    return shutdown


def run_once(args, exe_record: dict, deployed: Path, fixture: Path, role: str, phase: str,
             round_no: int, slot: int, order_index: int, capturer: ClientCapturer,
             warmup: bool) -> dict:
    env = prepare_environment(fixture)
    cleared = clear_application_cache(fixture) if phase == "cold" else []
    flood = cold_flood(args.cold_flood_mib, args.flood_source) if phase == "cold" else None
    time.sleep(args.inter_run_gap)
    host = host_memory()
    began_ns = time.perf_counter_ns()
    began_mono = time.monotonic()
    process = subprocess.Popen([str(deployed)], cwd=str(fixture), env=env,
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    created_ns = time.perf_counter_ns()
    deadline = time.monotonic() + args.startup_timeout

    l0_ns = l1_ns = l2_ns = None
    topmost_ns = None
    topmost_removed = False
    window = None
    locked_hwnd = 0
    frame_times: list[float] = []
    frame_hashes: list[str] = []
    frame_structured: list[bool] = []
    transitions: list[float] = []
    first_structured_ns = None
    capture_errors = 0
    recent: deque = deque(maxlen=args.settle_k)
    failure = None
    poll_index = 0

    def ms(stamp_ns: int | None) -> float | None:
        return None if stamp_ns is None else round((stamp_ns - began_ns) / 1e6, 3)

    while time.monotonic() < deadline:
        if process.poll() is not None and l1_ns is None:
            failure = f"process exited early (code {process.poll()}) before window-ready"
            break
        rows = windows_of(process.pid)
        target = choose_capture_target(rows, locked_hwnd)
        now_ns = time.perf_counter_ns()
        if target is not None:
            if l0_ns is None:
                l0_ns = now_ns
                if set_topmost(target["hwnd"], True):
                    topmost_ns = now_ns
            if locked_hwnd != target["hwnd"]:
                locked_hwnd = target["hwnd"]
            data = capturer.capture(target["x"], target["y"],
                                    target["clientWidth"], target["clientHeight"])
            if data is None:
                capture_errors += 1
            else:
                stamp_ms = round((now_ns - began_ns) / 1e6, 3)
                digest = hashlib.blake2b(data, digest_size=16).hexdigest()
                structured = frame_is_structured(data, args.distinct_threshold)
                if frame_hashes and digest != frame_hashes[-1]:
                    transitions.append(stamp_ms)
                if structured and first_structured_ns is None:
                    first_structured_ns = now_ns
                frame_times.append(stamp_ms)
                frame_hashes.append(digest)
                frame_structured.append(structured)
                recent.append((digest, structured))
                if l2_ns is None and l1_ns is not None and len(recent) == args.settle_k:
                    if all(item[0] == recent[-1][0] for item in recent) and recent[-1][1]:
                        l2_ns = now_ns
            if l1_ns is None and responsive(target["hwnd"]):
                l1_ns = time.perf_counter_ns()
                window = {"title": target["title"], "clientWidth": target["clientWidth"],
                          "clientHeight": target["clientHeight"], "dpi": target["dpi"]}
        if l1_ns is not None and l2_ns is not None:
            break
        poll_index += 1
        next_poll = began_mono + poll_index * (args.poll_ms / 1000.0)
        delay = next_poll - time.monotonic()
        if delay > 0:
            time.sleep(delay)

    if locked_hwnd:
        topmost_removed = set_topmost(locked_hwnd, False)

    observer = None
    if process.poll() is None:
        try:
            observer = WindowsObserver(process.pid)
        except (OSError, EvidenceError):
            observer = None
    shutdown = do_shutdown(process, locked_hwnd if l1_ns is not None else 0, observer)
    if failure is None and l1_ns is None:
        failure = f"no window-ready within {args.startup_timeout}s timeout"

    frame_log_limit = 256
    readiness = {
        "failed": failure is not None, "failReason": failure,
        "l0Ms": ms(l0_ns), "l1Ms": ms(l1_ns), "l2Ms": ms(l2_ns),
        "l2MinusL1Ms": (round((l2_ns - l1_ns) / 1e6, 3)) if l1_ns is not None and l2_ns is not None else None,
        "startupTimeoutS": args.startup_timeout, "pollMs": args.poll_ms, "settleK": args.settle_k,
        "distinctThreshold": args.distinct_threshold,
        "captureSize": [capturer.width, capturer.height],
        "window": window, "topmostAppliedMs": ms(topmost_ns), "topmostRemoved": topmost_removed,
        "framesCaptured": len(frame_hashes), "captureErrors": capture_errors, "polls": poll_index,
        "transitionsObserved": len(transitions),
        "transitionTimesMs": transitions[:frame_log_limit],
        "transitionTimesTruncated": len(transitions) > frame_log_limit,
        "firstStructuredMs": ms(first_structured_ns),
        "frameHashFirst": frame_hashes[0] if frame_hashes else None,
        "frameHashLast": frame_hashes[-1] if frame_hashes else None,
        "frameLog": [[frame_times[i], frame_hashes[i], frame_structured[i]]
                     for i in range(min(frame_log_limit, len(frame_hashes)))],
        "frameLogTruncated": len(frame_hashes) > frame_log_limit,
    }
    return {
        "orderIndex": order_index, "warmup": warmup, "phase": phase, "round": round_no,
        "slot": slot, "role": role,
        "utcStart": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "monotonicMs": round(began_mono * 1000, 3),
        "hostMemoryLoadPercent": host.get("memoryLoadPercent"),
        "hostAvailablePhysicalBytes": host.get("availablePhysicalBytes"),
        "exe": {"path": exe_record["path"], "bytes": exe_record["bytes"],
                "sha256": exe_record["sha256"]},
        "fixture": str(fixture), "cacheDirsCleared": cleared, "coldFlood": flood,
        "process": {"pid": process.pid, "createMs": round((created_ns - began_ns) / 1e6, 3),
                    **shutdown},
        "readiness": readiness,
    }


LIMITATIONS = [
    "L2 is a client-area frame-settle approximation (GDI screen capture, 128x80 downsample),",
    "not a compositor-presented pixel and not an input receipt; the strict WGC protocol remains",
    "the only content-paint-grade evidence path.",
    "A shell window whose first captured frame already looks final is credited with the earliest",
    "observable settle time; an animated UI never settles (L2 censored, excluded from L2 stats).",
    "The TOPMOST pin at L0 is a harness intervention applied identically to both products;",
    "it guarantees unoccluded capture but is recorded per run.",
    "Cold = fixture app-cache cleared + best-effort standby aging (1 GiB sequential read); OS",
    "file-cache/standby residency of EXE pages, Windows prefetcher and AV state stay uncontrolled.",
    "Every launch is a fresh process (new PID); per-product first-run cache building is absorbed",
    "by one uncounted warmup launch per role.",
    "WM_NULL responsiveness (L1) can block a poll for up to 2 s on a hung window; the block is",
    "inside the timed window exactly as in pd_performance_sample.py.",
    "No OS reboot, no CPU-load cooldown, no affinity/priority pinning; host memory state is",
    "recorded per run for drift audit instead of being controlled.",
    "This protocol is a local measurement tool; it is not the release performance gate and does",
    "not change the release HOLD state.",
]


def protocol_block(args, roles: list[str], order_cold: list[list[str]],
                   order_warm: list[list[str]]) -> dict:
    return {
        "schemaVersion": PROTOCOL_SCHEMA_VERSION,
        "readiness": {
            "L0": "first visible, non-minimized, non-cloaked root-PID window with client >= 200x120; pinned TOPMOST",
            "L1": "L0 window that answers WM_NULL within 2 s; timer from pre-CreateProcess to observation",
            "L2": "first poll at/after L1 where the last settleK captured frames are byte-identical AND structured",
            "structured": f">= {args.distinct_threshold} distinct byte values at capture resolution",
            "pollMs": args.poll_ms, "settleK": args.settle_k,
            "captureSize": [CAPTURE_WIDTH, CAPTURE_HEIGHT],
            "startupTimeoutS": args.startup_timeout,
        },
        "cold": {"appCacheDirsCleared": ["cache", "webview", "Nickel/cache"],
                 "standbyAging": {"mib": args.cold_flood_mib, "source": args.flood_source,
                                  "budgetSeconds": FLOOD_BUDGET_SECONDS, "guarantee": "best-effort only"},
                 "process": "fresh PID each launch"},
        "warm": {"appCacheDirsCleared": [], "standbyAging": None, "process": "fresh PID each launch"},
        "sampling": {"roles": roles, "rounds": args.rounds,
                     "starterAlternation": "even round = listed order, odd round = reversed",
                     "orderCold": order_cold, "orderWarm": order_warm,
                     "warmupPerRole": args.warmups, "interRunGapS": args.inter_run_gap,
                     "phaseOrder": ["warmup", "cold", "warm"]},
        "statistics": {"median": True,
                       "iqr": "inclusive linear-interpolation quantiles",
                       "madScaled": 1.4826,
                       "bootstrap": {"B": args.bootstrap, "seed": args.seed, "level": 0.95,
                                     "method": "percentile of resampled medians"},
                       "outlierRule": {"modifiedZ": {"factor": 0.6745, "threshold": args.madz_threshold,
                                                     "condition": "MAD > 0"},
                                       "fallback": "Tukey extreme fences when MAD == 0",
                                       "minimumGroup": OUTLIER_MINIMUM_GROUP},
                       "paired": "per-round pd-legacy differences per phase, same bootstrap"},
        "failureRule": "no L1 within timeout => failure; listed, excluded from timing stats, never re-run",
    }


def format_summary(document: dict) -> str:
    lines = [f"startup-protocol summary (schemaVersion {document['schemaVersion']})",
             f"generated: {document['generatedUtc']}",
             f"machine: {document['machine'].get('processorModel', '?')} | "
             f"{document['machine'].get('TotalPhysicalBytes', '?')}"]
    for role, exe in document["exes"].items():
        lines.append(f"exe[{role}]: {exe['path']} bytes={exe['bytes']} sha256={exe['sha256']}")
    if "pd" in document["exes"] and "legacy" not in document["exes"]:
        lines.append("legacy EXE: NOT PROVIDED (missing artifact) - single-sided run, no A/B comparison produced")
    for metric, table in document["stats"]["byMetric"].items():
        for role, phases in table.items():
            for phase, group in phases.items():
                stats = group["stats"]
                if not stats:
                    lines.append(f"{metric} {role}/{phase}: no usable samples "
                                 f"(nSamples={group['nSamples']}, missing={group['nMissing']})")
                    continue
                ci = group["ci95Median"]
                ci_text = f"[{ci[0]:.1f}, {ci[1]:.1f}]" if ci else "n/a"
                lines.append(f"{metric} {role}/{phase}: n={group['nKept']}/{group['nRaw']} "
                             f"median={stats['median']:.1f} iqr={stats['iqr']:.1f} mad={stats['mad']:.1f} "
                             f"ci95={ci_text} excluded={len(group['excluded'])} rule={group['outlierRule']}")
    for metric, phases in document["stats"]["paired"].items():
        for phase, entry in phases.items():
            if not entry.get("available"):
                continue
            ci = entry.get("ci95Median")
            ci_text = f"[{ci[0]:.1f}, {ci[1]:.1f}]" if ci else "n/a"
            median_text = f"{entry['median']:.1f}" if entry["median"] is not None else "n/a"
            lines.append(f"paired {metric}/{phase} (pd-legacy): rounds={len(entry['rounds'])} "
                         f"median={median_text} ci95={ci_text}")
    lines.append(f"failures: {len(document['stats']['failures'])} "
                 f"(warmup failures: {len(document['stats']['warmupFailures'])})")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pd-exe")
    parser.add_argument("--legacy-exe")
    parser.add_argument("--output", required=True)
    parser.add_argument("--rounds", type=int, default=15)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--poll-ms", type=int, default=12)
    parser.add_argument("--settle-k", type=int, default=5)
    parser.add_argument("--distinct-threshold", type=int, default=12)
    parser.add_argument("--startup-timeout", type=float, default=30.0)
    parser.add_argument("--inter-run-gap", type=float, default=1.0)
    parser.add_argument("--cold-flood-mib", type=int, default=1024)
    parser.add_argument("--flood-source", default=FLOOD_SOURCE_DEFAULT)
    parser.add_argument("--bootstrap", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=20261124)
    parser.add_argument("--madz-threshold", type=float, default=3.5)
    args = parser.parse_args()

    if args.rounds < 1:
        raise SystemExit("--rounds must be >= 1")
    if args.settle_k < 1:
        raise SystemExit("--settle-k must be >= 1")

    dpi_mode = set_process_dpi_aware()
    roles: list[str] = []
    exe_records: dict[str, dict] = {}
    for role, given in (("pd", args.pd_exe), ("legacy", args.legacy_exe)):
        if not given:
            continue
        path = Path(given).resolve()
        if not path.is_file():
            raise SystemExit(f"missing executable for role {role}: {path}")
        exe_records[role] = {"path": str(path), "bytes": path.stat().st_size,
                             "sha256": sha256_file(path)}
        roles.append(role)
    if not roles:
        raise SystemExit("no role executable provided (use --pd-exe and/or --legacy-exe)")

    output = Path(args.output).resolve()
    if output.exists():
        raise SystemExit("output directory must be unused (evidence is never overwritten)")
    fixture = output / "fixture"
    fixture.mkdir(parents=True)
    raw = output / "raw"
    raw.mkdir()

    deployed: dict[str, Path] = {}
    for role in roles:
        target = fixture / Path(exe_records[role]["path"]).name
        with Path(exe_records[role]["path"]).open("rb") as source, target.open("xb") as sink:
            while chunk := source.read(1024 * 1024):
                sink.write(chunk)
        if sha256_file(target) != exe_records[role]["sha256"]:
            raise SystemExit(f"deployed fixture executable does not match the measured source ({role})")
        deployed[role] = target

    marker = {"schemaVersion": 1, "format": "nickel-portable", "packageId": "0" * 31 + "1",
              "name": "Perf fixture", "version": "0.1.0", "instanceName": "perf-fixture",
              "payload": "Nickel/pack.mrpack", "gameDirectory": ".minecraft",
              "dataDirectory": "Nickel", "launcher": {"path": "Nickel.exe", "os": "windows",
                                                      "arch": "x64"},
              "personalization": None}
    (fixture / "nickel.portable.json").write_text(json.dumps(marker, indent=2) + "\n", encoding="utf-8")
    for name in ("Nickel", ".minecraft", "cache", "temp"):
        (fixture / name).mkdir(exist_ok=True)

    env_preview = prepare_environment(fixture)
    env_overrides = {key: env_preview[key] for key in ("TEMP", "TMP", "APPDATA", "LOCALAPPDATA",
                                                       "XDG_CONFIG_HOME", "XDG_DATA_HOME", "HOME",
                                                       "WEBVIEW2_USER_DATA_FOLDER")}
    profile_roots = [Path(os.environ["LOCALAPPDATA"]) / "studio.nickel.launcher",
                     Path(os.environ["APPDATA"]) / "studio.nickel.launcher"]
    profile_before = enumerate_profile(profile_roots)

    order_cold = build_ab_order(args.rounds, roles)
    order_warm = build_ab_order(args.rounds, roles)
    capturer = ClientCapturer()
    samples: list[dict] = []
    order_index = 0

    def execute(role: str, phase: str, round_no: int, slot: int, warmup: bool) -> dict:
        nonlocal order_index
        sample = run_once(args, exe_records[role], deployed[role], fixture, role, phase,
                          round_no, slot, order_index, capturer, warmup)
        samples.append(sample)
        (raw / f"sample-{order_index:03d}.json").write_text(
            json.dumps(sample, indent=2) + "\n", encoding="utf-8")
        readiness = sample["readiness"]
        print(f"[{order_index:03d}] {'warmup' if warmup else phase} r{round_no} s{slot} {role}: "
              f"l1={readiness['l1Ms']} l2={readiness['l2Ms']} frames={readiness['framesCaptured']} "
              f"transitions={readiness['transitionsObserved']}"
              f"{' FAILED=' + str(readiness['failReason']) if readiness['failed'] else ''}",
              flush=True)
        order_index += 1
        return sample

    try:
        for role in roles:
            execute(role, "warm", -1, 0, True)
        for round_no, slots in enumerate(order_cold):
            for slot, role in enumerate(slots):
                execute(role, "cold", round_no, slot, False)
        for round_no, slots in enumerate(order_warm):
            for slot, role in enumerate(slots):
                execute(role, "warm", round_no, slot, False)
    finally:
        capturer.close()

    profile_after = enumerate_profile(profile_roots)
    stats = assemble_stats(samples, bootstrap=args.bootstrap, seed=args.seed,
                           madz_threshold=args.madz_threshold)
    document = {
        "schemaVersion": PROTOCOL_SCHEMA_VERSION,
        "protocolName": "startup-protocol",
        "generatedUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "protocol": protocol_block(args, roles, order_cold, order_warm),
        "machine": machine_metadata(),
        "dpiAwareness": dpi_mode,
        "exes": {role: exe_records[role] for role in roles},
        "fixture": {"path": str(fixture), "marker": marker,
                    "environmentOverrides": env_overrides,
                    "profileBefore": profile_before, "profileAfter": profile_after},
        "orderLog": [{"orderIndex": s["orderIndex"], "phase": s["phase"], "round": s["round"],
                      "slot": s["slot"], "role": s["role"], "warmup": s["warmup"],
                      "utcStart": s["utcStart"], "monotonicMs": s["monotonicMs"],
                      "hostMemoryLoadPercent": s["hostMemoryLoadPercent"]}
                     for s in samples],
        "samples": samples,
        "stats": stats,
        "limitations": LIMITATIONS,
    }
    (output / "summary.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    summary_text = format_summary(document)
    (output / "summary.txt").write_text(summary_text, encoding="utf-8")
    print(summary_text, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
