"""Generate only public Microsoft build configuration; never inspect secrets files.

Without --input (or with an all-zero client id) the emitted header contains an empty
object, which keeps local builds working without client identifiers.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
import unicodedata
from collections.abc import Callable

MAX_SOURCE_BYTES = 256 * 1024
PUBLIC_ENVIRONMENT = {
    "clientId": "NICKEL_MSA_CLIENT_ID",
    "authority": "NICKEL_MSA_AUTHORITY",
    "redirectUri": "NICKEL_MSA_REDIRECT_URI",
}
FIELD_LIMITS = {"clientId": 256, "authority": 256, "redirectUri": 2048}


class PublicBuildConfigError(ValueError):
    pass


def _reject_constant(value: str):
    raise PublicBuildConfigError("Non-finite JSON values are not public build configuration")


def parse_source(text: str):
    try:
        size = len(text.encode("utf-8"))
    except UnicodeError:
        raise PublicBuildConfigError("Public build configuration must be valid UTF-8") from None
    if size > MAX_SOURCE_BYTES:
        raise PublicBuildConfigError("Public build configuration exceeds the source size limit")
    try:
        return json.loads(text, parse_constant=_reject_constant)
    except (json.JSONDecodeError, RecursionError):
        raise PublicBuildConfigError("Invalid public build configuration JSON") from None


def _field(section: dict, name: str) -> str | None:
    value = section.get(name)
    if value is None:
        return None
    if not isinstance(value, str):
        raise PublicBuildConfigError(f"Public Microsoft field {name} must be a string")
    value = value.strip()
    if not value:
        return None
    try:
        encoded = value.encode("utf-8")
    except UnicodeError:
        raise PublicBuildConfigError(f"Public Microsoft field {name} must be valid UTF-8") from None
    if len(encoded) > FIELD_LIMITS[name] or any(unicodedata.category(c) == "Cc" for c in value):
        raise PublicBuildConfigError(f"Public Microsoft field {name} exceeds its consumer contract")
    return value


def project(text: str, lookup: Callable[[str], str | None] | None = None) -> dict:
    value = parse_source(text)
    section = value.get("microsoft") if isinstance(value, dict) else None
    section = dict(section) if isinstance(section, dict) else {}
    if lookup is not None:
        for name, environment in PUBLIC_ENVIRONMENT.items():
            replacement = lookup(environment)
            if replacement is not None and replacement.strip():
                section[name] = replacement
    client = _field(section, "clientId")
    if client is None or client == "00000000-0000-0000-0000-000000000000":
        return {}
    return {"microsoft": {"clientId": client, "authority": _field(section, "authority") or "consumers",
                          "redirectUri": _field(section, "redirectUri") or "nickel://auth"}}


def header_for(configuration: dict) -> str:
    payload = json.dumps(configuration, ensure_ascii=True, sort_keys=True, separators=(",", ":"))
    literal = json.dumps(payload, ensure_ascii=True)
    return ("#pragma once\n\n#include <string_view>\n\nnamespace nickel::app::public_build {\n"
            f"inline constexpr std::string_view microsoft_json = {literal};\n"
            "} // namespace nickel::app::public_build\n")


def generate(output: Path, source: Path | None = None,
             lookup: Callable[[str], str | None] | None = None):
    text = "{}"
    if source is not None:
        try:
            with source.open("rb") as stream:
                data = stream.read(MAX_SOURCE_BYTES + 1)
        except OSError:
            raise PublicBuildConfigError("Unable to read the explicitly selected public build configuration") from None
        if len(data) > MAX_SOURCE_BYTES:
            raise PublicBuildConfigError("Public build configuration exceeds the source size limit")
        try:
            text = data.decode("utf-8")
        except UnicodeError:
            raise PublicBuildConfigError("Public build configuration must be valid UTF-8") from None
    content = header_for(project(text, lookup)).encode("ascii")
    if output.is_file() and output.read_bytes() == content:
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".public-build-", dir=output.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        os.replace(temporary, output)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, help="Explicit public-only JSON input; no default secrets lookup")
    parser.add_argument("--output", required=True, type=Path)
    options = parser.parse_args()
    try:
        generate(options.output, options.input, os.environ.get)
    except (PublicBuildConfigError, OSError) as error:
        message = str(error) if isinstance(error, PublicBuildConfigError) else "Unable to write public build configuration"
        print(message, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
