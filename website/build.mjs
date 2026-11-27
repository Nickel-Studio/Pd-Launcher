import { copyFile, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const source = dirname(fileURLToPath(import.meta.url));
const root = resolve(source, "..");
const output = resolve(root, "_site");

export function repositoryName(repositoryUrl) {
  const url = new URL(repositoryUrl.replace(/\.git$/, ""));
  const parts = url.pathname.split("/").filter(Boolean);
  if (url.protocol !== "https:" || url.hostname !== "github.com" || parts.length !== 2 || !parts.every((part) => /^[\w.-]+$/.test(part))) {
    throw new Error("Expected a GitHub repository URL");
  }
  return parts.join("/");
}

export function siteUrl(repository, override) {
  const [owner, name] = repository.split("/");
  const url = new URL(override || `https://${owner.toLowerCase()}.github.io/${name}/`);
  if (url.protocol !== "https:" || url.search || url.hash || url.username || url.password) {
    throw new Error("Expected an HTTPS Pages base URL without query or fragment");
  }
  return `${url.href.replace(/\/+$/, "")}/`;
}

export function normalizeRelease(release, repository) {
  const repositoryUrl = `https://github.com/${repository}`;
  const fallback = { tag: null, url: `${repositoryUrl}/releases/latest`, publishedAt: null, downloads: {} };
  if (!release || typeof release.tag_name !== "string" || !Array.isArray(release.assets) || release.draft || release.prerelease) return fallback;
  function trustedUrl(value) {
    try {
      const url = new URL(value);
      return url.protocol === "https:" && url.hostname === "github.com" && !url.username && !url.password && url.pathname.startsWith(`/${repository}/releases/`) ? url.href : null;
    } catch { return null; }
  }
  const url = trustedUrl(release.html_url);
  if (!url) return fallback;
  const assets = release.assets.filter((asset) => typeof asset.name === "string" && trustedUrl(asset.browser_download_url));
  const x64 = (name) => /(?:x64|amd64|x86_64)/i.test(name) && !/(?:arm64|aarch64)/i.test(name);
  const formats = {
    "windows-exe": (name) => /\.exe$/i.test(name) && x64(name),
    "windows-msi": (name) => /\.msi$/i.test(name) && x64(name),
    "macos-arm64": (name) => /\.dmg$/i.test(name) && /(?:arm64|aarch64)/i.test(name),
    "macos-x64": (name) => /\.dmg$/i.test(name) && x64(name),
    "linux-appimage": (name) => /\.appimage$/i.test(name) && x64(name),
    "linux-deb": (name) => /\.deb$/i.test(name) && x64(name),
  };
  const downloads = {};
  for (const [platform, matches] of Object.entries(formats)) {
    const asset = assets.find((candidate) => matches(candidate.name));
    if (asset) downloads[platform] = { name: asset.name, url: trustedUrl(asset.browser_download_url), size: Number.isFinite(asset.size) && asset.size > 0 ? asset.size : 0 };
  }
  return { tag: release.tag_name, url, publishedAt: release.published_at || null, downloads };
}

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[character]);
}

async function latestRelease(repository) {
  const headers = { Accept: "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "Palladium-Pages" };
  if (process.env.GITHUB_TOKEN) headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
  try {
    const response = await fetch(`https://api.github.com/repos/${repository}/releases/latest`, { headers, signal: AbortSignal.timeout(15000) });
    if (!response.ok) throw new Error(`GitHub returned HTTP ${response.status}`);
    return normalizeRelease(await response.json(), repository);
  } catch (error) {
    console.warn(`Release lookup unavailable (${error.message}); keeping working Releases links.`);
    return normalizeRelease(null, repository);
  }
}

export async function build() {
  const metadata = JSON.parse(await readFile(resolve(root, "native/metadata.json"), "utf8"));
  const repository = repositoryName(metadata.repository);
  const repositoryUrl = `https://github.com/${repository}`;
  const baseUrl = siteUrl(repository, process.env.PAGES_BASE_URL);
  const release = await latestRelease(repository);
  const replacements = {
    __SITE_URL__: escapeHtml(baseUrl),
    __REPOSITORY_URL__: escapeHtml(repositoryUrl),
    __VERSION_LABEL__: release.tag ? `${escapeHtml(release.tag)} 已发布` : "获取最新发行版",
  };

  // Only this allowlist is published; app source, credentials and build tools stay private.
  await rm(output, { recursive: true, force: true });
  await mkdir(resolve(output, "assets"), { recursive: true });
  for (const name of ["index.html", "404.html", "styles.css", "main.js"]) {
    let content = await readFile(resolve(source, name), "utf8");
    for (const [key, value] of Object.entries(replacements)) content = content.replaceAll(key, value);
    if (/__[A-Z_]+__/.test(content)) throw new Error(`Unresolved placeholder in ${name}`);
    await writeFile(resolve(output, name), content);
  }
  const assets = {
    "mark-black.png": "native/assets/brand/mark-black.png",
    "mark-white.png": "native/assets/brand/mark-white.png",
    "favicon.png": "native/assets/brand/platform/favicon-32.png",
    "social-banner.png": "native/assets/brand/banners/palladium-launcher-hero-light.png",
    "manrope-latin.woff2": "website/assets/manrope-latin.woff2",
    "Manrope-OFL.txt": "website/assets/Manrope-OFL.txt",
  };
  for (const [name, origin] of Object.entries(assets)) await copyFile(resolve(root, origin), resolve(output, "assets", name));
  await writeFile(resolve(output, "release.json"), `${JSON.stringify(release, null, 2)}\n`);
  await writeFile(resolve(output, ".nojekyll"), "");
  await writeFile(resolve(output, "robots.txt"), `User-agent: *\nAllow: /\nSitemap: ${baseUrl}sitemap.xml\n`);
  await writeFile(resolve(output, "sitemap.xml"), `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9"><url><loc>${escapeHtml(baseUrl)}</loc></url></urlset>\n`);
  console.log(`Built ${baseUrl} in ${output}; release: ${release.tag || "Releases fallback"}; ${Object.keys(release.downloads).length} download formats.`);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  build().catch((error) => { console.error(error); process.exitCode = 1; });
}
