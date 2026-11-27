"""Raw events and mutable phase channel for the existing evidence-driver v1 protocol."""
from __future__ import annotations

import hashlib
import io

import json
import os
from pathlib import Path
import re
import time
import warnings

from evidence_common import (ROOT, LOCAL, EvidenceError, MissingEvidence, _unique_object, canonical_sha,
                             check_space, file_ref, inside, read_json, reference, write_new)

PROTOCOL = "nickel-evidence-driver-v1"
NATIVE_ADAPTER = "native-production-markers-v1"
WGC_METHOD = "external-WGC-presented"
SOURCE_CLAIMS = "runner-context-claims-not-build-attestation"
MAX_EVENT_BYTES = 64 * 1024
MAX_EVENT_LOG_BYTES = 2 * 1024 * 1024
MAX_PNG_BYTES = 16 * 1024 * 1024
MAX_PNG_PIXELS = 16 * 1024 * 1024
IDENTITY_FIELDS = ("runId", "scenarioId", "requestNonce", "role", "observationAdapter", "sourceCommit",
                   "sourceSnapshotSha256", "buildSourceSha256", "executableSha256", "driverSourceSha256",
                   "inputCorpusSha256", "rootPid", "rootCreationTime100ns")
PRODUCTION_EVENTS = frozenset({"scenarioAppliedBeforePaint", "paintComplete", "flushComplete",
                               "presentDwmConfirmed", "inputReady", "pathContextAcceptance",
                               "shutdownObserved", "frameObservation", "renderedFrame", "startupReady"})


def _same_identity(document: dict, context: dict) -> None:
    for field in IDENTITY_FIELDS + ("phaseRequestNonce", "phase"):
        if field not in context or document.get(field) != context[field]:
            raise EvidenceError(f"Raw observation {field} belongs to a different launch or phase")
    for field in ("rootPid", "rootCreationTime100ns"):
        if type(document.get(field)) is not int or document[field] < 1:
            raise EvidenceError("Raw observation needs an actual kernel process identity")
    for field in ("sourceSnapshotSha256", "buildSourceSha256", "executableSha256", "driverSourceSha256", "inputCorpusSha256"):
        if not isinstance(document.get(field), str) or not re.fullmatch(r"[0-9a-f]{64}", document[field]):
            raise EvidenceError("Raw native observation needs exact committed/build/executable/input hashes")
    if not isinstance(document.get("sourceCommit"), str) or not re.fullmatch(r"[0-9a-f]{40}(?:[0-9a-f]{24})?", document["sourceCommit"]):
        raise EvidenceError("Raw native observation needs an exact source commit")


def validate_production_event(document: dict, context: dict, *, now: int | None = None) -> None:
    if (not isinstance(document, dict) or type(document.get("schemaVersion")) is not int or document["schemaVersion"] != 1 or
            document.get("protocol") != PROTOCOL or document.get("origin") != "actual-launcher-production-event" or
            document.get("sourceBindingsOrigin") != SOURCE_CLAIMS):
        raise EvidenceError("Production events use the existing protocol and disclose context claims, not build attestation")
    _same_identity(document, context)
    if document.get("role") != "native" or document.get("observationAdapter") != NATIVE_ADAPTER:
        raise EvidenceError("Frozen legacy never supplies native production events")
    if document.get("product") != "Palladium" or document.get("executableName") != "pd-launcher":
        raise EvidenceError("Production event does not identify the current Palladium executable")
    if document.get("platform") != "windows" or document.get("clock") != "perf_counter_ns":
        raise EvidenceError("Production observation must use Windows QPC/perf_counter_ns, not a second clock")
    if (type(document.get("sequence")) is not int or document["sequence"] < 1 or document.get("event") not in PRODUCTION_EVENTS or
            document.get("phase") not in {"ready", "state", "shutdown"}):
        raise EvidenceError("Production event needs a known phase/event and positive finite sequence")
    stamp, begin = document.get("monotonicNs"), context.get("phaseBeginNs")
    now = time.perf_counter_ns() if now is None else now
    if type(stamp) is not int or type(begin) is not int or not 0 < begin <= stamp <= now:
        raise EvidenceError("Production event is stale, future or on a different clock")
    if not isinstance(document.get("observation"), dict):
        raise EvidenceError("Production event requires its actual observation object")
    if "ready" in document and document["ready"] is not False:
        raise EvidenceError("Raw producer events are not final ready receipts")


def read_production_events(root: Path, row: dict, requests: dict[str, dict]) -> list[dict]:
    path = reference(root, row)
    if path.stat().st_size > MAX_EVENT_LOG_BYTES:
        raise EvidenceError("Production event log exceeds its 2 MiB bound")
    raw = path.read_bytes()
    if not raw.endswith(b"\n"):
        raise EvidenceError("Only a complete immutable JSONL snapshot can be verified")
    events, previous = [], 0
    for line in raw.splitlines():
        if not line or len(line) > MAX_EVENT_BYTES:
            raise EvidenceError("Empty or oversized production event record")
        try:
            event = json.loads(line.decode("utf-8"), object_pairs_hook=_unique_object,
                               parse_constant=lambda value: (_ for _ in ()).throw(EvidenceError("Nonfinite event: " + value)))
        except (UnicodeError, ValueError) as error:
            raise EvidenceError("Invalid production JSONL record") from error
        if not isinstance(event, dict) or event.get("phaseRequestNonce") not in requests:
            raise EvidenceError("Production log names an unrecorded phase request")
        validate_production_event(event, requests[event["phaseRequestNonce"]])
        if event["sequence"] != len(events) + 1 or event["monotonicNs"] < previous:
            raise EvidenceError("Duplicate/gapped sequence or backwards production clock")
        previous = event["monotonicNs"]
        events.append(event)
    return events


def publish_phase_request(request_path: Path, *, root: Path = ROOT) -> dict:
    def owned(path: Path, *, must_exist: bool = False) -> Path:
        try:
            name = path.relative_to(root.resolve()).as_posix()
        except ValueError as error:
            raise EvidenceError("Phase material must remain in the owned evidence root") from error
        if not name.startswith(LOCAL + "/"):
            raise EvidenceError("Phase material must use the private local evidence directory")
        return inside(root, name, must_exist=must_exist)

    request_path = owned(request_path, must_exist=True)
    if request_path.stat().st_size > MAX_EVENT_BYTES:
        raise EvidenceError("Phase context exceeds its 64 KiB bound")
    context = read_json(request_path)
    if (not isinstance(context, dict) or type(context.get("schemaVersion")) is not int or context["schemaVersion"] != 1 or
            context.get("protocol") != PROTOCOL or context.get("role") != "native" or context.get("observationAdapter") != NATIVE_ADAPTER):
        raise EvidenceError("Phase publication uses only the existing native driver context")
    phase, nonce = context.get("phase"), context.get("phaseRequestNonce")
    if phase not in {"ready", "state", "shutdown"}:
        raise EvidenceError("Finite ready/state/shutdown phase required")
    for field in ("runId", "scenarioId", "requestNonce", "phaseRequestNonce"):
        if not isinstance(context.get(field), str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", context[field]):
            raise EvidenceError("Phase requests need bounded run/scenario identifiers and nonces")
    _same_identity(context, context)
    begin, launch, returned = (context.get(field) for field in ("phaseBeginNs", "launchBeginNs", "launchCreateReturnedNs"))
    if (any(type(value) is not int for value in (begin, launch, returned)) or
            not 0 < launch <= begin <= time.perf_counter_ns() or not launch <= returned <= time.perf_counter_ns()):
        raise EvidenceError("Phase channel uses the actual launch/shared QPC clock")
    live_value = context.get("launchContext")
    if not isinstance(live_value, str) or not Path(live_value).is_absolute():
        raise EvidenceError("Existing launchContext must identify an absolute owned channel")
    live = owned(Path(live_value))
    if live.parent != request_path.parent or live == request_path:
        raise EvidenceError("Live channel and immutable request must share the same private parent")
    for field in ("productionEvents", "armedReceipt"):
        value = context.get(field)
        if not isinstance(value, str) or not Path(value).is_absolute():
            raise EvidenceError("Producer events/armed receipt need fixed absolute owned paths")
        path = owned(Path(value))
        if path.parent != live.parent or path in {live, request_path}:
            raise EvidenceError("Phase channel cannot redirect or overwrite producer/armed material")
    history = owned(live.parent / "phase-requests.raw.jsonl")
    records = []
    if history.exists():
        if history.stat().st_size > MAX_EVENT_BYTES:
            raise EvidenceError("Finite phase history exceeded its bound")
        raw_history = history.read_bytes()
        if not raw_history or not raw_history.endswith(b"\n"):
            raise EvidenceError("Phase history must consist of complete immutable JSONL records")
        try:
            records = [json.loads(line, object_pairs_hook=_unique_object) for line in raw_history.splitlines()]
        except (ValueError, UnicodeError) as error:
            raise EvidenceError("Invalid phase history") from error
        for record in records:
            if not isinstance(record, dict) or set(record) != {"phase", "phaseRequestNonce", "phaseBeginNs", "request"}:
                raise EvidenceError("Malformed phase history record")
            prior = read_json(reference(root, record["request"]))
            if any(prior.get(field) != record[field] for field in ("phase", "phaseRequestNonce", "phaseBeginNs")):
                raise EvidenceError("Phase history differs from its immutable request")
    if len(records) >= 32 or nonce in {record["phaseRequestNonce"] for record in records}:
        raise EvidenceError("Duplicate/replayed or excessive phase publication")
    mutable = {"phase", "phaseRequestNonce", "phaseBeginNs"}
    if live.exists():
        previous = read_json(live)
        if not records or canonical_sha(previous) != canonical_sha(read_json(reference(root, records[-1]["request"]))):
            raise EvidenceError("Live channel must equal the last recorded immutable publication")
        if canonical_sha({key: value for key, value in context.items() if key not in mutable}) != canonical_sha(
                {key: value for key, value in previous.items() if key not in mutable}):
            raise EvidenceError("Only phase/phaseRequestNonce/phaseBeginNs may change after spawn")
        if previous.get("phase") == "shutdown" or phase == "ready" or begin <= previous.get("phaseBeginNs", 0):
            raise EvidenceError("Shutdown is terminal and later requests must be strictly fresh")
        if nonce == context["requestNonce"]:
            raise EvidenceError("Later phase cannot replay the original ready nonce")
    elif phase != "ready" or records or nonce != context["requestNonce"] or begin != launch:
        raise EvidenceError("Actual post-spawn ready context with the original nonce must be published first")
    stage = owned(live.parent / ("phase-" + nonce + ".tmp"))
    write_new(stage, request_path.read_bytes())
    record = {"phase": phase, "phaseRequestNonce": nonce, "phaseBeginNs": begin, "request": file_ref(request_path, root)}
    raw = (json.dumps(record, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode("ascii")
    check_space(history, len(raw))
    with history.open("ab" if records else "xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    # The runner alone replaces the live channel; all original requests remain byte-verifiable.
    os.replace(stage, live)
    return record["request"]


def _png_envelope(raw: bytes) -> tuple[int, int]:
    import struct
    import zlib

    if not raw.startswith(b"\x89PNG\r\n\x1a\n") or len(raw) > MAX_PNG_BYTES:
        raise EvidenceError("Actual frame must be a bounded PNG, not a renamed image")
    cursor, size, idat, ended = 8, None, False, False
    while cursor < len(raw):
        if len(raw) - cursor < 12:
            raise EvidenceError("Truncated PNG chunk framing")
        length = struct.unpack_from(">I", raw, cursor)[0]
        kind = raw[cursor + 4:cursor + 8]
        end = cursor + 12 + length
        if end > len(raw) or not all(65 <= value <= 90 or 97 <= value <= 122 for value in kind):
            raise EvidenceError("Invalid bounded PNG chunk extent or name")
        payload = raw[cursor + 8:end - 4]
        crc = struct.unpack_from(">I", raw, end - 4)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != crc:
            raise EvidenceError("PNG chunk CRC mismatch, including IEND")
        if size is None:
            if kind != b"IHDR" or length != 13:
                raise EvidenceError("PNG must begin with exactly one complete IHDR")
            width, height = struct.unpack_from(">II", payload)
            if not (0 < width <= 16384 and 0 < height <= 16384 and width * height <= MAX_PNG_PIXELS):
                raise EvidenceError("PNG physical surface exceeds bounded dimensions or decoded pixels")
            size = (width, height)
        elif kind == b"IHDR":
            raise EvidenceError("Duplicate PNG header")
        if kind in {b"acTL", b"fcTL", b"fdAT"}:
            raise EvidenceError("An animation is not a single compositor frame")
        if kind == b"IDAT":
            if ended:
                raise EvidenceError("PNG IDAT chunks must be consecutive")
            idat = True
        elif idat:
            ended = True
        if kind == b"IEND":
            if length != 0 or not idat or end != len(raw):
                raise EvidenceError("PNG must end at a complete zero-length IEND; no trailing bytes")
            return size
        cursor = end
    raise EvidenceError("PNG lacks a complete terminal IEND")


def _png_pixels(root: Path, row: dict) -> tuple[tuple[int, int], bytes]:
    if isinstance(row, dict) and type(row.get("bytes")) is int and row["bytes"] > MAX_PNG_BYTES:
        raise EvidenceError("WGC/rendered PNG exceeds its 16 MiB byte bound")
    path = reference(root, row)
    if path.stat().st_size > MAX_PNG_BYTES:
        raise EvidenceError("WGC/rendered PNG exceeds its 16 MiB byte bound")
    with path.open("rb") as stream:
        raw = stream.read(MAX_PNG_BYTES + 1)
    if len(raw) > MAX_PNG_BYTES or hashlib.sha256(raw).hexdigest() != row["sha256"]:
        raise EvidenceError("PNG changed after reference verification or exceeded its byte bound")
    size = _png_envelope(raw)
    try:
        from PIL import Image, ImageFile
    except ImportError as error:
        raise MissingEvidence("Verified PNG pixel decoding requires Pillow in the observer Python environment") from error
    if ImageFile.LOAD_TRUNCATED_IMAGES:
        raise EvidenceError("Observer PNG decoding must never allow truncated images")
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(raw), formats=["PNG"]) as image:
                if image.size != size or getattr(image, "n_frames", 1) != 1:
                    raise EvidenceError("A bounded single actual PNG frame is required")
                image.verify()
            # Reopen the same verified bytes; verify() alone does not decode or check terminal IEND.
            with Image.open(io.BytesIO(raw), formats=["PNG"]) as image:
                with image.convert("RGBA") as rgba:
                    extrema = rgba.getextrema()
                    if extrema[3][1] == 0 or all(extrema[channel][1] == 0 for channel in range(3)):
                        raise EvidenceError("Black/transparent frame cannot establish a compositor-presented launcher")
                    return size, rgba.tobytes()
    except (OSError, ValueError, SyntaxError, EOFError, Image.DecompressionBombError, Image.DecompressionBombWarning) as error:
        raise EvidenceError("Invalid or oversized WGC/rendered PNG") from error


def validate_native_wgc_frame(frame: dict, window: dict, context: dict, state: dict, phase: str,
                              root: Path, now: int, *, input_ready: dict) -> int:
    if (not isinstance(frame, dict) or frame.get("method") != WGC_METHOD or frame.get("observed") is not True or
            frame.get("clientOnly") is not True or frame.get("frozenStateObserved") is not True or
            type(frame.get("hwnd")) is not int or frame["hwnd"] != window["hwnd"] or
            frame.get("rootPid") != context["rootPid"] or frame.get("rootCreationTime100ns") != context["rootCreationTime100ns"] or
            frame.get("phaseRequestNonce") != context["phaseRequestNonce"] or frame.get("phase") != phase or phase not in {"ready", "state"}):
        raise EvidenceError("Native WGC frame must identify the exact client surface, kernel birth and fresh phase")
    receipts = []
    for field in ("captureReceipt", "renderedReceipt"):
        path = reference(root, frame.get(field))
        if path.stat().st_size > MAX_EVENT_BYTES:
            raise EvidenceError("Raw capture/render receipt exceeds its 64 KiB record bound")
        document = read_json(path)
        if not isinstance(document, dict):
            raise EvidenceError("Raw capture/render receipt must be a JSON object")
        receipts.append(document)
    capture, rendered = receipts
    if (type(capture.get("schemaVersion")) is not int or capture["schemaVersion"] != 1 or capture.get("protocol") != PROTOCOL or
            capture.get("origin") != "actual-windows-graphics-capture-client-frame" or capture.get("sourceBindingsOrigin") != SOURCE_CLAIMS):
        raise EvidenceError("WGC needs an actual raw capture receipt, not a title/Qt/DwmFlush proxy")
    _same_identity(capture, context)
    if capture.get("platform") != "windows" or capture.get("clock") != "perf_counter_ns" or capture.get("method") != WGC_METHOD:
        raise EvidenceError("WGC capture clock must be the compositor QPC/perf_counter_ns clock")
    if capture.get("clientOnly") is not True or capture.get("windowIdentity") != window:
        raise EvidenceError("Raw WGC capture belongs to a different HWND/kernel surface")
    for key in ("observerRootPid", "observerRootCreationTime100ns"):
        if type(capture.get(key)) is not int or capture[key] < 1:
            raise EvidenceError("External observer needs its own actual kernel identity")
    if capture["observerRootPid"] == context["rootPid"]:
        raise EvidenceError("Observer is external to the launcher tree; launcher descendants are never subtracted")
    validate_production_event(rendered, context, now=now)
    rendered_frame = rendered["observation"]
    if (rendered.get("event") != "renderedFrame" or rendered_frame.get("method") != "production-client-rendered-png" or
            rendered_frame.get("clientOnly") is not True or rendered_frame.get("windowIdentity") != window or
            rendered_frame.get("renderedForPhaseRequestNonce") != context["phaseRequestNonce"]):
        raise EvidenceError("Production rendered PNG must identify this window and current phase nonce")
    state_sha = canonical_sha(state)
    if any(value != state_sha for value in (frame.get("stateSha256"), capture.get("stateSha256"), rendered_frame.get("stateSha256"))):
        raise EvidenceError("Captured/rendered pixels are not correlated to the fresh frozen state")
    reply = input_ready.get("replyMonotonicNs")
    if (type(reply) is not int or not context["phaseBeginNs"] <= reply <= rendered["monotonicNs"] or
            input_ready.get("replyRequestNonce") != context["phaseRequestNonce"]):
        raise EvidenceError("Production PNG must follow this phase's fresh queued runtime-information reply")
    relative, stamp = capture.get("systemRelativeTime100ns"), capture.get("frameMonotonicNs")
    begin, completed = capture.get("captureBeginNs"), capture.get("capturedMonotonicNs")
    if (any(type(value) is not int for value in (relative, stamp, begin, completed)) or relative <= 0 or stamp != relative * 100 or
            frame.get("monotonicNs") != stamp or not context["phaseBeginNs"] <= begin <= stamp <= completed <= now or
            not rendered["monotonicNs"] <= stamp):
        raise EvidenceError("WGC frame is stale or uses a mismatched compositor/QPC clock")
    if capture.get("frameEvidence") != frame.get("frameEvidence"):
        raise EvidenceError("Final WGC marker must reference the exact raw capture PNG")
    if capture["frameEvidence"].get("path") == rendered_frame.get("frameEvidence", {}).get("path"):
        raise EvidenceError("External capture and production render must retain their separate raw PNG files")
    captured_size, captured_pixels = _png_pixels(root, capture["frameEvidence"])
    rendered_size, rendered_pixels = _png_pixels(root, rendered_frame.get("frameEvidence"))
    expected = tuple((dimension * state["dpiPercent"] + 50) // 100 for dimension in state["windowLogicalSize"])
    if (captured_size != rendered_size or captured_size != expected or frame.get("pixelSize") != list(captured_size) or
            capture.get("pixelSize") != list(captured_size) or rendered_frame.get("pixelSize") != list(captured_size)):
        raise EvidenceError("Actual WGC/rendered physical surface size differs from the frozen logical size and DPI")
    if captured_pixels != rendered_pixels:
        raise EvidenceError("Actual WGC compositor pixels do not match the production rendered PNG")
    return completed
