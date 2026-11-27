import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


RESERVE = 16 * 1024 * 1024


def require_space(directory):
    if shutil.disk_usage(directory).free < RESERVE:
        raise RuntimeError("Insufficient available space before transaction fixture writes")


def execute(executable, arguments, expected=0):
    result = subprocess.run([str(executable), *arguments], timeout=60, check=False)
    if result.returncode != expected:
        raise RuntimeError(f"Transaction process returned {result.returncode}; expected {expected}: {arguments}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    workspace = args.workspace.resolve()
    require_space(workspace.parent)
    workspace.mkdir(exist_ok=True)
    execute(executable, ["--run"])
    points = [("files.planned", 1), ("files.backed_up", 1), ("files.published", 1),
              ("files.backed_up", 2), ("files.published", 2), ("files.committed", 1), ("cleanup", 1)]
    cases = 0
    for renamed in (False, True):
        checkpoints = points + ([("files.retained", 1)] if renamed else [])
        for point, ordinal in checkpoints:
            require_space(workspace)
            with tempfile.TemporaryDirectory(prefix="community-transaction-interruption-", dir=workspace) as temporary:
                root = str(Path(temporary))
                print(f"START real process interruption {point}:{ordinal}, renamed={renamed}, root characters={len(root)}", flush=True)
                execute(executable, ["--crash", root, point, str(ordinal), str(int(renamed))], expected=86)
                execute(executable, ["--recover", root, str(int(renamed)),
                                    str(int(point in ("files.committed", "cleanup"))), "0"])
                cases += 1
    for renamed in (False, True):
        require_space(workspace)
        with tempfile.TemporaryDirectory(prefix="community-transaction-conflict-", dir=workspace) as temporary:
            root = str(Path(temporary))
            execute(executable, ["--crash", root, "files.published", "1", str(int(renamed))], expected=86)
            execute(executable, ["--recover", root, str(int(renamed)), "0", "1"])
            cases += 1
    print(f"PASS {cases} real process interruption/recovery/conflict cases", flush=True)


if __name__ == "__main__":
    main()
