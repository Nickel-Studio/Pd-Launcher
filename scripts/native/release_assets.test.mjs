import assert from "node:assert/strict";
import test from "node:test";
import { identifyNativeAsset, nativeAssetName, nativeReceiptName, nativeVersion, releaseAssetContract } from "./release_assets.mjs";

const version = "0.1.0";

test("native tags are distinct from historical Ni v tags", () => {
  assert.equal(nativeVersion("pd-v0.1.0"), version);
  for (const tag of [null, "v0.1.0", "Pd-v0.1.0", "pd-v00.1.0", "pd-v0.1.0-rc1", "pd-v0.1.0/other", "pd-v0.1.0\n"]) {
    assert.equal(nativeVersion(tag), null);
  }
});

test("every native public program asset has an exact architecture and real format", () => {
  for (const [format, target] of Object.entries(releaseAssetContract.formats)) {
    const name = nativeAssetName(version, format);
    assert.deepEqual(identifyNativeAsset(name, version), { format, ...target });
    assert.ok(name.includes(`-${target.platform}-${target.architecture}${target.suffix}`));
  }
  assert.equal(nativeAssetName(version, "windows-exe"), "Palladium-0.1.0-windows-x64.exe");
  assert.equal(nativeAssetName(version, "macos-arm64-zip"), "Palladium-0.1.0-macos-arm64.zip");
  assert.equal(nativeAssetName(version, "linux-tar-gz"), "Palladium-0.1.0-linux-x64.tar.gz");
});

test("bare, historical, wrong-version, mixed-architecture and material assets are not program downloads", () => {
  for (const name of ["pd-launcher.exe", "Nickel_0.1.0_x64-setup.exe", "Palladium-1.0.1-windows-x64.exe",
    "Palladium-0.1.0-windows-x64-arm64.exe", "Palladium-0.1.0-linux-arm64.AppImage",
    "Palladium-0.1.0-macos-arm64-source.zip", "Palladium-0.1.0-windows-x64-relink.zip",
    "Palladium-0.1.0-linux-x64-legal.zip", "../Palladium-0.1.0-windows-x64.exe",
    "palladium-0.1.0-windows-x64.exe", "Palladium-0.1.0-windows-x64.exe\n"]) {
    assert.equal(identifyNativeAsset(name, version), null, name);
  }
});

test("a real Apple ZIP is never advertised as a DMG or a Linux archive as an AppImage", () => {
  assert.equal(identifyNativeAsset("Palladium-0.1.0-macos-x64.zip", version).format, "macos-x64-zip");
  assert.equal(identifyNativeAsset("Palladium-0.1.0-linux-x64.tar.gz", version).format, "linux-tar-gz");
  assert.equal(nativeReceiptName(version), "Palladium-0.1.0-release.json");
  assert.throws(() => nativeAssetName("../0.1.0", "windows-exe"));
  assert.throws(() => nativeAssetName(version, "linux-arm64"));
});
