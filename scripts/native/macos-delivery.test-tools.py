#!/usr/bin/env python3
"""Pinned, isolated Python golden-fixture tools; never an application dependency."""
from __future__ import annotations

from pathlib import Path
from email.parser import BytesParser
import json
import re
import shutil
import stat
import urllib.parse
import zipfile

INPUT_SHA256 = "ddaa89a3aa8e845ece43096c0063d75d08dfcfea2db12824ac36af0432ce2874"
VERSIONS = {"cryptography": "48.0.1", "cffi": "2.1.1", "pycparser": "2.23"}


def canonical_requirement(value: str) -> str:
    """Canonical requirement text for exact closure comparison.

    METADATA permits either quote style around marker string literals; the reviewed
    pins and the published wheels legitimately differ only there. Collapse whitespace
    and treat both quote characters as one canonical quote, while leaving every name,
    version bound, extra and marker operator untouched.
    """
    return re.sub(r"\s+", "", value).replace("'", '"')


def reviewed_closure(values) -> list[str]:
    return sorted(canonical_requirement(item) for item in values)


def read_pins(path: Path, api) -> dict:
    if api.digest(path) != INPUT_SHA256:
        raise api.DeliveryError("Independent fixture tool input hash changed")
    value = api.small_json(path)
    if value.get("schema") != 1 or value.get("kind") != "owner-scoped-official-fixture-python-inputs":
        raise api.DeliveryError("Independent fixture tool input schema mismatch")
    return value


def select_wheels(value: dict, architecture: str, python_minor: int, implementation: str, api) -> list[dict]:
    if architecture not in api.ARCHITECTURES or implementation != "CPython" or python_minor not in value.get("supportedPythonMinors", []):
        raise api.DeliveryError("No exact independent fixture wheels for this native CPython interpreter")
    selected = [item for item in value.get("sources", []) if architecture in item.get("architectures", []) and python_minor in item.get("pythonMinors", [])]
    if len(selected) != 3 or {item.get("name") for item in selected} != set(VERSIONS):
        raise api.DeliveryError("The complete independent fixture wheel set must be unique")
    for item in selected:
        url = urllib.parse.urlsplit(item["url"])
        if (item.get("version") != VERSIONS[item["name"]] or url.scheme != "https" or url.netloc != "files.pythonhosted.org"
                or url.query or url.fragment or url.path.rsplit("/", 1)[-1] != item["archive"]
                or item.get("role") != "independent-test-fixture-tool-not-product-dependency"):
            raise api.DeliveryError("Independent fixture wheel is not an exact official pin")
        if api.safe_relative(item["archive"]).name != item["archive"] or not item["archive"].endswith(".whl"):
            raise api.DeliveryError("Fixture wheel archive must be a single filename")
        api.require_sha(item["sha256"])
        if not isinstance(item.get("bytes"), int) or not 0 < item["bytes"] < 16 * 1024 * 1024:
            raise api.DeliveryError("Fixture wheel byte bound is missing")
    return sorted(selected, key=lambda item: item["name"])


def validate_interpreter(description: dict, architecture: str, api) -> None:
    if (architecture not in api.ARCHITECTURES or description.get("implementation") != "CPython"
            or description.get("major") != 3 or description.get("minor") != 12
            or description.get("architecture") != architecture or description.get("freeThreaded") is not False):
        raise api.DeliveryError("Independent fixture interpreter is not native regular CPython 3.12")


def unpack_wheel(archive: Path, entry: dict, site: Path, api) -> dict:
    if archive.is_symlink() or not archive.is_file() or archive.stat().st_size != entry["bytes"] or api.digest(archive) != entry["sha256"]:
        raise api.DeliveryError("Independent fixture wheel bytes must be verified before extraction")
    if site.is_symlink() or not site.is_dir():
        raise api.DeliveryError("Actual isolated fixture site directory required")
    with zipfile.ZipFile(archive) as wheel:
        members = wheel.infolist()
        if len(members) > 4000 or sum(member.file_size for member in members) > 64 * 1024 * 1024:
            raise api.DeliveryError("Independent fixture wheel expansion exceeds its bound")
        names = set()
        for member in members:
            relative = api.safe_relative(member.filename.rstrip("/"))
            identity = relative.as_posix().casefold()
            mode = stat.S_IFMT(member.external_attr >> 16)
            if (identity in names or mode not in (0, stat.S_IFREG, stat.S_IFDIR)
                    or relative.parts[0].casefold().endswith(".data") or relative.suffix.casefold() == ".pth"
                    or relative.name.casefold() in ("sitecustomize.py", "usercustomize.py")):
                raise api.DeliveryError("Linked, duplicate, startup-hook or relocating fixture wheel member rejected")
            names.add(identity)
            destination = site / relative.as_posix()
            for parent in [destination, *destination.parents]:
                if parent == site.parent:
                    break
                if parent.is_symlink() or (parent.exists() and getattr(parent.lstat(), "st_file_attributes", 0) & 0x400):
                    raise api.DeliveryError("Linked fixture wheel destination rejected")
            if not api.within(destination, site) or (not member.is_dir() and destination.exists()):
                raise api.DeliveryError("Fixture wheel member escapes or overwrites the isolated environment")
        metadata_members = [member for member in members if member.filename.endswith(".dist-info/METADATA")]
        expected_metadata = entry["name"] + "-" + entry["version"] + ".dist-info/METADATA"
        if len(metadata_members) != 1 or metadata_members[0].filename != expected_metadata:
            raise api.DeliveryError("Fixture wheel must contain exactly one correctly named actual METADATA")
        if metadata_members[0].file_size > 1024 * 1024:
            raise api.DeliveryError("Actual fixture wheel METADATA exceeds its bound")
        metadata = BytesParser().parsebytes(wheel.read(metadata_members[0]))
        if metadata.get_all("Name", []) != [entry["name"]] or metadata.get_all("Version", []) != [entry["version"]]:
            raise api.DeliveryError("Actual fixture wheel METADATA differs from its exact pin")
        requirements = metadata.get_all("Requires-Dist", [])
        reviewed = {
            "cryptography": ['cffi>=2.0.0; platform_python_implementation != "PyPy"',
                             'typing-extensions>=4.13.2; python_full_version < "3.11"',
                             'bcrypt>=3.1.5; extra == "ssh"'],
            "cffi": ['pycparser; implementation_name != "PyPy"'],
            "pycparser": [],
        }
        if reviewed_closure(requirements) != reviewed_closure(reviewed[entry["name"]]):
            raise api.DeliveryError("Actual fixture wheel dependency METADATA violates the reviewed CPython 3.12 closure")
        if wheel.testzip() is not None:
            raise api.DeliveryError("Independent fixture wheel CRC mismatch")
        wheel.extractall(site)
        return {"name": entry["name"], "version": metadata.get("Version"), "metadataPath": metadata_members[0].filename,
                "requiresPython": metadata.get("Requires-Python"), "requiresDist": requirements}


def install(architecture: str, root: Path, downloads: Path, output: Path, policy: dict, api) -> dict:
    environment, host = api.apple_environment(policy, architecture)
    root = root.absolute()
    pins = read_pins(Path(__file__).with_suffix(".json"), api)
    if root.exists() or root.is_symlink():
        raise api.DeliveryError("Independent fixture environment must be fresh and isolated")
    recorder = api.Recorder(output, {"kind": "isolated-independent-fixture-python", "host": host,
                                   "architecture": architecture, "inputPinsSha256": INPUT_SHA256,
                                   "productRuntimeDependency": False})
    try:
        interpreter = shutil.which("python3.12", path=environment.get("PATH"))
        if not interpreter:
            raise api.DeliveryError("An actual native regular CPython 3.12 interpreter is required")
        probe = "import json,platform,sys,sysconfig; print(json.dumps({'implementation':platform.python_implementation(),'minor':sys.version_info.minor,'major':sys.version_info.major,'architecture':platform.machine(),'freeThreaded':bool(sysconfig.get_config_var('Py_GIL_DISABLED')),'version':sys.version}))"
        description = json.loads(recorder.run([interpreter, "-I", "-B", "-c", probe], root.parent, environment, 30))
        validate_interpreter(description, architecture, api)
        selected = select_wheels(pins, architecture, 12, description["implementation"], api)
        records = [api.https_download(entry, downloads) for entry in selected]
        recorder.run([interpreter, "-I", "-B", "-m", "venv", "--without-pip", str(root)], root.parent, environment, 60)
        python = root / "bin/python"
        site = root / "lib/python3.12/site-packages"
        if not python.is_file() or not site.is_dir():
            raise api.DeliveryError("Actual isolated fixture interpreter/site directory missing")
        metadata = [unpack_wheel(downloads / entry["archive"], entry, site, api) for entry in selected]
        for variable in ("PYTHONHOME", "PYTHONPATH", "PYTHONUSERBASE"):
            environment.pop(variable, None)
        environment.update(PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1")
        probe = ("import importlib.metadata as m,json,pathlib,sys; "
                 "from cryptography.hazmat.primitives.ciphers.aead import AESGCM; "
                 "from cryptography.hazmat.bindings import _rust; "
                 "import cryptography,cffi,pycparser,_cffi_backend; "
                 "assert AESGCM(bytes(32)).encrypt(bytes(12),b'',None).hex()=='530f8afbc74536b9a963b4f1c4cb738b'; "
                 "versions={n:m.version(n) for n in ['cryptography','cffi','pycparser']}; "
                 "assert versions=={'cryptography':'48.0.1','cffi':'2.1.1','pycparser':'2.23'}; "
                 "paths=[pathlib.Path(x.__file__).resolve() for x in [cryptography,cffi,pycparser,_cffi_backend,_rust]]; "
                 "assert all(p.is_relative_to(pathlib.Path(sys.prefix).resolve()) for p in paths); "
                 "print(json.dumps({'versions':versions,'modulePaths':[str(p) for p in paths],'interpreter':sys.version,'prefix':sys.prefix,'nistAes256GcmEmptyVerified':True}))")
        observed = json.loads(recorder.run([str(python), "-I", "-B", "-c", probe], root, environment, 30))
        files = []
        for path in sorted(site.rglob("*")):
            if path.is_symlink():
                raise api.DeliveryError("Linked installed fixture package rejected")
            if path.is_file():
                files.append({"path": path.relative_to(site).as_posix(), "bytes": path.stat().st_size, "sha256": api.digest(path)})
        manifest = output.parent / "test-tools-prefix-manifest.json"
        api.write_json(manifest, {"schema": 1, "architecture": architecture, "inputPinsSha256": INPUT_SHA256, "files": files})
        api.verify_file_manifest(site, manifest, api.digest(manifest), "Independent fixture environment")
        recorder.finish(environmentRoot=str(root), python=str(python), pythonSha256=api.digest(python),
                        sitePackages=str(site), prefixManifest=str(manifest), prefixManifestSha256=api.digest(manifest),
                        downloads=records, actualWheelMetadata=metadata, sourceInterpreter=description,
                        observedInterpreter=observed, productRuntimeDependency=False,
                        appleRuntimeExecuted=False, productDelivered=False)
    except Exception as exception:
        recorder.record["failure"] = str(exception)
        recorder.save()
        raise
    return recorder.record


def verify_environment(evidence: Path, architecture: str, api) -> Path:
    value = api.small_json(evidence)
    if (not value.get("complete") or value.get("kind") != "isolated-independent-fixture-python"
            or value.get("inputPinsSha256") != INPUT_SHA256 or value.get("architecture") != architecture
            or value.get("productRuntimeDependency") is not False):
        raise api.DeliveryError("Actual pinned independent fixture environment evidence missing")
    validate_interpreter(value.get("sourceInterpreter", {}), architecture, api)
    root = Path(value["environmentRoot"])
    python = Path(value["python"])
    if not root.is_absolute() or root.is_symlink() or python != root / "bin/python" or api.digest(python) != api.require_sha(value["pythonSha256"]):
        raise api.DeliveryError("Independent fixture interpreter changed")
    site = Path(value["sitePackages"])
    if site != root / "lib/python3.12/site-packages" or site.is_symlink() or not api.within(site, root):
        raise api.DeliveryError("Independent fixture package prefix escapes its environment")
    verified = api.verify_file_manifest(site, Path(value["prefixManifest"]), value["prefixManifestSha256"], "Independent fixture environment")
    if verified["data"].get("architecture") != architecture or verified["data"].get("inputPinsSha256") != INPUT_SHA256:
        raise api.DeliveryError("Independent fixture manifest identity changed")
    actual = set()
    for path in site.rglob("*"):
        if path.is_symlink():
            raise api.DeliveryError("Linked installed fixture package rejected")
        if path.is_file():
            actual.add(path.relative_to(site).as_posix())
    if actual != set(verified["paths"]):
        raise api.DeliveryError("Unlisted installed fixture package files rejected")
    # Preserve the venv symlink path; resolving it would bypass its isolated site packages.
    return python
