"""Equivalent full Release benchmarking; dry-run first, never guess savings or use probes."""
from __future__ import annotations

import argparse
from collections import defaultdict
import json
import math
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import time
import uuid

from bounded_process import BoundedProcess
from evidence_common import (ROOT, LOCAL, EvidenceError, MissingEvidence, SingleWorker, canonical_sha,
                             check_space, copy_small_input, exit_code, file_ref, gate, inside,
                             new_output, overall, read_json, reference, relative_name, utc_now,
                             write_json, write_new)
from event_trace import EventTrace, trace_descendants
from performance_contract import WGC_METHOD, publish_phase_request, validate_native_wgc_frame
from performance_fixture import fixture_root_names, prepare_runtime_fixture, runtime_layout
from release_evidence import bind_native_generation, canonical_product, verify_manifest
from windows_metrics import GpuObserver, WindowsObserver, machine_metadata

THEME_IDS = {"mono", "emerald", "blue", "violet", "amber", "rose", "nickel", "copper", "abyss", "paper", "fortyTwo", "custom"}
COMMON_SCENARIOS = {"home-no-background", "home-4k-blur", "long-list", "task-running-cancelled", "game-return"}
REQUIRED_STATE = {"route", "windowLogicalSize", "dpiPercent", "fontProfile", "themeId", "mode", "backgroundSha256",
                  "blur", "reducedMotion", "fixtureId", "pendingImageCount", "activeTaskCount"}
GROUPS = {"cold-app-cache", "hot-app-cache"}
MAX_RUN_RAW_BYTES = 16 * 1024 * 1024


def stats(values: list[float | int]) -> dict | None:
    if not values:
        return None
    ordered = sorted(values)
    return {"count": len(values), "median": statistics.median(values),
            "p95NearestRank": ordered[max(0, math.ceil(len(values) * 0.95) - 1)], "max": ordered[-1], "min": ordered[0]}


def expand_argv(template: list[str], values: dict[str, str]) -> list[str]:
    result = []
    for item in template:
        if not isinstance(item, str) or "\x00" in item:
            raise EvidenceError("argv must contain NUL-free strings")
        for token, replacement in values.items():
            item = item.replace("{" + token + "}", replacement)
        if "{" in item or "}" in item:
            raise EvidenceError("Unknown driver/launch argv placeholder")
        result.append(item)
    return result


def validate_state(state: dict) -> None:
    if not isinstance(state, dict) or REQUIRED_STATE - set(state):
        raise MissingEvidence("Complete frozen functional/visual state is required")
    size = state["windowLogicalSize"]
    if not isinstance(size, list) or len(size) != 2 or any(type(value) is not int or value < 1 for value in size):
        raise EvidenceError("Window size must contain two positive logical integer dimensions")
    for key in ("route", "fontProfile", "fixtureId"):
        if not isinstance(state[key], str) or not state[key]:
            raise EvidenceError(f"Frozen state {key} must be nonempty")
    if state["themeId"] not in THEME_IDS or state["mode"] not in {"light", "dark"}:
        raise EvidenceError("Frozen state must identify an actual theme and light/dark mode")
    if type(state["dpiPercent"]) is not int or not 50 <= state["dpiPercent"] <= 800:
        raise EvidenceError("Frozen DPI must be an explicit bounded integer percentage")
    if type(state["reducedMotion"]) is not bool or type(state["pendingImageCount"]) is not int or state["pendingImageCount"] != 0:
        raise EvidenceError("Motion policy and fully loaded image state must be explicit")
    if type(state["activeTaskCount"]) is not int or state["activeTaskCount"] < 0:
        raise EvidenceError("Frozen task count must be a nonnegative integer")
    if type(state["blur"]) not in (int, float) or not math.isfinite(state["blur"]) or state["blur"] < 0:
        raise EvidenceError("Frozen blur must be a finite nonnegative number")
    background = state["backgroundSha256"]
    if background is not None and (not isinstance(background, str) or len(background) != 64 or
                                   any(value not in "0123456789abcdef" for value in background)):
        raise EvidenceError("Background must identify actual SHA-256 bytes or be explicitly absent")


def launch_configuration_sha(launcher: dict) -> str:
    arguments, environment = launcher.get("launchArgs"), launcher.get("environment", {})
    if not isinstance(arguments, list) or not isinstance(environment, dict):
        raise EvidenceError("Exact launch arguments and environment mapping are required")
    values = {"fixture": "owned-fixture-root", "context": "owned-driver-context"}
    expand_argv(arguments, values)
    for name, value in environment.items():
        if not isinstance(name, str) or not name or "=" in name or "\x00" in name or name.upper() in {"PATH", "TEMP", "TMP"}:
            raise EvidenceError("Launcher environment cannot hide dependencies or replace owned temporary roots")
        expand_argv([value], values)
    driver = launcher.get("driver", {})
    if not isinstance(driver, dict) or not isinstance(driver.get("source"), dict):
        raise MissingEvidence("Hash-identified real observation driver is required for safe launch configuration")
    roots = launcher.get("fixtureDataRoots")
    if roots is not None:
        if not isinstance(roots, dict) or set(roots) != {"config", "game", "cache", "temp"}:
            raise EvidenceError("Exact relative fixture roots are required")
        roots = {key: relative_name(value) for key, value in roots.items()}
    fields = {"launchArgs": arguments, "environment": environment, "driverSourceSha256": driver["source"].get("sha256")}
    if roots is not None:
        fields["fixtureDataRoots"] = roots
    return canonical_sha(fields)


def validate_receipt(receipt: dict, context: dict, state: dict, phase: str, *, root: Path = ROOT) -> None:
    validate_state(state)
    if (not isinstance(receipt, dict) or type(receipt.get("schemaVersion")) is not int or receipt["schemaVersion"] != 1 or
            receipt.get("protocol") != "nickel-evidence-driver-v1" or receipt.get("origin") != "actual-launcher-ui-observation"):
        raise EvidenceError("Receipt must follow the finite actual-launcher driver protocol")
    role = context.get("role")
    adapter = "native-production-markers-v1" if role == "native" else "legacy-external-ui-observer-v1" if role == "legacy" else None
    if adapter is None or receipt.get("observationAdapter") != adapter or context.get("observationAdapter") != adapter:
        raise EvidenceError("A frozen legacy UI is externally observed, never required to implement native internal markers")
    for field in ("runId", "scenarioId", "requestNonce", "phaseRequestNonce", "role", "executableSha256", "sourceCommit",
                  "driverSourceSha256", "rootPid", "rootCreationTime100ns", "inputCorpusSha256"):
        if field not in context or receipt.get(field) != context[field]:
            raise EvidenceError(f"Driver receipt {field} identifies a stale/different launch or phase")
    if type(context["rootPid"]) is not int or context["rootPid"] < 1 or type(context["rootCreationTime100ns"]) is not int or context["rootCreationTime100ns"] < 1:
        raise EvidenceError("Driver context lacks actual kernel process identity")
    for field in ("sourceSnapshotSha256", "buildSourceSha256"):
        digest = context.get(field)
        if role == "native" and (not isinstance(digest, str) or len(digest) != 64 or any(value not in "0123456789abcdef" for value in digest)):
            raise MissingEvidence("Native observation context requires committed and actual build-source SHA-256 bindings")
        if digest is not None and receipt.get(field) != digest:
            raise EvidenceError(f"Driver receipt {field} does not identify the same committed/actual source")
    now, phase_begin, launch_begin = time.perf_counter_ns(), context.get("phaseBeginNs"), context.get("launchBeginNs")
    if (phase not in {"ready", "state", "shutdown"} or type(phase_begin) is not int or type(launch_begin) is not int or
            not 0 < launch_begin <= phase_begin <= now or receipt.get("phase") != phase):
        raise EvidenceError("Driver receipt phase or actual request time mismatch")
    if phase == "shutdown":
        stamp = receipt.get("shutdownMonotonicNs")
        if receipt.get("shutdownObserved") is not True or not isinstance(receipt.get("rawEvidence"), list) or not receipt["rawEvidence"]:
            raise MissingEvidence("Actual normal UI shutdown observation is missing")
        if type(stamp) is not int or not phase_begin <= stamp <= now:
            raise EvidenceError("Normal shutdown receipt predates this phase request")
        for row in receipt["rawEvidence"]:
            reference(root, row)
        return
    if receipt.get("ready") is not True or receipt.get("inputResponsive") is not True:
        raise EvidenceError("Visible responsive first frame/state is not actually verified")
    if receipt.get("state") != state or receipt.get("pendingImageCount") != 0:
        raise EvidenceError("Observed functional/visual/image state differs from the frozen scenario")
    observation = receipt.get("uiObservation")
    if not isinstance(observation, dict) or observation.get("platform") != "windows" or observation.get("clock") != "perf_counter_ns":
        raise MissingEvidence("Windows observations need the shared QPC/perf_counter_ns clock; no generic first-frame inference")
    if role == "native" and "externalClientFramePresented" not in observation:
        raise MissingEvidence("Native actual presentation requires independent client WGC; rendered hashes, Qt dispatch and GPU/DwmFlush are not presentation")
    if role == "native" and any(observation.get(key) is not None for key in ("paintComplete", "flushComplete", "presentDwmConfirmed")):
        raise EvidenceError("Native WGC compositor path cannot promote Qt/DwmFlush proxies to actual markers")
    if role == "legacy" and any(key in observation for key in ("paintComplete", "flushComplete")):
        raise EvidenceError("Frozen legacy cannot claim native internal paint/flush markers")
    marker_names = ("scenarioAppliedBeforePaint", "externalClientFramePresented") if role == "native" else (
                   "scenarioAppliedBeforePaint", "presentDwmConfirmed", "externalClientFramePresented")
    stamps = []
    for name in marker_names:
        event = observation.get(name)
        if not isinstance(event, dict) or event.get("observed") is not True:
            raise MissingEvidence(f"Actual {name}, not startupReady/paint-request, must be observed")
        for field in ("rootPid", "scenarioId", "requestNonce"):
            if event.get(field) != context[field]:
                raise EvidenceError(f"Actual {name} belongs to a different launch/scenario/nonce")
        stamp = event.get("monotonicNs")
        lower = phase_begin if role == "native" else launch_begin
        if type(stamp) is not int or not lower <= stamp <= now:
            raise EvidenceError(f"Actual {name} timestamp is stale, future or on a different clock")
        if not isinstance(event.get("method"), str) or not event["method"] or not isinstance(event.get("rawEvidence"), list) or not event["rawEvidence"]:
            raise MissingEvidence(f"Actual {name} has no method/raw evidence")
        for item in event["rawEvidence"]:
            reference(root, item)
        stamps.append(stamp)
    if stamps != sorted(stamps):
        raise EvidenceError("Scenario must apply before actual Windows presentation")
    if role == "legacy":
        present = observation["presentDwmConfirmed"]
        if present.get("method") != "DwmFlush" or type(present.get("hresult")) is not int or present["hresult"] != 0:
            raise MissingEvidence("Legacy DwmFlush is supplementary only; an actual independent WGC frame is still required")
    window = observation.get("windowIdentity")
    if (not isinstance(window, dict) or type(window.get("hwnd")) is not int or window["hwnd"] < 1 or
            window.get("rootPid") != context["rootPid"] or window.get("rootCreationTime100ns") != context["rootCreationTime100ns"]):
        raise MissingEvidence("Actual HWND ownership must match the launcher kernel birth identity")
    if role == "legacy":
        frame = observation["externalClientFramePresented"]
        if (frame.get("method") != "windows-graphics-capture-client-frame" or frame.get("hwnd") != window["hwnd"] or
                frame.get("clientOnly") is not True or frame.get("frozenStateObserved") is not True):
            raise MissingEvidence("Legacy needs actual composited client pixels, not window/title discovery or native markers")
        reference(root, frame.get("frameEvidence"))
    event = observation.get("inputReady")
    if not isinstance(event, dict) or event.get("observed") is not True or event.get("rootPid") != context["rootPid"] or event.get("scenarioId") != context["scenarioId"]:
        raise MissingEvidence("Input needs an actual nonce-correlated application UI response")
    native_input = event.get("query") == "canonicalRuntimeInformation" and event.get("method") == "ui-queued-query-matching-reply"
    legacy_input = (event.get("query") == "observed-legacy-runtime-state" and event.get("method") == "uia-reversible-action-observed-response" and
                    event.get("normalization") == "external-driver-only" and event.get("productNonceSupported") is False and
                    event.get("responseObserved") is True and event.get("frozenStateRestored") is True)
    if not (native_input if role == "native" else legacy_input):
        raise EvidenceError("Native uses production queued query/reply; legacy needs an actually observed reversible UI response without product edits")
    if event.get("requestNonce") != context["phaseRequestNonce"] or event.get("replyRequestNonce") != context["phaseRequestNonce"]:
        raise EvidenceError("Input action/query/reply nonce does not identify the current phase")
    times = [event.get(key) for key in ("queuedEventMonotonicNs", "queryMonotonicNs", "replyMonotonicNs")]
    if any(type(stamp) is not int for stamp in times) or not phase_begin <= times[0] <= times[1] <= times[2] <= now:
        raise EvidenceError("Input event/query/reply clock ordering mismatch or stale pre/post state observation")
    if not isinstance(event.get("rawEvidence"), list) or not event["rawEvidence"]:
        raise MissingEvidence("Actual input round trip has no raw observation")
    for item in event["rawEvidence"]:
        reference(root, item)
    capture_complete = validate_native_wgc_frame(observation["externalClientFramePresented"], window, context, state,
                                                phase, root, now, input_ready=event) if role == "native" else stamps[-1]
    ready = receipt.get("readyMonotonicNs")
    if type(ready) is not int or not max(capture_complete, times[-1]) <= ready <= now:
        raise EvidenceError("Ready/state time must include verified actual capture and this phase's input response")
    acceptance = receipt.get("pathContextAcceptance")
    protocol = "explicit-accepted-path-context-v1" if role == "native" else "legacy-isolated-runtime-v1"
    known_folders = "disabled" if role == "native" else "fixture-isolated-os-profile"
    if not isinstance(acceptance, dict) or acceptance.get("protocol") != protocol or acceptance.get("requestNonce") != context["requestNonce"]:
        raise MissingEvidence("Actual guarded runtime path acceptance for this run is missing")
    flags = ("explicitAcceptedPathContext", "executableGuardPassed", "fixturePathsGuardPassed", "allDataRootsWithinRunFixture")
    if any(acceptance.get(key) is not True for key in flags) or acceptance.get("systemKnownFolderAccess") != known_folders:
        raise EvidenceError("APPDATA/CWD alone cannot isolate actual system known-folder configuration roots")
    if not isinstance(acceptance.get("rawEvidence"), list) or not acceptance["rawEvidence"]:
        raise MissingEvidence("Guarded runtime paths lack actual raw evidence")
    for item in acceptance["rawEvidence"]:
        reference(root, item)
    roots = receipt.get("dataRoots")
    if not isinstance(roots, dict) or set(roots) != {"config", "game", "cache", "temp"}:
        raise MissingEvidence("Actual config/game/cache/temp root observations are missing")
    expected_roots = context.get("explicitAcceptedPathContext", {}).get("dataRoots")
    if expected_roots is not None and roots != expected_roots:
        raise EvidenceError("Observed data roots differ from the exact accepted phase context")
    fixture = Path(context["fixtureRoot"]).resolve()
    for value in roots.values():
        if not isinstance(value, str) or not Path(value).is_absolute() or not Path(value).resolve().is_relative_to(fixture):
            raise EvidenceError("Launcher selected data outside the run-owned synthetic fixture")
        actual = Path(value)
        if actual.resolve() != fixture:
            inside(fixture, actual.relative_to(fixture).as_posix())


def validate_equivalence(root: Path, row: dict, manifests: dict, config: dict) -> dict:
    result = read_json(reference(root, row))
    if not isinstance(result, dict) or result.get("kind") != "full-launcher-equivalence-evidence" or result.get("scope") != "full-launcher":
        raise EvidenceError("Probe/assets/components are not old/native full functional equivalence")
    if result.get("executableSha256s") != {role: manifest["executable"]["sha256"] for role, manifest in manifests.items()}:
        raise EvidenceError("Equivalence evidence does not identify both actual full Release EXEs")
    if result.get("sourceCommits") != {role: manifest["sourceCommit"] for role, manifest in manifests.items()}:
        raise EvidenceError("Equivalence evidence identifies stale/different source commits")
    if canonical_product(manifests["native"]["product"]) == "Palladium":
        for field, expected in (("products", {role: value["product"] for role, value in manifests.items()}),
                                ("sourceSnapshotSha256s", {role: value.get("sourceSnapshotSha256") for role, value in manifests.items()}),
                                ("buildSourceSha256s", {role: value.get("buildSourceSha256") for role, value in manifests.items()})):
            if field not in result:
                raise MissingEvidence(f"Equivalence report lacks same-generation Palladium {field}")
            if result[field] != expected:
                raise EvidenceError("Equivalence report identifies a different product/source/build generation")
    if result.get("inputCorpusSha256") != canonical_sha(config["inputCorpus"]):
        raise EvidenceError("Equivalence evidence uses a different synthetic input corpus")
    if result.get("status") != "passed" or result.get("featuresComplete") is not True or result.get("visualsPreserved") is not True:
        raise MissingEvidence("Full feature/visual equivalence has not passed; no performance comparison allowed")
    if not isinstance(result.get("scenarios"), list) or not isinstance(result.get("themes"), list) or not result.get("rawEvidence"):
        raise MissingEvidence("Full equivalence report lacks inspected scenario/theme/raw evidence")
    if COMMON_SCENARIOS - set(result["scenarios"]) or THEME_IDS - set(result["themes"]):
        raise MissingEvidence("Five common scenarios/all 12 themes need actual equivalence coverage, not repeated probe starts")
    expected = {case["id"]: case["expectedState"] for case in config["scenarios"] if case["comparison"] == "legacy-common"}
    if result.get("expectedStates") != expected:
        raise EvidenceError("Benchmark frozen common states differ from the actually verified equivalent states")
    for item in result["rawEvidence"]:
        reference(root, item)
    return result


def runtime_isolation(root: Path, launcher: dict, manifest: dict) -> dict:
    path = reference(root, launcher.get("runtimeIsolationEvidence"))
    result = read_json(path)
    if not isinstance(result, dict) or result.get("kind") != "actual-launcher-isolation-evidence" or result.get("executableSha256") != manifest["executable"]["sha256"]:
        raise EvidenceError("Synthetic runtime isolation evidence identifies a different product")
    if result.get("launchConfigurationSha256") != launch_configuration_sha(launcher) or result.get("sourceCommit") != manifest["sourceCommit"]:
        raise EvidenceError("Isolation proof uses different launch arguments, environment, source or observation driver")
    bind_native_generation(result, manifest, "Runtime isolation")
    native = manifest["kind"] == "native-release"
    protocol = "explicit-accepted-path-context-v1" if native else "legacy-isolated-runtime-v1"
    known_folders = "disabled" if native else "fixture-isolated-os-profile"
    if result.get("pathContextProtocol") != protocol or result.get("systemKnownFolderAccess") != known_folders:
        raise MissingEvidence("No actual accepted/OS-isolated runtime paths: APPDATA alone does not isolate known folders")
    if result.get("status") != "passed" or any(result.get(key) is not True for key in
               ("allDataRootsWithinRunFixture", "executableGuardPassed", "fixturePathsGuardPassed")) or not result.get("method"):
        raise MissingEvidence("No verified safe launch/config/game/cache/temp paths for this real EXE")
    if not native and (result.get("frozenSourceUnmodified") is not True or result.get("runnerHostAlreadyIsolated") is not True):
        raise MissingEvidence("Frozen legacy requires an actually isolated execution host; this runner cannot sandbox a user profile through cwd/env")
    if not isinstance(result.get("rawEvidence"), list) or not result["rawEvidence"]:
        raise MissingEvidence("Isolation needs actual raw observations, not a claimed cwd")
    for item in result["rawEvidence"]:
        reference(root, item)
    return result


def make_schedule(config: dict) -> tuple[list[dict], dict]:
    if not isinstance(config, dict) or config.get("schemaVersion") != 1 or type(config.get("workerCount")) is not int or config["workerCount"] != 1:
        raise EvidenceError("Version 1 single-worker performance request required")
    if config.get("syntheticInputsOnly") is not True:
        raise EvidenceError("Real accounts/keys/user games are forbidden in fixtures")
    limits = config.get("limits")
    if not isinstance(limits, dict):
        raise EvidenceError("Explicit execution, disk and sampling budgets are required")
    for key, minimum, maximum in (("sampleIntervalMs", 100, 2000), ("settleSeconds", 30, 600),
                                  ("memoryWindowSeconds", 30, 300), ("readyTimeoutSeconds", 5, 120),
                                  ("cooldownSeconds", 1, 120), ("maxFixtureBytes", 1024, 16 * 1024 * 1024),
                                  ("maxExecutionSeconds", 60, 7200), ("maxOwnedOutputBytes", 1024 * 1024, 256 * 1024 * 1024)):
        value = limits.get(key)
        if type(value) not in (int, float) or not math.isfinite(value) or not minimum <= value <= maximum:
            raise EvidenceError(f"Sampling/disk limit {key} must be in [{minimum}, {maximum}]")
    for key in ("maxFixtureBytes", "maxOwnedOutputBytes"):
        if type(limits[key]) is not int:
            raise EvidenceError(f"Byte budget {key} must be an integer")
    seed, scenarios = config.get("scheduleSeed", 0), config.get("scenarios", [])
    if type(seed) is not int or not isinstance(scenarios, list) or len(scenarios) > 32:
        raise EvidenceError("Integer seed and at most 32 explicitly bounded scenarios are required")
    rng, schedule, names = random.Random(seed), [], set()
    for scenario in scenarios:
        if not isinstance(scenario, dict):
            raise EvidenceError("Every scenario must be an object")
        name = scenario.get("id")
        if not isinstance(name, str) or len(name) > 80 or relative_name(name) != name or "/" in name or name in names:
            raise EvidenceError("Unique safe scenario id required")
        names.add(name)
        comparison = scenario.get("comparison")
        if comparison not in {"legacy-common", "native-only-body-incremental"}:
            raise EvidenceError("New body/texture costs must stay outside old common-feature reductions")
        starts, memory_count = scenario.get("startupRunsPerGroup"), scenario.get("memoryRunsPerGroup")
        if type(starts) is not int or type(memory_count) is not int or not 1 <= starts <= 100 or not 0 <= memory_count <= starts:
            raise EvidenceError("Each scenario needs integer 1..100 starts and independent memory repetitions <= starts")
        roles = ["legacy", "native"] if comparison == "legacy-common" else ["native"]
        if len(schedule) + starts * len(GROUPS) * len(roles) > 512:
            raise EvidenceError("Schedule exceeds 512 measured launches; no accidental all-theme multi-hour explosion")
        for group in sorted(GROUPS):
            for index in range(starts):
                order = roles.copy()
                rng.shuffle(order)
                for role in order:
                    schedule.append({"scenario": name, "role": role, "group": group, "index": index,
                                     "memory": index < memory_count})
    warmups = sum(row["group"] == "hot-app-cache" for row in schedule)
    memory = sum(row["memory"] for row in schedule)
    lower = (len(schedule) + warmups) * limits["settleSeconds"] + len(schedule) * limits["cooldownSeconds"] + memory * limits["memoryWindowSeconds"]
    upper = lower + (len(schedule) + warmups) * (limits["readyTimeoutSeconds"] * 5 + 50)
    counts = {role: {group: {"startup": sum(row["role"] == role and row["group"] == group for row in schedule),
                            "memory": sum(row["role"] == role and row["group"] == group and row["memory"] for row in schedule)}
                     for group in sorted(GROUPS)} for role in ("legacy", "native")}
    plan = {"seed": seed, "measuredLaunches": len(schedule), "additionalUncountedWarmups": warmups,
            "independentMemoryWindows": memory, "perLauncher": counts, "nominalMinimumSeconds": lower,
            "worstCaseBoundSeconds": upper, "maxExecutionSeconds": limits["maxExecutionSeconds"],
            "note": "At least 20 starts/10 independent memory per launcher TOTAL, split cold/hot; themes are a separate equivalence gate."}
    return schedule, plan


def preflight(root: Path, config: dict) -> tuple[dict, dict, dict]:
    _, plan = make_schedule(config)
    limits, manifests, measured = config["limits"], {}, {}
    launchers = config.get("launchers")
    if not isinstance(launchers, dict):
        raise MissingEvidence("Actual complete legacy/native Release manifests are required")
    for role in ("legacy", "native"):
        launcher = launchers.get(role)
        if not isinstance(launcher, dict) or not launcher.get("artifactManifest"):
            raise MissingEvidence(f"No actual complete {role} Release launcher Artifact manifest")
        manifests[role] = read_json(reference(root, launcher["artifactManifest"]))
        measured[role] = verify_manifest(root, manifests[role], expected_kind=role + "-release", require_current_native=role == "native")
        driver = launcher.get("driver")
        if not isinstance(driver, dict):
            raise MissingEvidence(f"No actual {role} ready/state/shutdown UI observer driver")
        if reference(root, driver.get("source")).suffix != ".py":
            raise EvidenceError("The driver entry must be hash-identified Python source")
        adapter = "native-production-markers-v1" if role == "native" else "legacy-external-ui-observer-v1"
        if driver.get("protocol") != "nickel-evidence-driver-v1" or driver.get("prearmedReady") is not True or driver.get("observationAdapter") != adapter:
            raise MissingEvidence("Native internal markers and frozen legacy external UI observation need distinct prearmed adapters")
        if type(driver.get("pollIntervalMs")) is not int or not 1 <= driver["pollIntervalMs"] <= 100:
            raise EvidenceError("Ready sampling uncertainty must be explicitly 1..100 ms")
        launch_configuration_sha(launcher)
        fixture_root_names(launcher, native=role == "native")
        runtime, _ = runtime_layout(root, manifests[role])
        if sum(row["bytes"] for row in runtime) + limits["maxFixtureBytes"] + MAX_RUN_RAW_BYTES >= limits["maxOwnedOutputBytes"]:
            raise EvidenceError("Actual physical runtime and one fixture/observation do not fit the owned-output budget")
        if role == "native" and not any("{context}" in argument for argument in launcher["launchArgs"]):
            raise MissingEvidence("Native opt-in launch must receive the actual guarded per-launch context before normal user-root selection")
        runtime_isolation(root, launcher, manifests[role])
    baseline = config.get("legacyBaseline")
    if not isinstance(baseline, dict):
        raise MissingEvidence("The exact frozen legacy version and source oracle must be supplied")
    if (baseline.get("version") != "0.1.0" or baseline.get("sourceCommit") != "b19e28b818b353c9812d71d4ba475d1e26e799e9" or
            manifests["legacy"]["version"] != baseline["version"] or manifests["legacy"]["sourceCommit"] != baseline["sourceCommit"]):
        raise EvidenceError("Published 1.0.1 or a declared target_commitish is not the frozen feature-equivalent legacy 0.1.0 build")
    frozen = read_json(reference(root, baseline.get("sourceInventory")))
    if frozen.get("kind") != "frozen-legacy-oracle-git-inventory" or frozen.get("base") != baseline["sourceCommit"] or frozen.get("fileCount") != 442:
        raise EvidenceError("Legacy source inventory must identify all 442 exact oracle Git blobs")
    if (manifests["legacy"]["platform"], manifests["legacy"]["architecture"]) != (manifests["native"]["platform"], manifests["native"]["architecture"]):
        raise EvidenceError("Legacy/native platform or architecture differs")
    if manifests["native"]["platform"] != "windows" or manifests["native"]["architecture"] != "x64":
        raise MissingEvidence("This adapter is Windows-only; Linux PSS/macOS footprint need actual target hosts")
    corpus = config.get("inputCorpus")
    if not isinstance(corpus, list) or not corpus:
        raise MissingEvidence("Hash-identified synthetic input corpus is missing")
    for row in corpus:
        reference(root, row)
    hashes = {row["sha256"] for row in corpus}
    scenarios, names, totals = config["scenarios"], set(), {"startup": 0, "memory": 0}
    for scenario in scenarios:
        name, expected = scenario["id"], scenario.get("expectedState")
        names.add(name)
        validate_state(expected)
        if scenario["comparison"] == "legacy-common":
            totals["startup"] += scenario["startupRunsPerGroup"]
            totals["memory"] += scenario["memoryRunsPerGroup"]
        for role in ("legacy", "native") if scenario["comparison"] == "legacy-common" else ("native",):
            fixtures = scenario.get("fixtureFiles", {}).get(role)
            if not isinstance(fixtures, list) or not fixtures:
                raise MissingEvidence(f"Missing owned synthetic {role} fixture files for {name}")
            destinations, size = set(), 0
            for row in fixtures:
                size += reference(root, row).stat().st_size
                if row["sha256"] not in hashes or type(row.get("bytes")) is not int:
                    raise EvidenceError("Every fixture must have exact bytes and be part of the frozen input corpus")
                destination = relative_name(row.get("destination"))
                if destination.casefold() in destinations:
                    raise EvidenceError("Fixture paths collide")
                destinations.add(destination.casefold())
            if size > limits["maxFixtureBytes"]:
                raise EvidenceError("Fixture exceeds bounded owned disk budget; no full source/prefix/game copies")
        if name == "home-4k-blur" and expected["backgroundSha256"] not in hashes:
            raise MissingEvidence("4K+blur needs actual local background bytes in the input corpus")
    if COMMON_SCENARIOS - names:
        raise MissingEvidence("Five common performance scenarios missing: " + ", ".join(sorted(COMMON_SCENARIOS - names)))
    if totals["startup"] < 10 or totals["memory"] < 5:
        raise EvidenceError("Need >=20 startup and >=10 independent memory per launcher total, equally split cold/hot")
    idle = next((case for case in scenarios if case["id"] == config.get("idleBaselineScenario")), None)
    if idle is None or idle["comparison"] != "legacy-common" or idle["memoryRunsPerGroup"] < 5 or idle["expectedState"]["activeTaskCount"] != 0:
        raise MissingEvidence("Idle target needs >=5 independent memory per group in the same frozen idle state")
    cache = config.get("cachePolicy", {})
    if cache.get("groups") != sorted(GROUPS) or cache.get("hotWarmupRuns") != 1 or cache.get("osCache") != "uncontrolled-recorded":
        raise EvidenceError("These groups are APPLICATION cache only; an OS cold reboot study is external")
    if plan["nominalMinimumSeconds"] > limits["maxExecutionSeconds"]:
        raise MissingEvidence("Dry-run schedule exceeds the explicit time budget")
    equivalence = validate_equivalence(root, config.get("equivalenceEvidence"), manifests, config)
    return manifests, measured, equivalence


def driver_command(driver: dict, action: str, context: Path, output: Path) -> list[str]:
    source = reference(ROOT, driver["source"])
    return [sys.executable, "-B", str(source), action, "--context", str(context), "--output", str(output)]


def run_driver(driver: dict, action: str, context: dict, output: Path, environment: dict, timeout: float) -> dict:
    request = {**context, "phase": action, "phaseRequestNonce": uuid.uuid4().hex, "phaseBeginNs": time.perf_counter_ns()}
    context_path = output.with_suffix(".context.json")
    write_json(context_path, request)
    if request.get("role") == "native":
        publish_phase_request(context_path)
    process = BoundedProcess(driver_command(driver, action, context_path, output), cwd=ROOT, environment=environment,
                             log=output.with_suffix(".command.log"))
    try:
        code = process.wait(timeout)
    except Exception:
        process.terminate()
        process.wait(5)
        raise
    if code:
        raise EvidenceError(f"Actual UI driver {action} failed with exit {code}")
    receipt = read_json(output)
    validate_receipt(receipt, request, request["expectedState"], action)
    return receipt


def seed_fixture(files: list[dict], destination: Path, max_bytes: int) -> dict:
    if destination.exists():
        raise EvidenceError("Fixture root must be unused; never replace user state")
    total = sum(reference(ROOT, row).stat().st_size for row in files)
    if total > max_bytes:
        raise EvidenceError("Fixture budget exceeded before writing")
    check_space(destination, total)
    destination.mkdir(parents=True, exist_ok=False)
    copied = []
    for row in files:
        write_new(inside(destination, row["destination"]), reference(ROOT, row).read_bytes())
        copied.append({"destination": row["destination"], "sha256": row["sha256"], "bytes": row["bytes"]})
    write_json(destination / "fixture-inputs.manifest.json", copied)
    return {"bytes": total, "sha256": canonical_sha(copied), "files": copied}


def bounded_append(stream, value, written: int) -> int:
    raw = json.dumps(value, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode("ascii") + b"\n"
    if written + len(raw) > MAX_RUN_RAW_BYTES:
        raise EvidenceError("Raw evidence exceeded16MiB; stop, never truncate samples")
    check_space(Path(stream.name), len(raw))
    stream.write(raw)
    stream.flush()
    return written + len(raw)


def perform_run(config: dict, manifest: dict, scenario: dict, role: str, group: str, index: int, directory: Path,
                fixture: Path, *, measure_memory: bool, warmup: bool = False, execution_deadline: float | None = None) -> dict:
    limits, launcher = config["limits"], config["launchers"][role]
    isolation = runtime_isolation(ROOT, launcher, manifest)
    check_space(directory, 2 * 1024 * 1024)
    directory.mkdir(parents=True, exist_ok=False)
    owned_bytes = sum(path.stat().st_size for path in directory.parent.rglob("*") if path.is_file())
    remaining_bytes = limits["maxOwnedOutputBytes"] - owned_bytes - MAX_RUN_RAW_BYTES
    deployment = prepare_runtime_fixture(ROOT, manifest, fixture, launcher, max_bytes=remaining_bytes)
    data_roots = deployment["dataRoots"]
    temp = Path(data_roots["temp"])
    nonce = uuid.uuid4().hex
    context = {"schemaVersion": 1, "protocol": "nickel-evidence-driver-v1", "runId": uuid.uuid4().hex,
               "requestNonce": nonce, "phaseRequestNonce": nonce, "phase": "ready", "role": role,
               "observationAdapter": launcher["driver"]["observationAdapter"], "scenario": scenario["id"], "scenarioId": scenario["id"], "group": group,
               "executableSha256": manifest["executable"]["sha256"], "fixtureRoot": str(fixture),
               "inputCorpusSha256": canonical_sha(config["inputCorpus"]), "sourceCommit": manifest["sourceCommit"],
               "sourceSnapshotSha256": manifest.get("sourceSnapshotSha256"), "buildSourceSha256": manifest.get("buildSourceSha256"),
               "driverSourceSha256": launcher["driver"]["source"]["sha256"], "expectedState": scenario["expectedState"],
               "rootPid": None, "rootCreationTime100ns": None, "launchContext": str(directory / "launch-context.json"),
               "armedReceipt": str(directory / "armed.json"), "productionEvents": str(directory / "production-events.raw.jsonl"),
               "explicitAcceptedPathContext": {"protocol": isolation["pathContextProtocol"], "runFixture": str(fixture),
                    "executableSha256": manifest["executable"]["sha256"], "systemKnownFolderAccess": isolation["systemKnownFolderAccess"],
                    "dataRoots": data_roots}}
    context_path = directory / "driver-context.json"
    write_json(context_path, context)
    values = {"fixture": str(fixture), "context": str(context_path)}
    environment = os.environ.copy()
    environment.update({"TEMP": str(temp), "TMP": str(temp), "PYTHONDONTWRITEBYTECODE": "1"})
    for name, value in launcher.get("environment", {}).items():
        environment[name] = expand_argv([value], values)[0]
    deadline = execution_deadline if execution_deadline is not None else time.monotonic() + limits["maxExecutionSeconds"]

    def require_budget():
        if time.monotonic() >= deadline:
            raise MissingEvidence("Explicit total execution budget exhausted; incomplete samples remain pending")
        size = sum(path.stat().st_size for path in directory.parent.rglob("*") if path.is_file())
        if size >= limits["maxOwnedOutputBytes"]:
            raise EvidenceError("Owned evidence/fixture output budget exceeded; no samples are silently truncated")
        check_space(directory, 2 * 1024 * 1024)

    def remaining(requested):
        require_budget()
        return min(requested, max(0.001, deadline - time.monotonic()))

    process = observer = gpu = driver = None
    snapshots, errors, samples, memory_samples = [], [], [], []
    startup, cleanup_errors, written = None, [], 0
    memory_window_start = memory_window_end = None
    trace = EventTrace(directory, int(limits["readyTimeoutSeconds"] * 5 + limits["settleSeconds"] + limits["memoryWindowSeconds"] + 120))
    trace.start()
    try:
        require_budget()
        ready_path = directory / "ready.json"
        driver = BoundedProcess(driver_command(launcher["driver"], "ready", context_path, ready_path), cwd=ROOT,
                                environment=environment, log=directory / "ready.command.log")
        arm_deadline = min(deadline, time.monotonic() + limits["readyTimeoutSeconds"])
        while not Path(context["armedReceipt"]).exists():
            if driver.poll() is not None or driver.error or time.monotonic() >= arm_deadline:
                raise MissingEvidence("Actual observation driver did not arm before product process creation")
            require_budget()
            time.sleep(0.01)
        armed = read_json(Path(context["armedReceipt"]))
        fields = ("runId", "scenarioId", "requestNonce", "role", "observationAdapter", "executableSha256", "driverSourceSha256", "inputCorpusSha256")
        if (not isinstance(armed, dict) or armed.get("schemaVersion") != 1 or armed.get("status") != "armed" or
                any(armed.get(key) != context[key] for key in fields)):
            raise EvidenceError("Prearmed receipt belongs to a different driver/launch")
        armed_time = armed.get("armedMonotonicNs")
        if type(armed_time) is not int or not driver.creation_begin_ns <= armed_time <= time.perf_counter_ns():
            raise EvidenceError("Driver was not actually armed on the shared process-creation clock")
        if not isinstance(armed.get("rawEvidence"), list) or not armed["rawEvidence"]:
            raise MissingEvidence("Prelaunch observer subscription lacks actual raw evidence")
        for item in armed["rawEvidence"]:
            reference(ROOT, item)
        executable = Path(deployment["executablePath"])
        reference(ROOT, next(row for row in deployment["deployedFiles"] if row["path"] == executable.relative_to(ROOT).as_posix()))
        require_budget()
        argv = [str(executable), *expand_argv(launcher["launchArgs"], values)]
        process = BoundedProcess(argv, cwd=fixture, environment=environment, log=directory / "launcher-output.log")
        begin = process.creation_begin_ns
        candidate = WindowsObserver(process.pid)
        created = candidate.identity(process.pid, process.process._handle)
        if candidate.root_identity != (process.pid, created["creationTime100ns"]) or Path(created["executable"]).resolve() != executable.resolve():
            raise EvidenceError("Live PID does not match the process-creation handle/executable; do not claim or terminate a reused PID")
        observer = candidate
        context.update({"rootPid": process.pid, "rootCreationTime100ns": observer.root_identity[1], "phaseBeginNs": begin,
                        "launchBeginNs": begin, "launchCreateReturnedNs": process.creation_end_ns, "launchArgv": argv})
        initial_path = directory / "launch-initial.context.json"
        write_json(initial_path, context)
        if role == "native":
            publish_phase_request(initial_path)
        else:
            write_json(Path(context["launchContext"]), context)
        gpu = GpuObserver()
        with (directory / "samples.raw.jsonl").open("xb") as stream:
            def capture(phase):
                nonlocal written
                require_budget()
                row = observer.sample(gpu)
                written = bounded_append(stream, {"phase": phase, **row}, written)
                samples.append(row)
                if phase == "memory":
                    memory_samples.append(row)
                if process.error or driver.error:
                    raise EvidenceError(process.error or driver.error)
                if process.poll() is not None:
                    raise EvidenceError("Actual launcher exited during measured window")
            ready_deadline = min(deadline, time.monotonic() + limits["readyTimeoutSeconds"])
            while time.monotonic() < ready_deadline:
                capture("startup")
                if ready_path.exists() and driver.poll() is not None:
                    if driver.wait(5):
                        raise EvidenceError("Actual prearmed ready driver failed")
                    receipt = read_json(ready_path)
                    validate_receipt(receipt, context, scenario["expectedState"], "ready")
                    stamp, now = receipt["readyMonotonicNs"], time.perf_counter_ns()
                    startup = {"milliseconds": (stamp - begin) / 1_000_000, "parentObservedUpperBoundMs": (now - begin) / 1_000_000,
                               "driverReceipt": file_ref(ready_path), "armedReceipt": file_ref(Path(context["armedReceipt"])),
                               "pollIntervalMs": launcher["driver"]["pollIntervalMs"], "processCreationCallMs": (process.creation_end_ns - begin) / 1_000_000,
                               "basis": context["observationAdapter"] + "; actual Windows presentation plus this phase's input response, not helper startup/onboarding"}
                    break
                time.sleep(limits["sampleIntervalMs"] / 1000)
            if startup is None:
                raise MissingEvidence("No actual presented responsive first frame within timeout")
            snapshots.append(observer.module_snapshot(scenario["id"] + ":ready", launcher.get("childPurposes")))
            settle = time.monotonic() + limits["settleSeconds"]
            while time.monotonic() < settle:
                capture("settle")
                time.sleep(limits["sampleIntervalMs"] / 1000)
            run_driver(launcher["driver"], "state", context, directory / "state-before.json", environment, remaining(limits["readyTimeoutSeconds"]))
            if measure_memory:
                memory_window_start = time.perf_counter_ns()
                until = time.monotonic() + limits["memoryWindowSeconds"]
                while time.monotonic() < until:
                    capture("memory")
                    time.sleep(limits["sampleIntervalMs"] / 1000)
                memory_window_end = time.perf_counter_ns()
                run_driver(launcher["driver"], "state", context, directory / "state-after.json", environment, remaining(limits["readyTimeoutSeconds"]))
                snapshots.append(observer.module_snapshot(scenario["id"] + ":memory-end", launcher.get("childPurposes")))
            run_driver(launcher["driver"], "shutdown", context, directory / "shutdown.json", environment, remaining(10))
            if process.wait(remaining(10)):
                raise EvidenceError("Actual launcher normal shutdown returned an error")
    except (OSError, EvidenceError, subprocess.SubprocessError, KeyError, TypeError, AttributeError) as error:
        errors.append({"status": "pending" if isinstance(error, MissingEvidence) else "failed", "error": str(error)})
    finally:
        if observer:
            try:
                alive, tree_errors = observer.tree(require_root=False)
                cleanup_errors += tree_errors
                if alive:
                    cleanup_errors += [{"pid": row["pid"], "error": "Owned process still alive; forced cleanup is not normal shutdown"} for row in alive]
                    cleanup_errors += observer.terminate_owned()
            except (OSError, EvidenceError) as error:
                cleanup_errors.append({"error": str(error)})
        if driver and driver.poll() is None:
            driver.terminate()
        for owned in (driver, process):
            if owned:
                try:
                    if owned is process and observer is None:
                        owned.terminate()
                    owned.wait(5)
                except (OSError, EvidenceError, subprocess.SubprocessError) as error:
                    cleanup_errors.append({"pid": owned.pid, "error": str(error)})
        if gpu:
            gpu.close()
    trace_result = trace.finish()
    observed = {(row["pid"], row["creationTime100ns"]) for sample in samples for row in sample["processes"]}
    lifetime = {"completeObservedLifetime": False}
    if observer:
        try:
            lifetime = trace_descendants(trace.read_events(), context["rootPid"], context["rootCreationTime100ns"], observed,
                                         trace_observed=trace_result["status"] == "observed")
        except (OSError, EvidenceError) as error:
            errors.append({"status": "pending", "error": str(error)})
    memory_complete = bool(memory_samples and memory_window_end) and all(row["treeCompleteAtObservation"] and row["totals"] for row in memory_samples)
    memory = {metric: stats([row["totals"][metric] for row in memory_samples]) for metric in
              ("privateBytes", "privateWorkingSetBytes", "workingSetBytes", "processCount")} if memory_complete else None
    if memory:
        memory.update({"windowSeconds": limits["memoryWindowSeconds"], "observedWindowSeconds": (memory_window_end - memory_window_start) / 1e9,
                       "intervalMs": limits["sampleIntervalMs"], "completeAtLiveObservations": True, "fullLifetimeComplete": lifetime["completeObservedLifetime"]})
    gpu_complete = memory_complete and all(row["gpu"]["status"] == "observed" for row in memory_samples)
    gpu_result = {metric: stats([row["gpu"]["totals"][metric] for row in memory_samples]) for metric in
                  ("dedicatedBytes", "sharedBytes")} if gpu_complete else None
    gates = {
        "executionState": gate("failed" if cleanup_errors or any(row["status"] == "failed" for row in errors) else "pending" if errors else "passed", "Actual product ready/fresh-state/normal-shutdown", errors=errors, cleanupErrors=cleanup_errors),
        "startup": gate("passed" if startup else "pending", "Actual Windows presentation plus phase-matched input response"),
        "fullLifetimeTree": gate("passed" if lifetime["completeObservedLifetime"] else "pending", "Live snapshots plus actual start/stop trace; ephemeral gaps never disappear"),
        "cpuMemory": gate("passed" if memory_complete or not measure_memory else "pending", "Full independent live-tree private WS/private bytes window"),
        "gpu": gate("passed" if gpu_complete or not measure_memory else "pending", "GPU independently observed or explicitly unavailable, never zero"),
        "modules": gate("passed" if snapshots and all(row["treeComplete"] for row in snapshots) else "pending", "Actually loaded root/child module paths")}
    module_evidence = {"schemaVersion": 1, "executableSha256": manifest["executable"]["sha256"], "snapshots": snapshots,
                       "collection": {"origin": "actual-windows-process-observation", "collector": file_ref(Path(__file__).with_name("windows_metrics.py")),
                                      "machine": machine_metadata(), "processTrace": trace_result}}
    write_json(directory / "modules.json", module_evidence)
    result = {"schemaVersion": 1, "kind": "actual-full-launcher-performance-run", "status": overall(gates), "context": context,
              "index": index, "warmup": warmup, "startup": startup, "memory": memory, "gpu": gpu_result, "gates": gates,
              "runtimeDeployment": file_ref(fixture / "runtime-deployment.evidence.json"),
              "processTrace": trace_result, "processLifetime": lifetime, "modules": file_ref(directory / "modules.json"),
              "samples": file_ref(directory / "samples.raw.jsonl") if (directory / "samples.raw.jsonl").exists() else None,
              "sampleObservationErrors": [row for sample in samples for row in sample["errors"]],
              "limitations": ["All descendants remain counted, including legacy WebView; child purposes never remove memory.",
                              "Cold/hot APPLICATION cache is not OS cold boot. Missing GPU/lifetime/modules stay pending.",
                              "Native markers and externally observed frozen legacy pixels/actions have distinct methods; no product/source substitution.",
                              "Pre/post fresh phase nonces and guarded config/game/cache/temp roots prevent changed features from becoming savings."]}
    write_json(directory / "run-evidence.json", result)
    return result


def summarize(config: dict, runs: list[dict], machine: dict, manifests: dict) -> dict:
    grouped = defaultdict(list)
    for run in runs:
        if not run["warmup"]:
            context = run["context"]
            grouped[(context["scenario"], context["role"], context["group"])].append(run)
    aggregates, gates = [], {}
    for (scenario, role, group), values in sorted(grouped.items()):
        aggregates.append({"scenario": scenario, "role": role, "group": group,
                           "startupMs": stats([row["startup"]["milliseconds"] for row in values if row["startup"]]),
                           "privateBytes": stats([row["memory"]["privateBytes"]["median"] for row in values if row["memory"]]),
                           "privateWorkingSetBytes": stats([row["memory"]["privateWorkingSetBytes"]["median"] for row in values if row["memory"]]),
                           "gpuDedicatedBytes": stats([row["gpu"]["dedicatedBytes"]["median"] for row in values if row["gpu"]]),
                           "gpuSharedBytes": stats([row["gpu"]["sharedBytes"]["median"] for row in values if row["gpu"]]),
                           "completeRuns": sum(row["status"] == "passed" for row in values), "attemptedRuns": len(values),
                           "basis": "All valid raw metric observations shown even when a separate gate is pending; no failing attempt is discarded"})
    failed = sum(row["status"] == "failed" for row in runs)
    pending = sum(row["status"] == "pending" for row in runs)
    gates["actualRuns"] = gate("failed" if failed else "pending" if pending or not runs else "passed", "Every attempt/warmup retained", failedCount=failed, pendingCount=pending)
    for role in ("legacy", "native"):
        for group in sorted(GROUPS):
            values = [row for row in runs if not row["warmup"] and row["context"]["role"] == role and row["context"]["group"] == group and
                      next(case for case in config["scenarios"] if case["id"] == row["context"]["scenario"])["comparison"] == "legacy-common"]
            starts = sum(row["startup"] is not None and row["status"] == "passed" for row in values)
            memory_count = sum(row["memory"] is not None and row["status"] == "passed" for row in values)
            gates[f"samples:{role}:{group}"] = gate("passed" if starts >= 10 and memory_count >= 5 else "pending", "Total per launcher:10 startups+5 independent memory per cold/hot group", startupCount=starts, memoryCount=memory_count)
    baseline = config["idleBaselineScenario"]
    for group in sorted(GROUPS):
        old = [row["memory"]["privateWorkingSetBytes"]["median"] for row in grouped[(baseline, "legacy", group)] if row["status"] == "passed" and row["memory"]]
        new = [row["memory"]["privateWorkingSetBytes"]["median"] for row in grouped[(baseline, "native", group)] if row["status"] == "passed" and row["memory"]]
        reduction = 1 - statistics.median(new) / statistics.median(old) if len(old) >= 5 and len(new) >= 5 and statistics.median(old) > 0 else None
        gates[f"memoryTarget:{group}"] = gate("pending" if reduction is None else "passed" if reduction >= 0.50 else "failed", "Same frozen idle state, complete tree medians; measured only", measuredReduction=reduction, targetReduction=0.50)
    for scenario in config["scenarios"]:
        roles = ("legacy", "native") if scenario["comparison"] == "legacy-common" else ("native",)
        gates["coverage:" + scenario["id"]] = gate("passed" if all(any(row["status"] == "passed" for group in GROUPS for row in grouped[(scenario["id"], role, group)]) for role in roles) else "pending", "Shared scenes compared separately; native-only body cost has no legacy reduction ratio")
    native = manifests["native"]
    return {"schemaVersion": 1, "kind": "full-launcher-performance-evidence", "scope": "full-launcher", "createdUtc": utc_now(),
            "product": native["product"], "products": {role: row["product"] for role, row in manifests.items()},
            "sourceCommit": native["sourceCommit"], "sourceSnapshotSha256": native.get("sourceSnapshotSha256"), "buildSourceSha256": native.get("buildSourceSha256"),
            "executableSha256": native["executable"]["sha256"], "executableSha256s": {role: row["executable"]["sha256"] for role, row in manifests.items()},
            "sourceCommits": {role: row["sourceCommit"] for role, row in manifests.items()},
            "sourceSnapshotSha256s": {role: row.get("sourceSnapshotSha256") for role, row in manifests.items()},
            "buildSourceSha256s": {role: row.get("buildSourceSha256") for role, row in manifests.items()},
            "status": overall(gates), "nativeReleaseReady": False,
            "gates": gates, "aggregates": aggregates, "rawRuns": runs, "machine": machine, "cachePolicy": config["cachePolicy"],
            "configSha256": canonical_sha(config), "inputCorpusSha256": canonical_sha(config["inputCorpus"]),
            "metrics": {"windows": "Full live-tree private working set from QueryWorkingSet nonshared pages; private bytes separately",
                        "gpu": "PDH GPU Process Memory dedicated/shared; unavailable stays pending", "linux": "pending actual target-host PSS/RSS", "macos": "pending actual target-host footprint"}}


def execute(config: dict, manifests: dict, output: Path) -> dict:
    machine = machine_metadata()
    write_json(output / "machine.raw.json", machine)
    schedule, plan = make_schedule(config)
    write_json(output / "schedule.raw.json", {"plan": plan, "schedule": schedule})
    started, runs, stop_reason = time.monotonic(), [], None
    deadline = started + config["limits"]["maxExecutionSeconds"]
    for item in schedule:
        if time.monotonic() >= deadline:
            stop_reason = "total execution budget exhausted before next measured launch"
            break
        size = sum(path.stat().st_size for path in output.rglob("*") if path.is_file())
        runtime, _ = runtime_layout(ROOT, manifests[item["role"]])
        runtime_bytes = sum(row["bytes"] for row in runtime)
        if size + runtime_bytes + config["limits"]["maxFixtureBytes"] + MAX_RUN_RAW_BYTES >= config["limits"]["maxOwnedOutputBytes"]:
            stop_reason = "owned fixture/evidence output budget exhausted before next launch"
            break
        case = next(row for row in config["scenarios"] if row["id"] == item["scenario"])
        role, group, index = item["role"], item["group"], item["index"]
        fixture = output / f"fixture-{case['id']}-{role}-{group}-{index:03d}"
        try:
            seed_fixture(case["fixtureFiles"][role], fixture, config["limits"]["maxFixtureBytes"])
            if group == "hot-app-cache":
                warmup = perform_run(config, manifests[role], case, role, group, index,
                                     output / f"warmup-{case['id']}-{role}-{index:03d}", fixture,
                                     measure_memory=False, warmup=True, execution_deadline=deadline)
                runs.append(warmup)
                if warmup["gates"]["executionState"]["status"] != "passed" or not warmup["startup"]:
                    stop_reason = "warmup did not normally initialize the actual frozen application cache"
                    break
            run = perform_run(config, manifests[role], case, role, group, index,
                              output / f"run-{case['id']}-{role}-{group}-{index:03d}", fixture,
                              measure_memory=item["memory"], execution_deadline=deadline)
            runs.append(run)
            if run["gates"]["executionState"]["status"] != "passed" or not run["startup"]:
                stop_reason = "actual product execution/state/normal-shutdown gate did not pass"
                break
            check_space(output, 2 * 1024 * 1024)
            time.sleep(min(config["limits"]["cooldownSeconds"], max(0, deadline - time.monotonic())))
        except (OSError, EvidenceError, subprocess.SubprocessError, KeyError, TypeError, AttributeError) as error:
            stop_reason = str(error)
            write_json(output / "execution-stop.json", {"status": "pending" if isinstance(error, MissingEvidence) else "failed",
                       "error": stop_reason, "scheduledItem": item, "completedAttemptCount": len(runs), "createdUtc": utc_now()})
            break
    result = summarize(config, runs, machine, manifests)
    result["dryRunPlan"] = plan
    result["elapsedSeconds"] = time.monotonic() - started
    result["stopReason"] = stop_reason
    result["scheduledLaunches"] = len(schedule)
    result["attemptedMeasuredLaunches"] = sum(not row["warmup"] for row in runs)
    if stop_reason:
        status = read_json(output / "execution-stop.json")["status"] if (output / "execution-stop.json").exists() else "pending"
        result["gates"]["executionCompletion"] = gate(status, stop_reason)
        result["status"] = overall(result["gates"])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--execute", action="store_true", help="Only launch after verified full products/equivalent states/isolated roots")
    args = parser.parse_args()
    output = new_output(args.output, minimum_bytes=2 * 1024 * 1024)
    try:
        path = inside(ROOT, args.input, must_exist=True)
        config = read_json(path)
        copy_small_input(path, output, "performance-request.raw.json")
        schedule, plan = make_schedule(config)
        write_json(output / "dry-run.json", {"plan": plan, "schedule": schedule})
        manifests, measured, equivalence = preflight(ROOT, config)
        write_json(output / "preflight.json", {"status": "passed", "measuredArtifact": measured, "equivalence": equivalence})
        if not args.execute:
            raise MissingEvidence("Preflight/dry-run only; actual full launcher starts/memory have not been measured")
        if os.name != "nt":
            raise MissingEvidence("No actual Windows host for this metric adapter")
        with SingleWorker(inside(ROOT, LOCAL)):
            result = execute(config, manifests, output)
    except (MissingEvidence, OSError, EvidenceError, subprocess.SubprocessError) as error:
        status = "pending" if isinstance(error, MissingEvidence) else "failed"
        result = {"schemaVersion": 1, "kind": "full-launcher-performance-evidence", "status": status, "nativeReleaseReady": False,
                  "gates": {"inputsOrExecution": gate(status, str(error))}, "rawRuns": [], "measuredReduction": None,
                  "note": "No probe/assets/component/synthetic binary substitutes for the launcher;20%/50% are never guessed"}
    write_json(output / "performance-evidence.json", result)
    print(f"{result['status']}: {output.relative_to(ROOT).as_posix()}/performance-evidence.json")
    return exit_code(result["status"])


if __name__ == "__main__":
    raise SystemExit(main())
