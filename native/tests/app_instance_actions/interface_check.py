"""Record exact supplied interfaces. This is not a production acceptance result."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
SHARED = ROOT / ".snow" / "native-shared"
REQUIRED = {
    "native/app/application_state.hpp": ("action_instances", "install_pack", "operation_task"),
    "native/app/application_state.cpp": ("install_pack",),
    "native/core/instances/instances.hpp": ("patch_instance_metadata",),
    "native/core/instances/settings.hpp": ("set_version_settings", "reserved_resource_directory"),
    "native/core/instances/settings.cpp": ("reserved_resource_directory",),
    "native/core/tasks/tasks.hpp": ("commit_external",),
    "native/core/tasks/tasks.cpp": ("Task::commit_external",),
    "native/core/packs/packs.hpp": ("FinalPublish", "final_publish"),
    "native/core/packs/packs.cpp": ("final_publish",),
    "native/core/packs/native_services.hpp": ("final_publish",),
    "native/core/packs/native_services.cpp": ("final_publish",),
    "native/ui/DecodeImage.hpp": ("DecodeImage",),
    "native/app/core_adapters.hpp": ("network_configuration",),
}
PAIRED_IMPLEMENTATIONS = {
    "native/app/service/operation_scheduler.cpp":
        r"void\s+ApplicationState::schedule_operation\([^)]*std::shared_ptr<tasks::Task>\s+task",
    "native/core/config/path_guard.cpp":
        r"DirectoryGuard::DirectoryGuard\([^)]*DirectoryGuardMode",
}


def main() -> int:
    failures = []
    records = []
    raw = (SHARED / "snapshot.json").read_bytes()
    manifest = json.loads(raw.decode("utf-8-sig"))
    if manifest.get("member") != "app-instance-actions":
        failures.append("WRONG_SNAPSHOT_MEMBER")
    for relative, expected in manifest["files"].items():
        file = SHARED / relative
        if not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest() != expected["sha256"]:
            failures.append("HASH_MISMATCH " + relative)
    for relative, symbols in REQUIRED.items():
        file = SHARED / relative
        if relative not in manifest["files"] or not file.is_file():
            failures.append(f"NOT_SUPPLIED {relative}")
            continue
        content = file.read_bytes()
        records.append({"path": relative, "sha256": hashlib.sha256(content).hexdigest(), "symbols": symbols})
        for symbol in symbols:
            if symbol.encode("ascii") not in content:
                failures.append(f"STALE_INTERFACE {relative}: {symbol}")
    for relative, pattern in PAIRED_IMPLEMENTATIONS.items():
        file = SHARED / relative
        if relative not in manifest["files"] or not file.is_file():
            failures.append(f"NOT_SUPPLIED {relative}")
        elif not re.search(pattern, file.read_text(encoding="utf-8-sig")):
            failures.append("UNPAIRED_IMPLEMENTATION " + relative)
    output = {"kind": "interface-supply-only", "status": "BLOCKED" if failures else "READY_NOT_TESTED",
              "manifestSha256": hashlib.sha256(raw).hexdigest(), "createdUtc": manifest.get("createdUtc"),
              "files": records, "failures": failures, "productionAcceptance": False}
    print(json.dumps(output, ensure_ascii=True, indent=2))
    return 2 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
