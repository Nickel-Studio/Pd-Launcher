"""Persist actual owned-helper observer checks; explicitly not launcher/release evidence."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import time

from bounded_process import BoundedProcess
from evidence_common import EvidenceError, ROOT, exit_code, file_ref, gate, overall, write_json
from event_trace import EventTrace, trace_descendants
from windows_metrics import GpuObserver, WindowsObserver, machine_metadata


def run(directory: Path) -> dict:
    if os.name != "nt":
        result = {"kind": "release-tool-observer-selftest", "scope": "owned-helper-only", "status": "pending",
                  "reason": "No Windows host for actual PSAPI/PDH/WMI selftest", "nativeReleaseReady": False}
        write_json(directory / "observer-selftest.json", result)
        return result
    trace = EventTrace(directory, 20)
    samples, modules, errors, cleanup = [], [], [], []
    process = observer = gpu = None
    trace.start()
    try:
        helper = "import subprocess,sys,time;p=subprocess.Popen([sys.executable,'-B','-c','import time;time.sleep(3)']);time.sleep(3.5);p.wait()"
        environment = os.environ.copy()
        environment.update({"TEMP": str(directory), "TMP": str(directory), "PYTHONDONTWRITEBYTECODE": "1"})
        process = BoundedProcess([sys.executable, "-B", "-c", helper], cwd=directory,
                                 environment=environment, log=directory / "helper.raw.log")
        observer, gpu = WindowsObserver(process.pid), GpuObserver()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            sample = observer.sample(gpu)
            samples.append(sample)
            if sample.get("totals") and sample["totals"]["processCount"] >= 2:
                modules.append(observer.module_snapshot("owned-helper-selftest-not-launcher"))
                break
            time.sleep(0.05)
        process.wait(5)
    except (OSError, EvidenceError, subprocess.SubprocessError) as error:
        errors.append(str(error))
    finally:
        if process and process.poll() is None:
            if observer:
                cleanup += observer.terminate_owned()
            else:
                process.terminate()
            try:
                process.wait(5)
            except (OSError, EvidenceError, subprocess.SubprocessError) as error:
                cleanup.append({"pid": process.pid, "error": str(error)})
        if gpu:
            gpu.close()
    trace_result = trace.finish()
    observed = {(row["pid"], row["creationTime100ns"]) for sample in samples for row in sample["processes"]}
    lifetime = {"completeObservedLifetime": False}
    if observer:
        try:
            lifetime = trace_descendants(trace.read_events(), process.pid, observer.root_identity[1], observed,
                                         trace_observed=trace_result["status"] == "observed")
        except (OSError, EvidenceError) as error:
            errors.append(str(error))
    live_success = bool(samples and modules and samples[-1].get("totals") and
                        samples[-1]["totals"]["processCount"] >= 2 and modules[-1]["treeComplete"])
    gpu_observed = bool(samples and all(row["gpu"]["status"] == "observed" for row in samples))
    gates = {"liveTreeMemoryModules": gate("passed" if live_success else "failed", "Actual owned Python parent/child PSAPI checks"),
             "execution": gate("failed" if errors or cleanup else "passed", "Bounded owned helper normal exit", errors=errors, cleanupErrors=cleanup),
             "lifetime": gate("passed" if lifetime["completeObservedLifetime"] else "pending", "Missing WMI trace/root/child birth or stop never becomes a complete lifetime"),
             "gpu": gate("passed" if gpu_observed else "pending", "PDH availability is independent; absent GPU counters are not zero")}
    result = {"schemaVersion": 1, "kind": "release-tool-observer-selftest", "scope": "owned-helper-only",
              "status": overall(gates), "gates": gates, "nativeReleaseReady": False,
              "machine": machine_metadata(), "rootPid": process.pid if process else None,
              "creationTime100ns": observer.root_identity[1] if observer else None,
              "samples": samples, "moduleSnapshots": modules, "processTrace": trace_result,
              "lifetime": lifetime, "collectorSource": file_ref(Path(__file__)),
              "limitations": ["Python helpers are tool checks, never launcher performance or reduction evidence.",
                              "GPU availability and WMI lifetime status remain independent pending gates when unavailable."]}
    write_json(directory / "observer-selftest.json", result)
    return result


if __name__ == "__main__":
    import argparse
    from evidence_common import new_output
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    result = run(new_output(args.output))
    print(result["status"])
    raise SystemExit(exit_code(result["status"]))
