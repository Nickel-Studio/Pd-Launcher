import { readFileSync } from "node:fs";

export const releaseAssetContract = JSON.parse(readFileSync(new URL("./release_asset_contract.json", import.meta.url), "utf8"));
const versionPattern = /^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$/;

export function nativeVersion(tag) {
  if (typeof tag !== "string" || !tag.startsWith(releaseAssetContract.tagPrefix)) return null;
  const version = tag.slice(releaseAssetContract.tagPrefix.length);
  return versionPattern.test(version) ? version : null;
}

export function nativeAssetName(version, format) {
  const target = releaseAssetContract.formats[format];
  if (!versionPattern.test(version) || !target) throw new Error("Expected a stable native version and a known platform format");
  return `${releaseAssetContract.assetPrefix}-${version}-${target.platform}-${target.architecture}${target.suffix}`;
}

export function identifyNativeAsset(name, version) {
  if (typeof name !== "string" || !versionPattern.test(version)) return null;
  for (const [format, target] of Object.entries(releaseAssetContract.formats)) {
    if (name === nativeAssetName(version, format)) return { format, ...target };
  }
  return null;
}

export function nativeReceiptName(version) {
  if (!versionPattern.test(version)) throw new Error("Expected a stable native version");
  return `${releaseAssetContract.assetPrefix}-${version}-release.json`;
}
