"""Compile only four read-only primitive sources plus one owned small C++ test."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
from pathlib import Path
import shutil
import sys

import run_suite
import trusted_export_tools as tools

HERE = Path(__file__).resolve().parent
OWNER = HERE.parents[2]
JSON_SHA = "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = run_suite.owned(args.output)
    tools.require(not output.exists(), "conflict", "Use a fresh raw evidence directory")
    run_suite.disk_capacity()
    output.mkdir(parents=True, exist_ok=False)
    record = {"schema": "pd.trusted-export.pathguard-run.v1", "scope": "foundation-primitives-only",
              "productionAppServicesTested": False, "actualPdRoundTripPass": False, "status": "FAIL",
              "startedUtc": dt.datetime.now(dt.UTC).isoformat(), "capacityBefore": run_suite.disk_capacity()}
    try:
        build = output / "compile"
        build.mkdir()
        msvc_path = OWNER / "scripts/native/msvc.py"
        spec = importlib.util.spec_from_file_location("owned_msvc_loader", msvc_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        env = module.developer_environment()
        isolated = run_suite.environment(build)
        for name in ("APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                     "TEMP", "TMP", "NO_COLOR", "PYTHON_COLORS", "PYTHONDONTWRITEBYTECODE", "QT_QPA_PLATFORM"):
            env[name] = isolated[name]
        for name in ("QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH"):
            env.pop(name, None)
        compiler = shutil.which("cl.exe", path=env["PATH"])
        tools.require(compiler is not None, "missing_service", "Installed MSVC compiler is unavailable")
        dependency = HERE / "runs/deps/nlohmann/json.hpp"
        tools.require(dependency.is_file() and hashlib.sha256(dependency.read_bytes()).hexdigest() == JSON_SHA,
                      "integrity", "Owned exact nlohmann 3.11.3 header is required")
        sources = [HERE / "pathguard_tests.cpp"] + [OWNER / "native/core/config" / name for name in
                                                   ("path_guard.cpp", "atomic_file.cpp", "crypto.cpp", "detail.cpp")]
        headers = [OWNER / "native/core/config" / name for name in
                   ("path_guard.hpp", "atomic_file.hpp", "detail.hpp", "windows_security.hpp", "conditional_file.hpp", "conditional_file_impl.inc")]
        headers += [OWNER / "native/core/common" / name for name in ("json.hpp", "error.hpp", "cancellation.hpp")]
        bound_paths = sources + headers + [dependency, msvc_path, Path(__file__).resolve()]
        bound = {str(path.relative_to(OWNER)): hashlib.sha256(path.read_bytes()).hexdigest() for path in bound_paths}
        exe = build / "pd_pathguard_small.exe"
        command = [compiler, "/nologo", "/std:c++20", "/MT", "/EHsc", "/O1", "/W4", "/permissive-", "/Zc:__cplusplus",
                   "/DUNICODE", "/D_UNICODE", "/I" + str(OWNER / "native"), "/I" + str(dependency.parents[1]),
                   *map(str, sources), "/Fe:" + str(exe), "/link", "/INCREMENTAL:NO", "bcrypt.lib", "advapi32.lib"]
        import subprocess
        record.update(sourceCommit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=OWNER, text=True).strip(),
                      sourceHashes=bound, jsonDependencySha256=JSON_SHA)
        record["compile"] = run_suite.log_command(command, build, env, 180)
        if record["compile"]["returnCode"] == 0:
            record["exe"] = tools.file_snapshot(exe, HERE)
            run_dir = output / "run"
            run_dir.mkdir()
            record["run"] = run_suite.log_command([str(exe), str(run_dir / "fixtures")], run_dir, env, 60)
            tools.unchanged(record["exe"], exe, HERE)
            if record["run"]["returnCode"] == 0:
                result = tools.strict_json((run_dir / "fixtures/result.json").read_bytes(), 1024 * 1024)
                record["cases"] = result["cases"]
                record["status"] = "PASS" if result["cases"] and all(case["status"] == "PASS" for case in result["cases"]) else "FAIL"
        after = {str(path.relative_to(OWNER)): hashlib.sha256(path.read_bytes()).hexdigest() for path in bound_paths}
        tools.require(after == bound, "source_changed", "Source bytes changed during the owned small compile/run")
        record["sourceRecheckPassed"] = True
        record["capacityAfter"] = run_suite.disk_capacity()
    except Exception as error:
        record["status"] = "FAIL"
        record["failure"] = {"code": getattr(error, "code", type(error).__name__), "detail": str(error)}
        import traceback
        with (output / "runner-error.log").open("x", encoding="utf-8") as log:
            traceback.print_exc(file=log)
    record["finishedUtc"] = dt.datetime.now(dt.UTC).isoformat()
    tools.write_new_json(output / "result.json", record)
    print({"status": record["status"], "scope": record["scope"], "result": str(output / "result.json"),
           "productionAppServicesTested": False, "actualPdRoundTripPass": False})
    return 0 if record["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
