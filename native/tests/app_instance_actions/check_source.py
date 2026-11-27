"""Small structural guard; a successful result is not a compiler or test PASS."""
from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "native/app/service/instance_actions.cpp"


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")
    token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/')
    stripped = token.sub("", text)
    stack = []
    closing = {"}": "{", ")": "(", "]": "["}
    for offset, char in enumerate(stripped):
        if char in "{([":
            stack.append((char, offset))
        elif char in closing:
            if not stack or stack[-1][0] != closing[char]:
                print(f"FAIL mismatched {char} at offset {offset}")
                return 1
            stack.pop()
    if stack:
        print(f"FAIL unclosed pairs: {stack}")
        return 1
    forbidden = ["unsafeFs", "productionTestServices", "DirectoryLock", "configure_native_services",
                 "commit_files({})", "std::thread", "detach()"]
    found = [item for item in forbidden if item in text]
    if found:
        print(f"FAIL forbidden production shortcuts: {found}")
        return 1
    actions = ["ActivateRoot", "AddRoot", "CreateRoot", "RenameRoot", "RemoveRoot", "DeleteRoot",
               "SelectInstance", "RenameInstance", "SetInstanceIcon", "SetInstanceFavorite",
               "DeleteInstance", "PatchVersionSettings", "ResetVersionSettings", "InstallVersion", "ImportPack"]
    missing = [action for action in actions if f"case ui::UiAction::{action}" not in text]
    if missing:
        print(f"FAIL missing branches: {missing}")
        return 1
    print("READY structural pairs/actions/shortcut guard only; compiler and domain tests still required")
    return 0


if __name__ == "__main__":
    sys.exit(main())
