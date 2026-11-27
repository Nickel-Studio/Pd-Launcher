"""Read-only receiver checks for the existing production observation protocol."""
from __future__ import annotations

import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import importlib
from io import BytesIO
import json
from pathlib import Path
import sys

from PIL import Image

MAX_FRAME_BYTES = 4 * 1024 * 1024
MAX_FRAME_PIXELS = 8 * 1024 * 1024
MAX_JSON_BYTES = 16 * 1024 * 1024


def _unique_json_object(pairs):
    value = {}
    for name, item in pairs:
        if name in value:
            raise ValueError("Duplicate JSON field: " + name)
        value[name] = item
    return value


def _reject_json_constant(value):
    raise ValueError("Nonfinite JSON value: " + value)


def _bounded_bytes(path: Path, maximum_bytes: int) -> bytes:
    for item in (path, *path.parents):
        if (item.is_symlink() or (hasattr(item, "is_junction") and item.is_junction())
                or (item.exists() and getattr(item.stat(follow_symlinks=False), "st_file_attributes", 0) & 0x400)):
            raise ValueError("Evidence cannot follow a link/reparse redirect: " + str(path))
    if not path.is_file():
        raise ValueError("Evidence must be an existing regular file: " + str(path))
    before = path.stat()
    with path.open("rb") as stream:
        raw = stream.read(maximum_bytes + 1)
    after = path.stat()
    if not 0 < len(raw) <= maximum_bytes:
        raise ValueError("Evidence exceeds its bounded input size: " + str(path))
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise ValueError("Evidence changed while reading: " + str(path))
    return raw


def read_bound_json(path: Path, root: Path) -> tuple[object, dict]:
    target = (path if path.is_absolute() else root / path).absolute()
    try:
        relative = target.relative_to(root)
    except ValueError as error:
        raise ValueError("JSON input is outside the received evidence root") from error
    if ".." in relative.parts or ":" in relative.as_posix():
        raise ValueError("JSON input cannot traverse the received evidence root")
    raw = _bounded_bytes(target, MAX_JSON_BYTES)
    document = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_unique_json_object,
                          parse_constant=_reject_json_constant)
    return document, {"path": relative.as_posix(), "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}


def verify_bound_files(root: Path, records: list[dict]) -> None:
    for row in records:
        raw = _bounded_bytes(root / row["path"], MAX_JSON_BYTES)
        if len(raw) != row["bytes"] or hashlib.sha256(raw).hexdigest() != row["sha256"]:
            raise ValueError("Received input bytes changed: " + row["path"])


def verify_loaded_tool_origins(tools: Path, files: dict) -> list[dict]:
    loaded = []
    for name in files:
        path = tools.parent.parent / name
        if path.suffix != ".py" or path.stem not in sys.modules:
            continue
        module = sys.modules[path.stem]
        origin = getattr(module, "__file__", None)
        spec_origin = getattr(getattr(module, "__spec__", None), "origin", None)
        if (not isinstance(origin, str) or not isinstance(spec_origin, str)
                or Path(origin).resolve() != path.resolve() or Path(spec_origin).resolve() != path.resolve()):
            raise ValueError("Loaded validator module is outside the selected publication: " + path.stem)
        loaded.append({"module": path.stem, **{key: files[name][key] for key in ("sha256", "bytes")}, "path": name})
    return loaded


def load_protocol_tools(tools: Path) -> tuple[dict, dict]:
    tools = tools.absolute()
    publication_root = tools.parent.parent
    if tools != publication_root / "tests" / "native-release" or (publication_root / ".publisher.lock").exists():
        raise ValueError("Selected tools need a completed existing native-release publication")
    publication, publication_ref = read_bound_json(publication_root / "publication-complete.json", publication_root)
    snapshot, snapshot_ref = read_bound_json(publication_root / "snapshot.json", publication_root)
    if (not isinstance(publication, dict) or not isinstance(snapshot, dict)
            or publication.get("publicationComplete") is not True
            or publication.get("snapshotFileSha256") != snapshot_ref["sha256"]
            or publication.get("receiverManifestSha256") != snapshot_ref["sha256"]
            or publication.get("sourceRoot") != snapshot.get("sourceRoot")
            or Path(publication.get("sourceRoot", "")).resolve() != publication_root.resolve()
            or Path(publication.get("snapshot", "")).resolve() != (publication_root / "snapshot.json").resolve()
            or publication.get("sourceCommit") != snapshot.get("sourceCommit")):
        raise ValueError("Selected tool publication does not bind its actual snapshot bytes/root")
    files = snapshot.get("files")
    if not isinstance(files, dict) or not files or publication.get("sourceFiles") != len(files):
        raise ValueError("Selected tool publication has no complete file inventory")
    records = [snapshot_ref, publication_ref]
    for name, row in files.items():
        if (not isinstance(name, str) or not name.startswith("tests/native-release/")
                or Path(name).parent.as_posix() != "tests/native-release" or ":" in name or "\\" in name
                or not isinstance(row, dict) or type(row.get("bytes")) is not int or row["bytes"] < 1
                or not isinstance(row.get("sha256"), str) or len(row["sha256"]) != 64
                or any(char not in "0123456789abcdef" for char in row["sha256"])):
            raise ValueError("Selected tool inventory contains a malformed file binding")
        records.append({"path": name, "sha256": row["sha256"], "bytes": row["bytes"]})
    if publication.get("sourceBytes") != sum(row["bytes"] for row in files.values()):
        raise ValueError("Selected tool publication byte count differs from its inventory")
    verify_bound_files(publication_root, records)
    verify_loaded_tool_origins(tools, files)
    sys.path.insert(0, str(tools))
    modules = {name: importlib.import_module(name) for name in
               ("evidence_common", "performance_runner", "release_evidence", "windows_metrics")}
    loaded = verify_loaded_tool_origins(tools, files)
    if not set(modules) <= {row["module"] for row in loaded}:
        raise ValueError("Required validator modules are absent from the selected publication")
    verify_bound_files(publication_root, records)
    return modules, {"root": str(publication_root), "sourceCommit": snapshot.get("sourceCommit"),
                     "snapshot": snapshot_ref, "publication": publication_ref, "files": records,
                     "loadedModules": loaded}



def referenced_frame(receipt: dict, frame: dict) -> bool:
    if not isinstance(frame, dict) or set(frame) != {"path", "sha256", "bytes"}:
        raise ValueError("Frame must be an exact existing protocol file reference")
    observation = receipt.get("uiObservation", {}) if isinstance(receipt, dict) else {}
    if not isinstance(observation, dict):
        return False
    for name in ("scenarioAppliedBeforePaint", "paintComplete", "flushComplete",
                 "presentDwmConfirmed", "inputReady"):
        marker = observation.get(name, {})
        rows = marker.get("rawEvidence", []) if isinstance(marker, dict) else []
        if isinstance(rows, list) and any(
            isinstance(row, dict) and all(row.get(key) == frame[key] for key in frame)
            for row in rows
        ):
            return True
    return False


def inspect_png(path: Path, state: dict, *, expected_sha256: str | None = None) -> dict:
    with path.open("rb") as stream:
        raw = stream.read(MAX_FRAME_BYTES + 1)
    if not 0 < len(raw) <= MAX_FRAME_BYTES:
        raise ValueError("PNG exceeds the bounded evidence input size")
    if expected_sha256 is not None and hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError("Decoded PNG input differs from the recorded frame hash")
    header = raw[:33]
    if (len(header) != 33 or header[:8] != b"\x89PNG\r\n\x1a\n"
            or header[8:16] != b"\x00\x00\x00\rIHDR" or header[24] != 8
            or header[25] not in {2, 6}):
        raise ValueError("Frame must be an RGB/RGBA eight-bit PNG, not renamed image bytes")
    width = int.from_bytes(header[16:20], "big")
    height = int.from_bytes(header[20:24], "big")
    if width < 1 or height < 1 or width > MAX_FRAME_PIXELS // height:
        raise ValueError("PNG pixel budget exceeded before decoding")
    logical_width, logical_height = state["windowLogicalSize"]
    dpi = state["dpiPercent"]
    expected = ((logical_width * dpi + 50) // 100, (logical_height * dpi + 50) // 100)
    if (width, height) != expected:
        raise ValueError("Frame dimensions do not match this scenario's logical size/DPI")
    # Verify and decode the same bounded bytes, not two potentially different path reads.
    with Image.open(BytesIO(raw)) as image:
        if image.format != "PNG" or getattr(image, "n_frames", 1) != 1:
            raise ValueError("Frame is not a single PNG image")
        image.verify()
    with Image.open(BytesIO(raw)) as image:
        image.load()
        rgba = image.convert("RGBA")
        try:
            visible_nonblack = False
            visible_colors = set()
            pixels = rgba.load()
            for y in range(height):
                for x in range(width):
                    red, green, blue, alpha = pixels[x, y]
                    if not alpha:
                        continue
                    visible_nonblack |= bool(red or green or blue)
                    visible_colors.add((red, green, blue))
                    if visible_nonblack and len(visible_colors) > 1:
                        break
                if visible_nonblack and len(visible_colors) > 1:
                    break
            if not visible_nonblack or len(visible_colors) < 2:
                raise ValueError("Black, transparent or uniform pixels are not a rendered UI proof")
        finally:
            rgba.close()
    return {"width": width, "height": height, "decodedPlaneBytes": width * height * 4,
            "visibleNonblack": True, "nonuniform": True}


def check_process_identity(actual: dict, context: dict, executable: Path) -> None:
    name = actual.get("executable")
    if (type(actual.get("pid")) is not int or actual["pid"] != context["rootPid"]
            or type(actual.get("creationTime100ns")) is not int
            or actual["creationTime100ns"] != context["rootCreationTime100ns"]
            or not isinstance(name, str) or not name or not Path(name).is_absolute()
            or Path(name).resolve() != executable.resolve()):
        raise ValueError("Observed kernel process is not this same executable/birth identity")


def observe_live_window(observer_type, context: dict, receipt: dict, executable: Path,
                        image: dict) -> dict:
    observer = observer_type(context["rootPid"], root_creation=context["rootCreationTime100ns"])
    actual = observer.identity(context["rootPid"])
    check_process_identity(actual, context, executable)
    hwnd = receipt["uiObservation"]["windowIdentity"]["hwnd"]
    if type(hwnd) is not int or not 0 < hwnd < 1 << (8 * C.sizeof(C.c_void_p)):
        raise ValueError("Receipt HWND is not a positive pointer-sized window handle")
    observer.user.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
    observer.user.GetWindowThreadProcessId.restype = W.DWORD
    observer.user.IsWindowVisible.argtypes = [W.HWND]
    observer.user.IsWindowVisible.restype = W.BOOL
    observer.user.IsIconic.argtypes = [W.HWND]
    observer.user.IsIconic.restype = W.BOOL
    observer.user.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
    observer.user.GetClientRect.restype = W.BOOL
    observer.user.GetDpiForWindow.argtypes = [W.HWND]
    observer.user.GetDpiForWindow.restype = W.UINT
    observer.user.SetThreadDpiAwarenessContext.argtypes = [W.HANDLE]
    observer.user.SetThreadDpiAwarenessContext.restype = W.HANDLE
    # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 changes only this receiver thread.
    previous_dpi = observer.user.SetThreadDpiAwarenessContext(C.c_void_p(-4))
    if not previous_dpi:
        raise ValueError("Physical client geometry observation requires per-monitor DPI awareness")
    try:
        owner, rect = W.DWORD(), W.RECT()
        if (not observer.user.GetWindowThreadProcessId(hwnd, C.byref(owner))
                or owner.value != context["rootPid"] or not observer.user.IsWindowVisible(hwnd)
                or observer.user.IsIconic(hwnd) or not observer.user.GetClientRect(hwnd, C.byref(rect))):
            raise ValueError("The receipt HWND is not a live visible nonminimized owned window")
        client = [rect.right - rect.left, rect.bottom - rect.top]
        if client != [image["width"], image["height"]]:
            raise ValueError("Live client geometry differs from the captured scenario")
        dpi = observer.user.GetDpiForWindow(hwnd)
        if not dpi or dpi * 100 != context["expectedState"]["dpiPercent"] * 96:
            raise ValueError("Actual Windows DPI differs from the requested scenario; Qt scale alone is not OS DPI evidence")
        check_process_identity(observer.identity(context["rootPid"]), context, executable)
        return {"actualExecutableObserved": True, "pid": actual["pid"],
                "creationTime100ns": actual["creationTime100ns"], "executable": actual["executable"],
                "hwnd": hwnd, "clientPhysicalSize": client, "windowsDpi": dpi}
    finally:
        if not observer.user.SetThreadDpiAwarenessContext(previous_dpi):
            raise ValueError("Receiver thread DPI awareness could not be restored")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--context", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--frame", required=True)
    parser.add_argument("--frame-sha256", required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    original_search_path = sys.path.copy()
    result = {"scope": "single-live-production-frame-binding-only", "status": "BLOCK",
              "actualExecutableObserved": False, "productionVisualAcceptance": "NOT_ASSESSED",
              "actualPresentedPixels": "NOT_ESTABLISHED", "capturePhaseCorrelation": "NOT_ESTABLISHED",
              "fullProductAcceptance": "NOT_RUN"}
    try:
        modules, publication = load_protocol_tools(args.tools)
        common, runner, release, metrics = (modules[name] for name in
            ("evidence_common", "performance_runner", "release_evidence", "windows_metrics"))
        documents = {name: read_bound_json(path, root) for name, path in
                     (("context", args.context), ("receipt", args.receipt), ("artifact", args.artifact))}
        context, receipt, artifact = (documents[name][0] for name in ("context", "receipt", "artifact"))
        if not all(isinstance(value, dict) for value in (context, receipt, artifact)):
            raise ValueError("Context, receipt and Artifact must be existing protocol objects")
        if context.get("role") != "native" or artifact.get("kind") != "native-release":
            raise ValueError("Only the existing native full-launcher artifact/driver protocol is accepted")
        if context.get("phase") not in {"ready", "state"}:
            raise ValueError("Shutdown evidence cannot substitute for a live ready/state frame")
        if artifact.get("platform") != "windows" or artifact.get("architecture") != "x64":
            raise ValueError("Live frame binding requires the actual Windows x64 Artifact")
        expected = context["expectedState"]
        runner.validate_receipt(receipt, context, expected, context["phase"], root=root)
        if any(artifact.get(key) != context[key] for key in
               ("sourceCommit", "sourceSnapshotSha256", "buildSourceSha256")):
            raise ValueError("Artifact and observed production source generation differ")
        if artifact["executable"]["sha256"] != context["executableSha256"]:
            raise ValueError("Artifact and observation refer to different EXEs")
        artifact_check = release.verify_manifest(root, artifact, expected_kind="native-release", require_current_native=True)
        if (not isinstance(artifact_check, dict) or artifact_check.get("kind") != "native-release"
                or artifact_check.get("canonicalProduct") != "Palladium"
                or artifact_check.get("eligibleForCurrentNativeRelease") is not True
                or not isinstance(artifact_check.get("buildInputs"), dict)
                or artifact_check["buildInputs"].get("buildSourceSha256") != context["buildSourceSha256"]):
            raise ValueError("Full validator did not return a bound current Palladium Artifact")
        executable = common.reference(root, artifact["executable"])
        frame = {"path": args.frame, "sha256": args.frame_sha256,
                 "bytes": common.inside(root, args.frame, must_exist=True).stat().st_size}
        if not referenced_frame(receipt, frame):
            raise ValueError("The PNG is not bound by the actual driver receipt's raw evidence")
        image = inspect_png(common.reference(root, frame), expected, expected_sha256=frame["sha256"])
        live = observe_live_window(metrics.WindowsObserver, context, receipt, executable, image)
        if not isinstance(live, dict) or live.get("actualExecutableObserved") is not True:
            raise ValueError("Live observer did not establish the received executable identity")
        check_process_identity(live, context, executable)
        if (live.get("hwnd") != receipt["uiObservation"]["windowIdentity"]["hwnd"]
                or live.get("clientPhysicalSize") != [image["width"], image["height"]]
                or type(live.get("windowsDpi")) is not int or live["windowsDpi"] * 100 != expected["dpiPercent"] * 96):
            raise ValueError("Live observer result does not bind this frame/window/DPI")
        for name, (document, row) in documents.items():
            received, received_ref = read_bound_json(root / row["path"], root)
            if (received_ref != row or json.dumps(received, sort_keys=True, allow_nan=False)
                    != json.dumps(document, sort_keys=True, allow_nan=False)):
                raise ValueError("Received JSON bytes/document changed during validation: " + name)
            common.reference(root, row)
        common.reference(root, artifact["executable"])
        common.reference(root, frame)
        publication_root = Path(publication["root"])
        verify_bound_files(publication_root, publication["files"])
        tool_files = {row["path"]: row for row in publication["files"] if row["path"].startswith("tests/native-release/")}
        publication["loadedModules"] = verify_loaded_tool_origins(args.tools.absolute(), tool_files)
        result.update({"status": "PASS_SINGLE_FRAME_BINDING", **live, "frame": frame,
                       "receivedInputs": {name: row for name, (_, row) in documents.items()},
                       "validatorPublication": publication,
                       "pixels": image, "state": expected, "sourceCommit": context["sourceCommit"],
                       "sourceSnapshotSha256": context["sourceSnapshotSha256"],
                       "buildSourceSha256": context["buildSourceSha256"],
                       "executableSha256": context["executableSha256"],
                       "buildInputs": artifact_check["buildInputs"],
                       "runId": context["runId"], "scenarioId": context["scenarioId"],
                       "requestNonce": context["requestNonce"], "phaseRequestNonce": context["phaseRequestNonce"],
                       "windowsDpiMatchesRequested": True,
                       "limitations": ["Not a pixel oracle, glyph/IME audit or full visual acceptance.",
                                       "Reuses the production runner's observation validation; no new driver or clock.",
                                       "Widget rendering, dispatch-return and DwmFlush do not prove these pixels appeared onscreen.",
                                       "Phase/page and actual Qt DPR need the producer's bound capture raw contract; a PNG reference alone is insufficient.",
                                       "Live identity and nonblack pixels do not establish matched legacy parity."]})
    except Exception as error:
        result["error"] = str(error)
    finally:
        sys.path[:] = original_search_path
    print(json.dumps(result, indent=2, ensure_ascii=True, allow_nan=False))
    return 0 if result["status"] == "PASS_SINGLE_FRAME_BINDING" else 1


if __name__ == "__main__":
    raise SystemExit(main())
