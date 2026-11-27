import argparse
import hashlib
import io
import json
import pathlib
import subprocess
import sys
import threading
import time
import urllib.parse
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def encoded(value):
    return json.dumps(value, separators=(",", ":")).encode()


def zipped(entries):
    target = io.BytesIO()
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return target.getvalue()


def zipped_pairs(entries):
    target = io.BytesIO()
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries:
            archive.writestr(name, data)
    return target.getvalue()


class Fixture:
    def __init__(self, mock):
        self.mock = pathlib.Path(mock).read_bytes()
        self.files = {}
        self.base = ""

    def descriptor(self, name, data):
        self.files[name] = data
        return {"url": self.base + name, "sha1": hashlib.sha1(data).hexdigest(), "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}

    def prepare(self, base):
        self.base = base
        header = b"\xca\xfe\xba\xbe\0\0\0\x34"
        self.files["/hello.txt"] = b"controlled-body"
        self.files["/invalid.jar"] = b"<html>not a java archive</html>"
        self.files["/traversal.zip"] = zipped({"../escape.txt": b"unsafe"})
        # Exact duplicate names are unsafe on every filesystem. Case-distinct names are a
        # separate, platform-specific policy covered by the transport archive tests.
        self.files["/duplicate.zip"] = zipped_pairs([("a.txt", b"first"), ("a.txt", b"second")])
        self.descriptor("/native.jar", zipped({"META-INF/MANIFEST.MF": b"Manifest-Version: 1.0\n", "lwjgl.dll": b"controlled-native"}))
        self.descriptor("/runtime.zip", zipped({"controlled-jre/bin/java.exe": self.mock, "controlled-jre/release": b"JAVA_VERSION=17.0.1"}))
        client = self.descriptor("/client.jar", zipped({"net/minecraft/client/main/Main.class": header}))
        common = self.descriptor("/regular.jar", zipped({"test/Regular.class": header}))
        self.descriptor("/launchwrapper.jar", zipped({"net/minecraft/launchwrapper/Launch.class": header}))
        self.descriptor("/liteloader.jar", zipped({"com/mumfrey/liteloader/launch/LiteLoaderTweaker.class": header}))
        self.descriptor("/fabric-api.jar", zipped({"net/fabricmc/api/ModInitializer.class": header}))
        self.descriptor("/optifabric.jar", zipped({"me/modmuss50/optifabric/mod/OptifabricSetup.class": header}))
        asset_data = b"controlled-asset"
        asset_hash = hashlib.sha1(asset_data).hexdigest()
        self.files["/assets/" + asset_hash[:2] + "/" + asset_hash] = asset_data
        index = {"virtual": True, "map_to_resources": True, "objects": {"sounds/test.ogg": {"hash": asset_hash, "size": len(asset_data)}}}
        index_descriptor = self.descriptor("/index.json", encoded(index))
        index_descriptor["id"] = "controlled"
        profile = {"id": "1.20.1", "type": "release", "mainClass": "net.minecraft.client.main.Main", "downloads": {"client": client}, "assetIndex": index_descriptor,
                   "javaVersion": {"component": "java-runtime-gamma", "majorVersion": 17},
                   "libraries": [{"name": "test:regular:1", "downloads": {"artifact": common}}, {"name": "org.lwjgl:lwjgl:3.3.1:natives-windows", "downloads": {"artifact": self.describe("/native.jar")}}],
                   "arguments": {"game": ["--username", "${auth_player_name}", "--uuid", "${auth_uuid}", "--accessToken", "${auth_access_token}"]}}
        runtime = {"files": {"bin": {"type": "directory"}, "bin/java.exe": {"type": "file", "executable": True, "downloads": {"raw": self.descriptor("/java.exe", self.mock)}}}}
        runtime_descriptor = self.descriptor("/java-manifest.json", encoded(runtime))
        self.files["/all.json"] = encoded({"windows-x64": {"java-runtime-gamma": [{"version": {"name": "17.0.1"}, "manifest": runtime_descriptor}]}})
        profile["downloads"]["client_mappings"] = self.descriptor("/mappings.txt", b"processor-result")
        self.files["/profile.json"] = encoded(profile)
        old = json.loads(encoded(profile))
        old["id"] = "1.12.2"
        old["javaVersion"] = {"component": "jre-legacy", "majorVersion": 8}
        self.files["/old-profile.json"] = encoded(old)
        processor_jar = zipped({"META-INF/MANIFEST.MF": b"Manifest-Version: 1.0\nMain-Class: example.Processor\n\n", "example/Processor.class": header})
        generated_jar = zipped({"example/LoaderMain.class": header})
        self.descriptor("/generated.jar", generated_jar)
        for minecraft in ("1.20.1", "1.12.2"):
            for kind in ("forge", "neoforge"):
                coordinate = "test:processor:1"
                mapping_coordinate = "net.minecraftforge:installertools:1" if kind == "forge" else "net.neoforged.installertools:installertools:1"
                output = "[test:generated:1]"
                generated_hash = hashlib.sha1(generated_jar).hexdigest()
                manifest = {"json": "/version.json", "libraries": [{"name": coordinate, "downloads": {"artifact": {"url": "", "path": "test/processor/1/processor-1.jar", "sha1": hashlib.sha1(processor_jar).hexdigest()}}}],
                            "processors": [{"jar": coordinate, "args": ["--output", output, "--copy", "{LIBRARY_DIR}/test/generated-fixture/1/generated-fixture-1.jar"], "outputs": {output: generated_hash}},
                                           {"jar": coordinate, "sides": ["server"], "args": ["--exit", "45"]}]}
                layer = {"id": minecraft + "-" + kind, "inheritsFrom": minecraft, "mainClass": "net.minecraft.launchwrapper.Launch" if minecraft == "1.12.2" else "example.LoaderMain",
                         "libraries": [{"name": "test:generated:1", "downloads": {"artifact": {"url": "", "sha1": generated_hash}}}]}
                embedded = {"maven/test/processor/1/processor-1.jar": processor_jar, "maven/test/generated-fixture/1/generated-fixture-1.jar": generated_jar}
                self.descriptor("/" + minecraft + "-" + kind + "-installer.jar", zipped({"install_profile.json": encoded(manifest), "version.json": encoded(layer), **embedded}))
                for scenario in ("bad-hash", "nonzero", "slow", "mapping"):
                    changed = json.loads(encoded(manifest))
                    changed_layer = json.loads(encoded(layer))
                    processor = changed["processors"][0]
                    if scenario == "bad-hash":
                        processor["outputs"][output] = "0" * 40
                    elif scenario == "nonzero":
                        processor["args"] += ["--exit", "7"]
                    elif scenario == "slow":
                        processor["args"] += ["--sleep", "10000"]
                    else:
                        mapping_output = "[test:mappings:1@txt]"
                        processor["jar"] = mapping_coordinate
                        processor["args"] = ["--task", "DOWNLOAD_MOJMAPS", "--version", minecraft, "--side", "client", "--output", mapping_output]
                        processor["outputs"] = {mapping_output: hashlib.sha1(b"processor-result").hexdigest()}
                        changed_layer["libraries"] = [{"name": "test:generated:1", "downloads": {"artifact": self.describe("/generated.jar")}}]
                        parts = mapping_coordinate.split(":")
                        mapping_path = parts[0].replace(".", "/") + "/" + parts[1] + "/" + parts[2] + "/" + parts[1] + "-" + parts[2] + ".jar"
                        changed["libraries"].append({"name": mapping_coordinate, "downloads": {"artifact": {"url": "", "path": mapping_path, "sha1": hashlib.sha1(processor_jar).hexdigest()}}})
                    entries = {"install_profile.json": encoded(changed), "version.json": encoded(changed_layer), **embedded}
                    if scenario == "mapping":
                        entries["maven/" + mapping_path] = processor_jar
                    self.descriptor("/" + minecraft + "-" + kind + "-" + scenario + ".jar", zipped(entries))
        for kind in ("fabric", "quilt"):
            main = "net.fabricmc.loader.impl.launch.knot.KnotClient" if kind == "fabric" else "org.quiltmc.loader.impl.launch.knot.KnotClient"
            self.files["/" + kind + "-profile.json"] = encoded({"id": "1.20.1-" + kind, "inheritsFrom": "1.20.1", "mainClass": main, "libraries": []})
        self.files["/liteloader-profile.json"] = encoded({"id": "1.12.2-LiteLoader", "inheritsFrom": "1.12.2", "mainClass": "net.minecraft.launchwrapper.Launch",
            "libraries": [{"name": "com.mumfrey:liteloader:1.12.2-SNAPSHOT-r0", "downloads": {"artifact": self.describe("/liteloader.jar")}},
                          {"name": "net.minecraft:launchwrapper:1.12", "downloads": {"artifact": self.describe("/launchwrapper.jar")}}],
            "arguments": {"game": ["--tweakClass", "com.mumfrey.liteloader.launch.LiteLoaderTweaker"]}})
        for minecraft, patch in (("1.12.2", "G5"), ("1.20.1", "I6")):
            self.descriptor("/OptiFine_" + minecraft + "_HD_U_" + patch + ".jar", zipped({"optifine/OptiFineTweaker.class": header, "optifine/Installer.class": header}))
        self.files["/inventory.json"] = encoded({name: self.describe(name) for name in self.files if not name.endswith(".json")})

    def describe(self, name):
        data = self.files[name]
        return {"url": self.base + name, "sha1": hashlib.sha1(data).hexdigest(), "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionError, OSError):
            pass

    def do_GET(self):
        self.respond(False)

    def do_HEAD(self):
        self.respond(True)

    def respond(self, head):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/fail":
            self.send_response(503)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/slow.bin":
            self.send_response(200)
            self.send_header("Content-Length", str(4 * 1024 * 1024))
            self.send_header("ETag", '"controlled-slow"')
            self.end_headers()
            if not head:
                try:
                    for _ in range(1024):
                        self.wfile.write(b"s" * 4096)
                        self.wfile.flush()
                        time.sleep(0.02)
                except (ConnectionError, OSError):
                    pass
            return
        if path in ("/snapshot", "/snapshot-slow"):
            if path == "/snapshot-slow":
                time.sleep(0.15)
            data = self.headers.get("User-Agent", "").encode("ascii")
        else:
            data = self.server.fixture.files.get(path)
        if data is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("ETag", '"' + hashlib.sha256(data).hexdigest() + '"')
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        if not head:
            try:
                self.wfile.write(data)
            except (ConnectionError, OSError):
                pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mock", required=True)
    parser.add_argument("--test", required=True)
    parser.add_argument("--case", default="all")
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server.fixture = Fixture(args.mock)
    base = "http://127.0.0.1:" + str(server.server_address[1])
    server.fixture.prepare(base)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run([args.test, base, str(pathlib.Path(args.mock).resolve()), args.case], timeout=120, check=False)
        return result.returncode
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    sys.exit(main())
