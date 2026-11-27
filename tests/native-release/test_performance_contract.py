"""Synthetic WIP-contract regressions only; no helper is a product benchmark."""
from __future__ import annotations

import copy
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import evidence_common as common
import performance_contract as contract
import performance_fixture as fixture
import performance_runner as performance


def synthetic_wgc_receipt(root: Path, context: dict, state: dict, *, prefix: str) -> dict:
    from PIL import Image

    def material(name: str, raw: bytes) -> dict:
        path = root / (prefix + "-" + name)
        common.write_new(path, raw)
        return common.file_ref(path, root)

    begin = context["phaseBeginNs"]
    window = {"hwnd": 88888, "rootPid": context["rootPid"], "rootCreationTime100ns": context["rootCreationTime100ns"]}
    raw = material("fixture-only.raw.txt", b"synthetic contract fixture only; not observed launcher presentation\n")
    input_ready = {"observed": True, "rootPid": context["rootPid"], "scenarioId": context["scenarioId"],
                   "requestNonce": context["phaseRequestNonce"], "replyRequestNonce": context["phaseRequestNonce"],
                   "queuedEventMonotonicNs": begin + 10, "queryMonotonicNs": begin + 20, "replyMonotonicNs": begin + 30,
                   "query": "canonicalRuntimeInformation", "method": "ui-queued-query-matching-reply", "rawEvidence": [raw]}
    size = tuple((value * state["dpiPercent"] + 50) // 100 for value in state["windowLogicalSize"])
    buffer = io.BytesIO()
    with Image.new("RGBA", size, (73, 144, 192, 255)) as image:
        image.save(buffer, format="PNG")
    captured = material("capture.png", buffer.getvalue())
    rendered_png = material("rendered.png", buffer.getvalue())
    state_sha = common.canonical_sha(state)
    rendered = {**context, "schemaVersion": 1, "protocol": contract.PROTOCOL,
                "origin": "actual-launcher-production-event", "sourceBindingsOrigin": contract.SOURCE_CLAIMS,
                "product": "Palladium", "executableName": "pd-launcher", "platform": "windows", "clock": "perf_counter_ns",
                "event": "renderedFrame", "sequence": 1, "monotonicNs": begin + 40, "ready": False,
                "observation": {"method": "production-client-rendered-png", "clientOnly": True, "windowIdentity": window,
                                "renderedForPhaseRequestNonce": context["phaseRequestNonce"], "stateSha256": state_sha,
                                "frameEvidence": rendered_png, "pixelSize": list(size)}}
    rendered_ref = material("rendered.json", common.json_bytes(rendered))
    stamp = ((begin + 40 + 99) // 100) * 100
    capture = {**context, "schemaVersion": 1, "protocol": contract.PROTOCOL,
               "origin": "actual-windows-graphics-capture-client-frame", "sourceBindingsOrigin": contract.SOURCE_CLAIMS,
               "platform": "windows", "clock": "perf_counter_ns", "method": contract.WGC_METHOD,
               "clientOnly": True, "windowIdentity": window, "observerRootPid": context["rootPid"] + 1,
               "observerRootCreationTime100ns": 456, "stateSha256": state_sha, "systemRelativeTime100ns": stamp // 100,
               "frameMonotonicNs": stamp, "captureBeginNs": begin + 40, "capturedMonotonicNs": stamp + 20,
               "frameEvidence": captured, "pixelSize": list(size)}
    capture_ref = material("capture.json", common.json_bytes(capture))
    marker = {"observed": True, "rootPid": context["rootPid"], "scenarioId": context["scenarioId"],
              "requestNonce": context["requestNonce"], "rawEvidence": [raw]}
    frame = {**marker, "method": contract.WGC_METHOD, "rootCreationTime100ns": context["rootCreationTime100ns"],
             "hwnd": window["hwnd"], "phase": context["phase"], "phaseRequestNonce": context["phaseRequestNonce"],
             "monotonicNs": stamp, "clientOnly": True, "frozenStateObserved": True, "stateSha256": state_sha,
             "frameEvidence": captured, "captureReceipt": capture_ref, "renderedReceipt": rendered_ref, "pixelSize": list(size)}
    roots = context.get("explicitAcceptedPathContext", {}).get("dataRoots", {key: str(root / key) for key in fixture.NATIVE_ROOTS})
    return {**context, "schemaVersion": 1, "protocol": contract.PROTOCOL, "origin": "actual-launcher-ui-observation",
            "state": state, "ready": True, "inputResponsive": True, "readyMonotonicNs": stamp + 30, "pendingImageCount": 0,
            "uiObservation": {"platform": "windows", "clock": "perf_counter_ns", "windowIdentity": window,
                              "scenarioAppliedBeforePaint": {**marker, "method": "synthetic-fixture-only", "monotonicNs": begin + 1},
                              "externalClientFramePresented": frame, "inputReady": input_ready}, "dataRoots": roots,
            "pathContextAcceptance": {"protocol": "explicit-accepted-path-context-v1", "requestNonce": context["requestNonce"],
                                      "explicitAcceptedPathContext": True, "executableGuardPassed": True,
                                      "fixturePathsGuardPassed": True, "allDataRootsWithinRunFixture": True,
                                      "systemKnownFolderAccess": "disabled", "rawEvidence": [raw]}}


class PerformanceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.local = common.inside(common.ROOT, common.LOCAL + "/selftests")
        common.check_space(cls.local, 2 * 1024 * 1024, reserve=256 * 1024 * 1024)
        cls.local.mkdir(parents=True, exist_ok=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="owned-contract-", dir=self.local)
        self.root = Path(self.temp.name).resolve()
        self.phase_dir = self.root / common.LOCAL / "phase"
        self.phase_dir.mkdir(parents=True)
        self.state = {"route": "home", "windowLogicalSize": [16, 12], "dpiPercent": 100, "fontProfile": "same-host",
                      "themeId": "mono", "mode": "dark", "backgroundSha256": None, "blur": 0,
                      "reducedMotion": False, "fixtureId": "synthetic-contract-only", "pendingImageCount": 0, "activeTaskCount": 0}
        self.context = {"schemaVersion": 1, "protocol": contract.PROTOCOL, "runId": "owned_run", "scenarioId": "home-no-background",
                        "requestNonce": "owned_ready", "phaseRequestNonce": "owned_ready", "phase": "ready", "role": "native",
                        "observationAdapter": contract.NATIVE_ADAPTER, "sourceCommit": "3" * 40, "sourceSnapshotSha256": "4" * 64,
                        "buildSourceSha256": "6" * 64, "driverSourceSha256": "5" * 64, "executableSha256": "1" * 64,
                        "inputCorpusSha256": "2" * 64, "rootPid": 123456, "rootCreationTime100ns": 123,
                        "fixtureRoot": str(self.root), "launchBeginNs": 100, "phaseBeginNs": 100, "launchCreateReturnedNs": 105,
                        "launchContext": str(self.phase_dir / "launch-context.json"), "productionEvents": str(self.phase_dir / "events.jsonl"),
                        "armedReceipt": str(self.phase_dir / "armed.json"), "expectedState": self.state}

    def tearDown(self):
        self.temp.cleanup()

    def material(self, name: str, raw: bytes) -> dict:
        path = self.root / name
        common.write_new(path, raw)
        return common.file_ref(path, self.root)

    def publish(self, context: dict, name: str) -> dict:
        path = self.phase_dir / name
        common.write_json(path, context)
        with patch.object(contract.time, "perf_counter_ns", return_value=1000):
            return contract.publish_phase_request(path, root=self.root)

    def test_phase_immutability_replay_roots_and_terminal_shutdown(self):
        first = self.publish(self.context, "ready.context.json")
        initial = common.reference(self.root, first).read_bytes()
        state = {**self.context, "phase": "state", "phaseRequestNonce": "owned_state", "phaseBeginNs": 200}
        self.publish(state, "state.context.json")
        self.assertEqual(common.reference(self.root, first).read_bytes(), initial)
        self.assertEqual(common.read_json(Path(self.context["launchContext"])), state)
        for index, altered in enumerate(({**state, "phaseBeginNs": 300},
                                        {**state, "phaseRequestNonce": "root_redirect", "phaseBeginNs": 300, "fixtureRoot": str(self.root.parent)},
                                        {**state, "phaseRequestNonce": "source_redirect", "phaseBeginNs": 300, "buildSourceSha256": "0" * 64})):
            with self.subTest(altered=altered), self.assertRaises(common.EvidenceError):
                self.publish(altered, f"bad-{index}.context.json")
        shutdown = {**self.context, "phase": "shutdown", "phaseRequestNonce": "owned_shutdown", "phaseBeginNs": 400}
        self.publish(shutdown, "shutdown.context.json")
        with self.assertRaises(common.EvidenceError):
            self.publish({**state, "phaseRequestNonce": "after_shutdown", "phaseBeginNs": 500}, "after.context.json")

    def test_phase_fails_without_first_ready_and_rejects_live_tampering(self):
        with self.assertRaises(common.EvidenceError):
            self.publish({**self.context, "phase": "state", "phaseRequestNonce": "not_ready", "phaseBeginNs": 200}, "wrong-first.json")
        self.publish(self.context, "ready.json")
        live = Path(self.context["launchContext"])
        live.write_bytes(common.json_bytes({**self.context, "buildSourceSha256": "0" * 64}))
        with self.assertRaises(common.EvidenceError):
            self.publish({**self.context, "phase": "state", "phaseRequestNonce": "fresh_state", "phaseBeginNs": 200}, "state.json")

    def test_native_wgc_decodes_separate_rgba_files_and_fresh_reply(self):
        receipt = synthetic_wgc_receipt(self.root, self.context, self.state, prefix="ready")
        with patch.object(performance.time, "perf_counter_ns", return_value=1000):
            performance.validate_receipt(receipt, self.context, self.state, "ready", root=self.root)
        frame = receipt["uiObservation"]["externalClientFramePresented"]
        with self.assertRaises(common.EvidenceError):
            contract.validate_native_wgc_frame(frame, receipt["uiObservation"]["windowIdentity"], self.context, self.state,
                                               "ready", self.root, 1000, input_ready={**receipt["uiObservation"]["inputReady"], "replyRequestNonce": "stale"})
        fresh = {**self.context, "phase": "state", "phaseRequestNonce": "fresh_state", "phaseBeginNs": 300}
        fresh_receipt = synthetic_wgc_receipt(self.root, fresh, self.state, prefix="state")
        with patch.object(performance.time, "perf_counter_ns", return_value=1000):
            performance.validate_receipt(fresh_receipt, fresh, self.state, "state", root=self.root)
            with self.assertRaises(common.EvidenceError):
                performance.validate_receipt({**receipt, **fresh}, fresh, self.state, "state", root=self.root)

    def test_native_wgc_rejects_proxy_black_hash_pixel_clock_and_identity_drift(self):
        from PIL import Image

        receipt = synthetic_wgc_receipt(self.root, self.context, self.state, prefix="drift")
        frame = receipt["uiObservation"]["externalClientFramePresented"]
        window = receipt["uiObservation"]["windowIdentity"]
        input_ready = receipt["uiObservation"]["inputReady"]
        capture = common.read_json(common.reference(self.root, frame["captureReceipt"]))
        for index, change in enumerate(({"clock": "steady_clock"}, {"rootCreationTime100ns": 456},
                                        {"phaseRequestNonce": "old_phase"}, {"stateSha256": "0" * 64},
                                        {"frameMonotonicNs": 999}, {"observerRootPid": self.context["rootPid"]},
                                        {"windowIdentity": {**window, "hwnd": 111}})):
            row = self.material(f"capture-drift-{index}.json", common.json_bytes({**capture, **change}))
            with self.subTest(change=change), self.assertRaises(common.EvidenceError):
                contract.validate_native_wgc_frame({**frame, "captureReceipt": row}, window, self.context, self.state,
                                                   "ready", self.root, 1000, input_ready=input_ready)
        for index, color in enumerate(((0, 0, 0, 255), (12, 34, 56, 0), (74, 144, 192, 255))):
            output = io.BytesIO()
            with Image.new("RGBA", (16, 12), color) as image:
                image.save(output, format="PNG")
            png = self.material(f"bad-pixels-{index}.png", output.getvalue())
            row = self.material(f"bad-pixels-{index}.json", common.json_bytes({**capture, "frameEvidence": png}))
            with self.subTest(color=color), self.assertRaises(common.EvidenceError):
                contract.validate_native_wgc_frame({**frame, "frameEvidence": png, "captureReceipt": row}, window,
                                                   self.context, self.state, "ready", self.root, 1000, input_ready=input_ready)
        wrong_hash = {**frame["frameEvidence"], "sha256": "0" * 64}
        with self.assertRaises(common.EvidenceError):
            contract._png_pixels(self.root, wrong_hash)
        with patch.object(performance.time, "perf_counter_ns", return_value=1000):
            for name in ("paintComplete", "flushComplete", "presentDwmConfirmed"):
                with self.subTest(proxy=name), self.assertRaises(common.EvidenceError):
                    performance.validate_receipt({**receipt, "uiObservation": {**receipt["uiObservation"], name: {"observed": True}}},
                                                 self.context, self.state, "ready", root=self.root)
            with self.assertRaises(common.MissingEvidence):
                performance.validate_receipt({**receipt, "uiObservation": {"platform": "windows", "clock": "perf_counter_ns",
                                                                         "paintComplete": True, "flushComplete": True,
                                                                         "presentDwmConfirmed": {"method": "DwmFlush", "hresult": 0}}},
                                             self.context, self.state, "ready", root=self.root)

    def test_png_input_and_pixel_bounds_and_missing_decoder_are_fail_closed(self):
        receipt = synthetic_wgc_receipt(self.root, self.context, self.state, prefix="bounds")
        png = receipt["uiObservation"]["externalClientFramePresented"]["frameEvidence"]
        with patch.object(contract, "MAX_PNG_BYTES", 8), self.assertRaises(common.EvidenceError):
            contract._png_pixels(self.root, png)
        with patch.object(contract, "MAX_PNG_PIXELS", 8), self.assertRaises(common.EvidenceError):
            contract._png_pixels(self.root, png)
        with patch.dict("sys.modules", {"PIL": None}), self.assertRaises(common.MissingEvidence):
            contract._png_pixels(self.root, png)
        for index, raw in enumerate((b"not a png", common.reference(self.root, png).read_bytes()[:-20])):
            row = self.material(f"invalid-{index}.png", raw)
            with self.assertRaises(common.EvidenceError):
                contract._png_pixels(self.root, row)

    def test_png_envelope_rejects_crc_trailing_animation_and_chunk_drift(self):
        import struct
        import zlib
        from PIL import ImageFile

        receipt = synthetic_wgc_receipt(self.root, self.context, self.state, prefix="envelope")
        png = receipt["uiObservation"]["externalClientFramePresented"]["frameEvidence"]
        raw = common.reference(self.root, png).read_bytes()

        def chunk(kind: bytes, data: bytes) -> bytes:
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

        invalid = {
            "iend-crc": raw[:-1] + bytes([raw[-1] ^ 1]),
            "trailing": raw + b"unconsumed trailing bytes",
            "no-iend": raw[:-12],
            "chunk-extent": raw[:8] + struct.pack(">I", 0xffffffff) + raw[12:],
            "duplicate-header": raw[:33] + raw[8:33] + raw[33:],
            "animated": raw[:33] + chunk(b"acTL", struct.pack(">II", 1, 0)) + raw[33:],
            "nonempty-iend": raw[:-12] + chunk(b"IEND", b"unexpected"),
        }
        for name, data in invalid.items():
            row = self.material("envelope-" + name + ".png", data)
            with self.subTest(case=name), self.assertRaises(common.EvidenceError):
                contract._png_pixels(self.root, row)
        with patch.object(ImageFile, "LOAD_TRUNCATED_IMAGES", True), self.assertRaises(common.EvidenceError):
            contract._png_pixels(self.root, png)


    def test_event_jsonl_checks_sequence_phase_claims_clock_and_duplicate_fields(self):
        receipt = synthetic_wgc_receipt(self.root, self.context, self.state, prefix="event")
        frame = receipt["uiObservation"]["externalClientFramePresented"]
        event = common.read_json(common.reference(self.root, frame["renderedReceipt"]))
        raw = common.json_bytes(event).replace(b"\n", b" ") + b"\n"
        row = self.material("valid-events.jsonl", raw)
        with patch.object(contract.time, "perf_counter_ns", return_value=1000):
            self.assertEqual(contract.read_production_events(self.root, row, {self.context["phaseRequestNonce"]: self.context}), [event])
            bad_records = [common.json_bytes({**event, "sequence": 2}), common.json_bytes({**event, "ready": True}),
                           common.json_bytes({**event, "sourceBindingsOrigin": "compiled-source-attestation"}),
                           common.json_bytes({**event, "phaseRequestNonce": "missing_request"}),
                           common.json_bytes({**event, "monotonicNs": 99}), common.json_bytes({**event, "clock": "steady_clock"}),
                           b'{"sequence":1,"sequence":2}\n']
            for index, bad in enumerate(bad_records):
                row = self.material(f"bad-events-{index}.jsonl", bad.replace(b"\n", b" ") + b"\n")
                with self.subTest(index=index), self.assertRaises(common.EvidenceError):
                    contract.read_production_events(self.root, row, {self.context["phaseRequestNonce"]: self.context})

    def test_runtime_fixture_copies_exact_exe_uncompressed_and_warmup_identity(self):
        executable = self.material("inputs/pd-launcher.exe", b"synthetic non-executable fixture only\n")
        manifest = {"kind": "native-release", "product": "Palladium", "sourceCommit": "3" * 40,
                    "sourceSnapshotSha256": "4" * 64, "buildSourceSha256": "6" * 64, "executable": executable,
                    "files": [{**executable, "role": "application", "distributionPath": "pd-launcher.exe"}]}
        target = self.root / "owned-runtime"
        target.mkdir()
        common.write_json(target / fixture.NATIVE_ROOTS["config"] / "settings.json", {})
        result = fixture.prepare_runtime_fixture(self.root, manifest, target, {}, max_bytes=1024)
        self.assertEqual(result["status"], "prepared-not-runtime-accepted")
        self.assertFalse(result["nativeReleaseReady"])
        deployed = Path(result["executablePath"])
        self.assertFalse(deployed.samefile(common.reference(self.root, executable)))
        self.assertEqual(deployed.read_bytes(), common.reference(self.root, executable).read_bytes())
        self.assertEqual(fixture.prepare_runtime_fixture(self.root, manifest, target, {}, max_bytes=1024), result)
        with self.assertRaises(common.EvidenceError):
            fixture.prepare_runtime_fixture(self.root, {**manifest, "sourceCommit": "0" * 40}, target, {}, max_bytes=1024)
        deployed.write_bytes(b"changed deployment")
        with self.assertRaises(common.EvidenceError):
            fixture.prepare_runtime_fixture(self.root, manifest, target, {}, max_bytes=1024)

    def test_runtime_fixture_requires_seed_budget_and_nonoverlapping_roots(self):
        executable = self.material("input.exe", b"synthetic input")
        manifest = {"kind": "native-release", "executable": executable,
                    "files": [{**executable, "role": "application", "distributionPath": "pd-launcher.exe"}]}
        target = self.root / "missing-seed"
        target.mkdir()
        with self.assertRaises(common.MissingEvidence):
            fixture.prepare_runtime_fixture(self.root, manifest, target, {}, max_bytes=1024)
        self.assertFalse((target / "pd-launcher.exe").exists())
        with self.assertRaises(common.EvidenceError):
            fixture.prepare_runtime_fixture(self.root, manifest, target, {}, max_bytes=1)
        for roots in ({**fixture.NATIVE_ROOTS, "config": "config"}, {**fixture.NATIVE_ROOTS, "game": "../outside"},
                      {**fixture.NATIVE_ROOTS, "game": fixture.NATIVE_ROOTS["cache"]}):
            with self.subTest(roots=roots), self.assertRaises(common.EvidenceError):
                fixture.fixture_root_names({"fixtureDataRoots": roots}, native=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
