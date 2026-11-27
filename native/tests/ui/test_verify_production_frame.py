"""Receiver helper tests use synthetic pixels and never emit production PASS receipts."""
import ctypes as C
from ctypes import wintypes as W
import hashlib
from io import StringIO
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

import verify_production_frame as receiver
from verify_production_frame import (MAX_FRAME_BYTES, MAX_FRAME_PIXELS, check_process_identity,
                                     inspect_png, observe_live_window, referenced_frame)


class ProductionFrameReceiverTests(unittest.TestCase):
    def setUp(self):
        directory = Path(__file__).resolve().parent / ".build" / "tmp"
        directory.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="frame-receiver-test-", dir=directory)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.state = {"windowLogicalSize": [4, 4], "dpiPercent": 100}

    def image(self, color, varied=False, size=(4, 4)):
        target = self.root / "synthetic-not-production.png"
        with Image.new("RGBA", size, color) as image:
            if varied:
                image.putpixel((1, 1), (120, 180, 220, 255))
            image.save(target)
        return target

    def test_nonblack_nonuniform_pixels_are_decoded(self):
        result = inspect_png(self.image((10, 20, 30, 255), True), self.state)
        self.assertEqual((result["width"], result["height"]), (4, 4))
        self.assertEqual(result["decodedPlaneBytes"], 64)

    def test_black_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Black"):
            inspect_png(self.image((0, 0, 0, 255)), self.state)

    def test_transparent_colored_pixels_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "transparent"):
            inspect_png(self.image((150, 180, 220, 0)), self.state)

    def test_uniform_nonblack_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "uniform"):
            inspect_png(self.image((10, 20, 30, 255)), self.state)

    def test_dpi_geometry_mismatch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "logical size/DPI"):
            inspect_png(self.image((10, 20, 30, 255), True), {**self.state, "dpiPercent": 125})

    def test_fractional_dpi_geometry_is_supported(self):
        result = inspect_png(self.image((10, 20, 30, 255), True, (5, 5)),
                             {**self.state, "dpiPercent": 125})
        self.assertEqual(result["width"], 5)

    def test_renamed_bytes_are_rejected(self):
        path = self.root / "false.png"
        path.write_bytes(b"not an image")
        with self.assertRaisesRegex(ValueError, "PNG"):
            inspect_png(path, self.state)

    def test_frame_requires_raw_evidence_binding(self):
        frame = {"path": "frame.png", "bytes": 10, "sha256": "a" * 64}
        receipt = {"uiObservation": {"paintComplete": {"rawEvidence": [frame]}}}
        self.assertTrue(referenced_frame(receipt, frame))
        self.assertFalse(referenced_frame(receipt, {**frame, "sha256": "b" * 64}))
        self.assertFalse(referenced_frame({"unrelatedFrame": frame}, frame))

    def test_frame_references_do_not_accept_extra_fields(self):
        with self.assertRaisesRegex(ValueError, "exact"):
            referenced_frame({}, {"path": "frame.png", "bytes": 10, "sha256": "a" * 64, "ready": True})

    def test_kernel_birth_and_executable_must_match(self):
        path = self.root / "synthetic-not-production.exe"
        context = {"rootPid": 123, "rootCreationTime100ns": 456}
        actual = {"pid": 123, "creationTime100ns": 456, "executable": str(path)}
        check_process_identity(actual, context, path)
        for mismatch in ({**actual, "pid": 124}, {**actual, "creationTime100ns": 457},
                         {**actual, "executable": str(self.root / "other.exe")},
                         {**actual, "executable": "synthetic-not-production.exe"},
                         {**actual, "executable": ""}, {**actual, "executable": None},
                         {**actual, "pid": 123.0}, {**actual, "creationTime100ns": 456.0}):
            with self.subTest(mismatch=mismatch), self.assertRaisesRegex(ValueError, "same executable/birth"):
                check_process_identity(mismatch, context, path)

    def test_decoded_bytes_must_match_recorded_hash(self):
        path = self.image((10, 20, 30, 255), True)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        inspect_png(path, self.state, expected_sha256=digest)
        with self.assertRaisesRegex(ValueError, "recorded frame hash"):
            inspect_png(path, self.state, expected_sha256="0" * 64)

    def test_crc_corruption_is_rejected_before_decode(self):
        path = self.image((10, 20, 30, 255), True)
        raw = bytearray(path.read_bytes())
        chunk = raw.index(b"IDAT")
        length = int.from_bytes(raw[chunk - 4:chunk], "big")
        raw[chunk + 4 + length] ^= 1
        path.write_bytes(raw)
        with self.assertRaisesRegex(SyntaxError, "checksum"):
            inspect_png(path, self.state)

    def test_truncated_png_is_rejected(self):
        path = self.image((10, 20, 30, 255), True)
        path.write_bytes(path.read_bytes()[:-15])
        with self.assertRaises((OSError, SyntaxError, ValueError)):
            inspect_png(path, self.state)

    def test_input_byte_budget_is_checked_before_decode(self):
        path = self.root / "oversize-not-production.png"
        with path.open("wb") as stream:
            stream.seek(MAX_FRAME_BYTES)
            stream.write(b"x")
        with patch("verify_production_frame.Image.open") as decode:
            with self.assertRaisesRegex(ValueError, "bounded evidence input size"):
                inspect_png(path, self.state)
            decode.assert_not_called()

    def test_pixel_budget_is_checked_before_decode(self):
        path = self.image((10, 20, 30, 255), True)
        raw = bytearray(path.read_bytes())
        raw[16:20] = (MAX_FRAME_PIXELS + 1).to_bytes(4, "big")
        path.write_bytes(raw)
        with patch("verify_production_frame.Image.open") as decode:
            with self.assertRaisesRegex(ValueError, "pixel budget"):
                inspect_png(path, self.state)
            decode.assert_not_called()

    def test_zero_dimensions_are_rejected_before_decode(self):
        path = self.image((10, 20, 30, 255), True)
        raw = bytearray(path.read_bytes())
        raw[20:24] = (0).to_bytes(4, "big")
        path.write_bytes(raw)
        with self.assertRaisesRegex(ValueError, "pixel budget"):
            inspect_png(path, self.state)

    def test_palette_png_is_not_accepted_as_rgba_capture(self):
        path = self.root / "palette-not-production.png"
        with Image.new("P", (4, 4)) as image:
            image.save(path)
        with self.assertRaisesRegex(ValueError, "RGB/RGBA"):
            inspect_png(path, self.state)

    def test_animation_is_not_a_single_frame_capture(self):
        path = self.root / "animated-not-production.png"
        with Image.new("RGBA", (4, 4), (10, 20, 30, 255)) as first:
            with Image.new("RGBA", (4, 4), (120, 180, 220, 255)) as second:
                first.save(path, save_all=True, append_images=[second], duration=20, loop=0)
        with self.assertRaisesRegex(ValueError, "single PNG"):
            inspect_png(path, self.state)

    def test_only_existing_observation_marker_references_bind_pixels(self):
        frame = {"path": "frame.png", "bytes": 10, "sha256": "a" * 64}
        for receipt in (None, [], {"uiObservation": None},
                        {"uiObservation": {"unrelated": {"rawEvidence": [frame]}}},
                        {"rawEvidence": [frame]}, {"uiObservation": {"paintComplete": None}},
                        {"uiObservation": {"paintComplete": {"rawEvidence": frame}}}):
            with self.subTest(receipt=receipt):
                self.assertFalse(referenced_frame(receipt, frame))


class SyntheticApi:
    def __init__(self, implementation):
        self.implementation = implementation
        self.calls = []

    def __call__(self, *arguments):
        self.calls.append(arguments)
        return self.implementation(*arguments)


class LiveWindowReceiverTests(unittest.TestCase):
    """Synthetic Win32 adapters test helper logic, not actual production observation."""

    def setUp(self):
        self.executable = Path(__file__).resolve().parent / "synthetic-not-production.exe"
        self.context = {"rootPid": 123, "rootCreationTime100ns": 456,
                        "expectedState": {"dpiPercent": 125}}
        self.receipt = {"uiObservation": {"windowIdentity": {"hwnd": 789}}}
        self.image = {"width": 1375, "height": 900}
        self.identity = {"pid": 123, "creationTime100ns": 456, "executable": str(self.executable)}
        self.identity_calls = 0
        self.owner = 123
        self.client = (1375, 900)
        self.dpi = 120
        self.user = SimpleNamespace(
            GetWindowThreadProcessId=SyntheticApi(self.window_owner),
            IsWindowVisible=SyntheticApi(lambda hwnd: 1),
            IsIconic=SyntheticApi(lambda hwnd: 0),
            GetClientRect=SyntheticApi(self.client_rect),
            GetDpiForWindow=SyntheticApi(lambda hwnd: self.dpi),
            SetThreadDpiAwarenessContext=SyntheticApi(lambda context: 111))

    def window_owner(self, hwnd, owner):
        C.cast(owner, C.POINTER(W.DWORD)).contents.value = self.owner
        return 222

    def client_rect(self, hwnd, rect):
        value = C.cast(rect, C.POINTER(W.RECT)).contents
        value.left, value.top = 0, 0
        value.right, value.bottom = self.client
        return 1

    def observed_identity(self, pid):
        self.identity_calls += 1
        return self.identity

    def observer_type(self, pid, *, root_creation):
        self.assertEqual((pid, root_creation), (123, 456))
        return SimpleNamespace(user=self.user, identity=self.observed_identity)

    def observe(self):
        return observe_live_window(self.observer_type, self.context, self.receipt,
                                   self.executable, self.image)

    def assert_restored(self):
        calls = self.user.SetThreadDpiAwarenessContext.calls
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[1], (111,))

    def test_matching_physical_dpi_and_geometry_restore_thread_context(self):
        result = self.observe()
        self.assertEqual(result["windowsDpi"], 120)
        self.assertEqual(result["clientPhysicalSize"], [1375, 900])
        self.assertEqual(self.identity_calls, 2)
        self.assert_restored()

    def test_qt_scale_alone_does_not_establish_windows_125_percent(self):
        self.dpi = 96
        with self.assertRaisesRegex(ValueError, "Qt scale alone"):
            self.observe()
        self.assert_restored()

    def test_unavailable_windows_dpi_fails_closed(self):
        self.dpi = 0
        with self.assertRaisesRegex(ValueError, "Actual Windows DPI"):
            self.observe()
        self.assert_restored()

    def test_wrong_owner_fails_closed_and_restores_context(self):
        self.owner = 124
        with self.assertRaisesRegex(ValueError, "owned window"):
            self.observe()
        self.assert_restored()

    def test_hidden_window_fails_closed_and_restores_context(self):
        self.user.IsWindowVisible.implementation = lambda hwnd: 0
        with self.assertRaisesRegex(ValueError, "visible nonminimized"):
            self.observe()
        self.assert_restored()

    def test_minimized_window_fails_closed_and_restores_context(self):
        self.user.IsIconic.implementation = lambda hwnd: 1
        with self.assertRaisesRegex(ValueError, "visible nonminimized"):
            self.observe()
        self.assert_restored()

    def test_geometry_drift_fails_closed_and_restores_context(self):
        self.client = (1100, 720)
        with self.assertRaisesRegex(ValueError, "client geometry"):
            self.observe()
        self.assert_restored()

    def test_process_birth_is_rechecked_after_window_queries(self):
        calls = []

        def identity(pid):
            calls.append(pid)
            return self.identity if len(calls) == 1 else {**self.identity, "creationTime100ns": 457}

        self.observed_identity = identity
        with self.assertRaisesRegex(ValueError, "same executable/birth"):
            self.observe()
        self.assertEqual(calls, [123, 123])
        self.assert_restored()

    def test_unavailable_dpi_awareness_fails_before_geometry_access(self):
        self.user.SetThreadDpiAwarenessContext.implementation = lambda context: 0
        with self.assertRaisesRegex(ValueError, "per-monitor DPI awareness"):
            self.observe()
        self.assertEqual(self.user.GetClientRect.calls, [])

    def test_context_restore_failure_cannot_return_success(self):
        self.user.SetThreadDpiAwarenessContext.implementation = (
            lambda context: 111 if len(self.user.SetThreadDpiAwarenessContext.calls) == 1 else 0)
        with self.assertRaisesRegex(ValueError, "could not be restored"):
            self.observe()

    def test_oversize_hwnd_cannot_be_truncated_by_ctypes(self):
        self.receipt["uiObservation"]["windowIdentity"]["hwnd"] = 1 << (8 * C.sizeof(C.c_void_p))
        with self.assertRaisesRegex(ValueError, "pointer-sized"):
            self.observe()
        self.assertEqual(self.user.SetThreadDpiAwarenessContext.calls, [])


class ReceiverCommandTests(unittest.TestCase):
    """Mocked command rejection paths never manufacture a production PASS receipt."""

    def setUp(self):
        directory = Path(__file__).resolve().parent / ".build" / "tmp"
        directory.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="receiver-command-test-", dir=directory)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.executable = self.root / "synthetic-not-production.exe"
        self.executable.write_bytes(b"synthetic, not a Windows executable")
        self.frame = self.root / "synthetic-not-production.png"
        with Image.new("RGBA", (4, 4), (10, 20, 30, 255)) as image:
            image.putpixel((1, 1), (120, 180, 220, 255))
            image.save(self.frame)
        self.context = {"role": "native", "phase": "ready", "expectedState": {"windowLogicalSize": [4, 4], "dpiPercent": 100},
                        "sourceCommit": "a" * 40, "sourceSnapshotSha256": "b" * 64,
                        "buildSourceSha256": "c" * 64, "executableSha256": self.reference(self.executable)["sha256"],
                        "rootPid": 123, "rootCreationTime100ns": 456, "runId": "synthetic-run", "scenarioId": "synthetic-state",
                        "requestNonce": "synthetic-launch", "phaseRequestNonce": "synthetic-phase"}
        self.artifact = {"kind": "native-release", "platform": "windows", "architecture": "x64",
                         "sourceCommit": "a" * 40, "sourceSnapshotSha256": "b" * 64,
                         "buildSourceSha256": "c" * 64, "executable": self.reference(self.executable)}
        self.receipt = {"uiObservation": {"windowIdentity": {"hwnd": 789},
                        "paintComplete": {"rawEvidence": [self.reference(self.frame)]}}}
        self.common = SimpleNamespace(reference=self.check_reference,
                                      inside=lambda root, name, **options: root / name)
        self.runner = SimpleNamespace(validate_receipt=unittest.mock.Mock())
        self.release = SimpleNamespace(verify_manifest=unittest.mock.Mock(
            side_effect=ValueError("existing full Artifact validator rejected")))
        self.modules = {"evidence_common": self.common, "performance_runner": self.runner,
                        "release_evidence": self.release, "windows_metrics": SimpleNamespace(WindowsObserver=object)}
        self.publication = {"root": str(self.root), "files": []}
        self.live = {"actualExecutableObserved": True, "pid": 123, "creationTime100ns": 456,
                     "executable": str(self.executable), "hwnd": 789, "clientPhysicalSize": [4, 4], "windowsDpi": 96}
        self.observer = unittest.mock.Mock(return_value=self.live)
        self.arguments = ["verify_production_frame.py", "--tools", str(self.root),
                          "--root", str(self.root), "--context", "context.json",
                          "--receipt", "receipt.json", "--artifact", "artifact.json",
                          "--frame", self.frame.name, "--frame-sha256", self.reference(self.frame)["sha256"]]

    def reference(self, path):
        raw = path.read_bytes()
        return {"path": path.relative_to(self.root).as_posix(), "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}

    def check_reference(self, root, row):
        target = root / row["path"]
        if self.reference(target) != {key: row[key] for key in ("path", "sha256", "bytes")}:
            raise ValueError("Synthetic test input hash drift")
        return target

    def allow_late_rejection(self):
        self.release.verify_manifest.side_effect = None
        self.release.verify_manifest.return_value = {
            "kind": "native-release", "canonicalProduct": "Palladium", "eligibleForCurrentNativeRelease": True,
            "buildInputs": {"buildSourceSha256": self.context["buildSourceSha256"]}}

    def run_rejection(self):
        for name, document in (("context", self.context), ("receipt", self.receipt), ("artifact", self.artifact)):
            (self.root / (name + ".json")).write_text(json.dumps(document), encoding="utf-8")
        original_search_path = receiver.sys.path.copy()
        with patch.object(receiver.sys, "argv", self.arguments), \
                patch.object(receiver, "load_protocol_tools", return_value=(self.modules, self.publication)), \
                patch.object(receiver, "observe_live_window", self.observer), \
                patch.object(receiver.sys, "stdout", StringIO()) as output:
            code = receiver.main()
        document = json.loads(output.getvalue())
        self.assertEqual(code, 1)
        self.assertEqual(document["status"], "BLOCK")
        self.assertFalse(document["actualExecutableObserved"])
        self.assertEqual(document["productionVisualAcceptance"], "NOT_ASSESSED")
        self.assertEqual(document["actualPresentedPixels"], "NOT_ESTABLISHED")
        self.assertEqual(receiver.sys.path, original_search_path)
        return document

    def test_component_artifact_is_not_native_release(self):
        self.artifact["kind"] = "component-fixture"
        document = self.run_rejection()
        self.assertIn("full-launcher", document["error"])
        self.runner.validate_receipt.assert_not_called()

    def test_legacy_observation_is_not_native_production(self):
        self.context["role"] = "legacy"
        self.run_rejection()
        self.runner.validate_receipt.assert_not_called()

    def test_existing_receipt_validation_failure_is_not_bypassed(self):
        self.runner.validate_receipt.side_effect = ValueError("stale phase nonce")
        document = self.run_rejection()
        self.assertEqual(document["error"], "stale phase nonce")
        self.release.verify_manifest.assert_not_called()

    def test_each_source_generation_identity_must_match(self):
        for name in ("sourceCommit", "sourceSnapshotSha256", "buildSourceSha256"):
            with self.subTest(name=name):
                original = self.artifact[name]
                self.artifact[name] = "0" * len(original)
                document = self.run_rejection()
                self.assertIn("source generation", document["error"])
                self.artifact[name] = original
        self.release.verify_manifest.assert_not_called()

    def test_different_executable_is_rejected_before_manifest_validation(self):
        self.artifact["executable"]["sha256"] = "f" * 64
        document = self.run_rejection()
        self.assertIn("different EXEs", document["error"])
        self.release.verify_manifest.assert_not_called()

    def test_complete_artifact_validator_is_required_and_rejection_survives(self):
        document = self.run_rejection()
        self.assertIn("full Artifact validator rejected", document["error"])
        self.release.verify_manifest.assert_called_once_with(
            self.root, self.artifact, expected_kind="native-release", require_current_native=True)

    def test_shutdown_short_circuit_cannot_bypass_ui_observation_validation(self):
        self.context["phase"] = "shutdown"
        document = self.run_rejection()
        self.assertIn("live ready/state", document["error"])
        self.runner.validate_receipt.assert_not_called()

    def test_non_windows_or_non_x64_artifact_cannot_bind_windows_hwnd(self):
        for platform, architecture in (("linux", "x64"), ("windows", "arm64"), (None, "x64")):
            with self.subTest(platform=platform, architecture=architecture):
                self.artifact.update(platform=platform, architecture=architecture)
                document = self.run_rejection()
                self.assertIn("Windows x64 Artifact", document["error"])
                self.runner.validate_receipt.assert_not_called()

    def test_nonobject_evidence_fails_closed(self):
        self.receipt = []
        document = self.run_rejection()
        self.assertIn("protocol objects", document["error"])
        self.runner.validate_receipt.assert_not_called()

    def test_successful_callback_without_current_bound_artifact_cannot_pass(self):
        self.allow_late_rejection()
        for returned in (None, {}, {"kind": "native-release", "canonicalProduct": "Nickel"},
                         {"kind": "native-release", "canonicalProduct": "Palladium", "eligibleForCurrentNativeRelease": True,
                          "buildInputs": {"buildSourceSha256": "0" * 64}}):
            with self.subTest(returned=returned):
                self.release.verify_manifest.return_value = returned
                self.assertIn("bound current Palladium", self.run_rejection()["error"])
        self.observer.assert_not_called()

    def test_wrong_live_observer_identity_cannot_pass(self):
        self.allow_late_rejection()
        self.live["creationTime100ns"] = 457
        self.assertIn("same executable/birth", self.run_rejection()["error"])

    def test_live_callback_without_actual_identity_cannot_pass(self):
        self.allow_late_rejection()
        self.live["actualExecutableObserved"] = False
        self.assertIn("received executable identity", self.run_rejection()["error"])

    def test_live_result_geometry_and_dpi_must_match(self):
        self.allow_late_rejection()
        for name, changed in (("hwnd", 790), ("clientPhysicalSize", [5, 4]), ("windowsDpi", 120), ("windowsDpi", 96.0)):
            with self.subTest(name=name, changed=changed):
                original = self.live[name]
                self.live[name] = changed
                self.assertIn("frame/window/DPI", self.run_rejection()["error"])
                self.live[name] = original

    def test_input_bytes_changed_after_validator_cannot_pass(self):
        self.allow_late_rejection()
        for name in ("context", "receipt", "artifact"):
            with self.subTest(name=name):
                self.runner.validate_receipt.side_effect = lambda *args, **options: (
                    self.root / (name + ".json")).write_text("{}", encoding="utf-8")
                self.assertIn("Received JSON bytes/document changed", self.run_rejection()["error"])

    def test_callback_document_mutation_cannot_replace_received_bytes(self):
        self.allow_late_rejection()
        self.runner.validate_receipt.side_effect = lambda receipt, *args, **options: receipt.update(fakeAfterRead=True)
        self.assertIn("Received JSON bytes/document changed", self.run_rejection()["error"])

    def test_frame_changed_during_live_check_cannot_pass(self):
        self.allow_late_rejection()
        self.observer.side_effect = lambda *args: (self.frame.write_bytes(b"changed after decode"), self.live)[1]
        self.assertIn("hash drift", self.run_rejection()["error"])

    def test_executable_changed_during_live_check_cannot_pass(self):
        self.allow_late_rejection()
        self.observer.side_effect = lambda *args: (self.executable.write_bytes(b"changed after identity"), self.live)[1]
        self.assertIn("hash drift", self.run_rejection()["error"])

    def test_tool_bytes_changed_during_validation_cannot_pass(self):
        self.allow_late_rejection()
        tool = self.root / "synthetic-tool.py"
        tool.write_bytes(b"synthetic = True\n")
        self.publication["files"] = [self.reference(tool)]
        self.observer.side_effect = lambda *args: (tool.write_bytes(b"synthetic = False\n"), self.live)[1]
        self.assertIn("Received input bytes changed", self.run_rejection()["error"])


class BoundInputAndToolOriginTests(unittest.TestCase):
    """Exercise actual file reads and module-origin guards without production receipts."""

    def setUp(self):
        directory = Path(__file__).resolve().parent / ".build" / "tmp"
        directory.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="receiver-binding-test-", dir=directory)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()

    def test_json_reference_records_the_same_bytes_that_are_parsed(self):
        raw = b'\xef\xbb\xbf{ "state" : "synthetic" }\r\n'
        path = self.root / "input.json"
        path.write_bytes(raw)
        document, row = receiver.read_bound_json(Path(path.name), self.root)
        self.assertEqual(document, {"state": "synthetic"})
        self.assertEqual(row, {"path": path.name, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)})
        receiver.verify_bound_files(self.root, [row])

    def test_bound_input_replacement_is_rejected(self):
        path = self.root / "input.json"
        path.write_text('{"v":1}', encoding="utf-8")
        _, row = receiver.read_bound_json(path, self.root)
        path.write_text('{"v":2}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "input bytes changed"):
            receiver.verify_bound_files(self.root, [row])

    def test_duplicate_and_nonfinite_json_are_rejected(self):
        path = self.root / "input.json"
        for raw in ('{"v":1,"v":2}', '{"v":NaN}', '{"v":Infinity}', '{"v":-Infinity}'):
            with self.subTest(raw=raw):
                path.write_text(raw, encoding="utf-8")
                with self.assertRaises(ValueError):
                    receiver.read_bound_json(path, self.root)

    def test_oversized_json_is_rejected_before_parsing(self):
        path = self.root / "oversize.json"
        path.write_bytes(b"x" * 33)
        with patch.object(receiver, "MAX_JSON_BYTES", 32):
            with self.assertRaisesRegex(ValueError, "bounded input size"):
                receiver.read_bound_json(path, self.root)

    def test_outside_or_traversing_json_input_is_rejected(self):
        for path in (self.root.parent / "outside.json", Path("../outside.json"), Path("sub/../input.json")):
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "root"):
                receiver.read_bound_json(path, self.root)

    def test_valid_module_origin_is_bound_to_the_selected_tree(self):
        tools = self.root / "tests" / "native-release"
        path = tools / "evidence_common.py"
        files = {"tests/native-release/evidence_common.py": {"sha256": "a" * 64, "bytes": 1}}
        module = SimpleNamespace(__file__=str(path), __spec__=SimpleNamespace(origin=str(path)))
        with patch.dict(receiver.sys.modules, evidence_common=module):
            result = receiver.verify_loaded_tool_origins(tools, files)
        self.assertEqual(result[0]["module"], "evidence_common")

    def test_cached_validator_outside_selected_tree_is_rejected(self):
        tools = self.root / "tests" / "native-release"
        path = self.root / "foreign-evidence_common.py"
        files = {"tests/native-release/evidence_common.py": {"sha256": "a" * 64, "bytes": 1}}
        module = SimpleNamespace(__file__=str(path), __spec__=SimpleNamespace(origin=str(path)))
        with patch.dict(receiver.sys.modules, evidence_common=module):
            with self.assertRaisesRegex(ValueError, "outside the selected publication"):
                receiver.verify_loaded_tool_origins(tools, files)

    def test_missing_or_different_module_spec_origin_is_rejected(self):
        tools = self.root / "tests" / "native-release"
        path = tools / "evidence_common.py"
        files = {"tests/native-release/evidence_common.py": {"sha256": "a" * 64, "bytes": 1}}
        for module in (SimpleNamespace(__file__=str(path)),
                       SimpleNamespace(__file__=str(path), __spec__=SimpleNamespace(origin=str(self.root / "foreign.py")))):
            with self.subTest(module=module), patch.dict(receiver.sys.modules, evidence_common=module):
                with self.assertRaisesRegex(ValueError, "outside the selected publication"):
                    receiver.verify_loaded_tool_origins(tools, files)

    def synthetic_publication(self):
        tools = self.root / "tests" / "native-release"
        tools.mkdir(parents=True)
        files, modules = {}, {}
        for name in ("evidence_common", "performance_runner", "release_evidence", "windows_metrics"):
            path = tools / (name + ".py")
            raw = b"# Synthetic origin guard fixture, not production validator\n"
            path.write_bytes(raw)
            files[path.relative_to(self.root).as_posix()] = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
            modules[name] = SimpleNamespace(__file__=str(path), __spec__=SimpleNamespace(origin=str(path)))
        snapshot = {"sourceRoot": str(self.root), "sourceCommit": "a" * 40, "files": files}
        raw = json.dumps(snapshot).encode("ascii")
        (self.root / "snapshot.json").write_bytes(raw)
        publication = {"publicationComplete": True, "sourceRoot": str(self.root), "sourceCommit": "a" * 40,
                       "snapshot": str(self.root / "snapshot.json"), "snapshotFileSha256": hashlib.sha256(raw).hexdigest(),
                       "receiverManifestSha256": hashlib.sha256(raw).hexdigest(), "sourceFiles": len(files),
                       "sourceBytes": sum(row["bytes"] for row in files.values())}
        (self.root / "publication-complete.json").write_text(json.dumps(publication), encoding="ascii")
        return tools, modules, publication

    def test_publication_binds_snapshot_and_loaded_sources(self):
        tools, modules, _ = self.synthetic_publication()
        with patch.object(receiver.sys, "path", receiver.sys.path.copy()), patch.dict(receiver.sys.modules, modules), \
                patch.object(receiver.importlib, "import_module", side_effect=modules.__getitem__):
            loaded, binding = receiver.load_protocol_tools(tools)
        self.assertEqual(set(loaded), set(modules))
        self.assertEqual(len(binding["loadedModules"]), 4)
        receiver.verify_bound_files(self.root, binding["files"])

    def test_incomplete_or_rehashed_snapshot_publication_is_rejected(self):
        tools, modules, publication = self.synthetic_publication()
        for name, value in (("publicationComplete", False), ("snapshotFileSha256", "0" * 64),
                            ("receiverManifestSha256", "0" * 64), ("sourceFiles", 0), ("sourceBytes", 1)):
            with self.subTest(name=name):
                (self.root / "publication-complete.json").write_text(json.dumps({**publication, name: value}), encoding="ascii")
                with patch.dict(receiver.sys.modules, modules), self.assertRaises(ValueError):
                    receiver.load_protocol_tools(tools)

    def test_active_publisher_lock_is_rejected(self):
        tools, _, _ = self.synthetic_publication()
        (self.root / ".publisher.lock").write_text("synthetic", encoding="ascii")
        with self.assertRaisesRegex(ValueError, "completed"):
            receiver.load_protocol_tools(tools)

    def test_import_time_tool_byte_drift_is_rejected(self):
        tools, modules, _ = self.synthetic_publication()

        def changed_import(name):
            (tools / "performance_runner.py").write_text("changed = True\n", encoding="ascii")
            return modules[name]

        with patch.object(receiver.sys, "path", receiver.sys.path.copy()), patch.dict(receiver.sys.modules, modules), \
                patch.object(receiver.importlib, "import_module", side_effect=changed_import):
            with self.assertRaisesRegex(ValueError, "input bytes changed"):
                receiver.load_protocol_tools(tools)



if __name__ == "__main__":
    unittest.main()
