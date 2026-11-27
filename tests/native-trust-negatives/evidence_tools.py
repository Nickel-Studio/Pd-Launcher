"""Bounded evidence orchestration using the frozen trusted-export oracle."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import types

HERE = Path(__file__).resolve().parent
WORKTREE = HERE.parents[1]
ORACLE_COMMIT = "ec2aa538c76ff0fca402c394d8a7d59975045285"
ORACLE_PATH = "native/tests/trusted_export/trusted_export_tools.py"
ORACLE_SHA256 = "f9e887ee005f0d32127db559da74e68a64026d3b04040679eea3a818b16d11a8"
SMALL_BUDGET = 16 * 1024 * 1024


def load_oracle():
    # Read the original immutable object in this worktree; never create a copy
    # under another owner's directory or silently substitute an unpinned parser.
    command = ["git", "show", ORACLE_COMMIT + ":" + ORACLE_PATH]
    result = subprocess.run(command, cwd=WORKTREE, capture_output=True, check=True,
                            timeout=20, shell=False)
    if len(result.stdout) > SMALL_BUDGET or hashlib.sha256(result.stdout).hexdigest() != ORACLE_SHA256:
        raise ValueError("Frozen trusted-export oracle bytes differ")
    module = types.ModuleType("pd_final_frozen_trusted_export_tools")
    module.__file__ = str(WORKTREE / ORACLE_PATH)
    sys.modules[module.__name__] = module
    exec(compile(result.stdout, module.__file__, "exec"), module.__dict__)
    return module


tools = load_oracle()


def owned(path: Path, must_exist: bool = False) -> Path:
    candidate = Path(os.path.abspath(path))
    tools.require(candidate == HERE or candidate.is_relative_to(HERE), "unauthorized",
                  "Outputs and fixture inputs must stay in native-trust-negatives")
    current = candidate
    while current != current.parent:
        if current.exists():
            info = current.lstat()
            tools.require(not current.is_symlink() and not getattr(info, "st_file_attributes", 0) & 0x400,
                          "unsafe_path", "Owned path contains a filesystem redirect")
        current = current.parent
    if must_exist:
        tools.require(candidate.exists(), "missing_service", "Owned input is absent")
    return candidate


def inventory() -> list[str]:
    document = tools.strict_json((HERE / "required_cases.json").read_bytes())
    tools.require(document.get("schema") == "pd.trust-negatives.inventory.v1", "invalid_schema",
                  "Unknown case inventory")
    return document["publicNegative"] + document["requiresIndependentBranchOrActualPd"]


def final_inventory(cases: list[dict]) -> dict:
    for case in cases:
        tools.require(isinstance(case, dict), "invalid_schema", "Case must be an object")
        if case.get("status") == "NOT_RUN":
            tools.require(case.get("executed") is False and case.get("assertions") == 0
                          and isinstance(case.get("reason"), str) and bool(case["reason"]),
                          "invalid_evidence", "NOT_RUN must not claim executed assertions")
        if case.get("status") == "PASS" and str(case.get("id", "")).startswith("public-"):
            proof = case.get("evidence", {})
            tools.require(proof.get("actualAppServices") is True and proof.get("includeLauncher") is True,
                          "invalid_evidence", "Public negative must use actual AppServices and includeLauncher=true")
            tools.require(proof.get("destinationExists") is False and proof.get("productionReceiptUsed") is False,
                          "invalid_evidence", "Untrusted public negative must not publish or mint a receipt")
    result = tools.aggregate(cases, inventory())
    result.update({"schema": "pd.trust-negatives.final-audit.v1", "scope": "negative-evidence-audit-only",
                   "productAccepted": False, "actualPdRoundTripPass": False,
                   "oracleCommit": ORACLE_COMMIT, "oracleSha256": ORACLE_SHA256})
    return result


def audit_result(result: dict, graph: dict, graph_sha256: str, source_commit: str, source_sha256: str) -> dict:
    tools.require(tools.HEX40.fullmatch(source_commit) is not None
                  and tools.HEX64.fullmatch(source_sha256) is not None
                  and tools.HEX64.fullmatch(graph_sha256) is not None, "invalid_schema", "Full generation pins are required")
    tools.require(graph.get("schema") == "pd.trusted-export.graph.v1"
                  and graph.get("sourceCommit") == source_commit
                  and graph.get("sourceSha256") == source_sha256
                  and graph.get("appServicesImported") is False, "source_changed",
                  "Graph is not the same pinned non-imported production generation")
    sources = graph.get("appServicesSources", [])
    tools.require(isinstance(sources, list) and all(isinstance(item, str) for item in sources),
                  "invalid_schema", "Graph source list is invalid")
    names = {item.replace("\\", "/").split("/")[-1] for item in sources}
    tools.require({"AppServices.cpp", "application_state.cpp", "resource_actions.cpp", "trusted_launcher.cpp"} <= names,
                  "missing_service", "Graph omits the actual public service/factory/consumer sources")
    tools.require(result.get("schema") == "pd.trust-negatives.final.v1"
                  and result.get("scope") == "production-appservices-negative-only"
                  and result.get("productAccepted") is False, "invalid_evidence",
                  "Result is not an independent production negative receipt")
    tools.require(result.get("sourceCommit") == source_commit and result.get("sourceSha256") == source_sha256
                  and result.get("graphSha256") == graph_sha256, "source_changed", "Runtime and root graph pins differ")
    audit = final_inventory(result.get("cases", []))
    tools.require(result.get("status") == audit["status"], "invalid_evidence", "Runtime aggregate hides FAIL or NOT_RUN")
    audit["sourceCommit"] = source_commit
    audit["sourceSha256"] = source_sha256
    audit["graphSha256"] = graph_sha256
    audit["sameGenerationReceiptPinsMatch"] = True
    audit["productionLinkClosureIndependentlyProved"] = False
    return audit


def isolated_environment(directory: Path) -> dict[str, str]:
    directory = owned(directory, True)
    result = dict(os.environ)
    for name in ("APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "TEMP", "TMP"):
        path = directory / "environment" / name.lower()
        owned(path)
        path.mkdir(parents=True, exist_ok=False)
        result[name] = str(path)
    result.update({"PYTHONDONTWRITEBYTECODE": "1", "PYTHON_COLORS": "0", "NO_COLOR": "1",
                   "QT_QPA_PLATFORM": "offscreen"})
    for name in ("QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH"):
        result.pop(name, None)
    return result


def exit_code(result: dict) -> int:
    return 1 if result["status"] == "FAIL" else 77 if result["status"] == "NOT_RUN" else 0
