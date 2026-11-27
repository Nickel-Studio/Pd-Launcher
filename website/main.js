const root = document.documentElement;
root.classList.add("js");

const themeButton = document.querySelector("[data-theme-toggle]");
const themeColor = document.querySelector('meta[name="theme-color"]');
function updateThemeButton() {
  const dark = root.dataset.theme === "dark";
  themeButton.setAttribute("aria-label", dark ? "切换为浅色模式" : "切换为深色模式");
  themeButton.setAttribute("aria-pressed", String(dark));
  themeColor.setAttribute("content", dark ? "#191b18" : "#f5f5f1");
}
if (themeButton) {
  themeButton.hidden = false;
  updateThemeButton();
  themeButton.addEventListener("click", () => {
    root.dataset.theme = root.dataset.theme === "dark" ? "light" : "dark";
    try { localStorage.setItem("palladium.website.theme", root.dataset.theme); } catch { /* Optional preference. */ }
    updateThemeButton();
  });
}

const menuButton = document.querySelector("[data-menu-toggle]");
const header = document.querySelector("[data-header]");
const navigation = document.querySelector("#main-nav");
function closeMenu() {
  header.dataset.menuOpen = "false";
  menuButton.setAttribute("aria-expanded", "false");
  menuButton.setAttribute("aria-label", "打开导航菜单");
}
if (menuButton) {
  menuButton.hidden = false;
  menuButton.addEventListener("click", () => {
    const open = menuButton.getAttribute("aria-expanded") !== "true";
    header.dataset.menuOpen = String(open);
    menuButton.setAttribute("aria-expanded", String(open));
    menuButton.setAttribute("aria-label", open ? "关闭导航菜单" : "打开导航菜单");
  });
  navigation.addEventListener("click", (event) => {
    if (event.target.closest("a")) closeMenu();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && menuButton.getAttribute("aria-expanded") === "true") {
      closeMenu();
      menuButton.focus();
    }
  });
  document.addEventListener("click", (event) => {
    if (!header.contains(event.target)) closeMenu();
  });
  matchMedia("(min-width: 961px)").addEventListener("change", closeMenu);
}

const tablist = document.querySelector(".platform-tabs");
const tabs = [...document.querySelectorAll("[data-platform]")];
const panels = [...document.querySelectorAll("[data-panel]")];
const platformHint = document.querySelector("[data-platform-hint]");
const device = navigator.userAgentData?.platform || navigator.platform || "";
const detectedPlatform = /mac/i.test(device) ? "macos" : /linux/i.test(device) && !/android/i.test(navigator.userAgent) ? "linux" : "windows";
const platformNames = { windows: "Windows x64", macos: "macOS · Apple Silicon / Intel", linux: "Linux x64" };
function selectPlatform(platform, moveFocus = false) {
  tabs.forEach((tab) => {
    const selected = tab.dataset.platform === platform;
    tab.setAttribute("aria-selected", String(selected));
    tab.tabIndex = selected ? 0 : -1;
    if (selected && moveFocus) tab.focus();
  });
  panels.forEach((panel) => {
    panel.hidden = panel.dataset.panel !== platform;
    panel.setAttribute("role", "tabpanel");
    panel.setAttribute("aria-labelledby", `tab-${panel.dataset.panel}`);
    panel.tabIndex = 0;
  });
  platformHint.textContent = `当前选择：${platformNames[platform]}`;
}
if (tablist) {
  tablist.hidden = false;
  selectPlatform(detectedPlatform);
  tabs.forEach((tab) => tab.addEventListener("click", () => selectPlatform(tab.dataset.platform)));
  tablist.addEventListener("keydown", (event) => {
    const index = tabs.indexOf(document.activeElement);
    if (index === -1) return;
    let next;
    if (event.key === "ArrowRight") next = (index + 1) % tabs.length;
    if (event.key === "ArrowLeft") next = (index + tabs.length - 1) % tabs.length;
    if (event.key === "Home") next = 0;
    if (event.key === "End") next = tabs.length - 1;
    if (next !== undefined) {
      event.preventDefault();
      selectPlatform(tabs[next].dataset.platform, true);
    }
  });
}

function safeGitHubUrl(value) {
  try {
    const url = new URL(value);
    return url.protocol === "https:" && url.hostname === "github.com" && !url.username && !url.password ? url.href : null;
  } catch { return null; }
}
function fileSize(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "";
  return bytes >= 1024 * 1024 ? `${(bytes / (1024 * 1024)).toFixed(1)} MiB` : `${Math.ceil(bytes / 1024)} KiB`;
}
function showReleaseNotice(text) {
  const notice = document.querySelector("[data-release-notice]");
  notice.textContent = text;
  notice.hidden = false;
}
async function loadRelease() {
  try {
    // Metadata is generated at deploy time: visitors never need a GitHub API token.
    const response = await fetch("./release.json", { signal: AbortSignal.timeout(8000) });
    if (!response.ok) throw new Error("Release metadata unavailable");
    const release = await response.json();
    if (!release.tag || !safeGitHubUrl(release.url)) throw new Error("No published release");
    document.querySelectorAll("[data-release-label]").forEach((label) => { label.textContent = `${release.tag} 已发布`; });
    document.querySelectorAll("[data-release-url]").forEach((link) => { link.href = safeGitHubUrl(release.url); });
    let missing = false;
    document.querySelectorAll("[data-download]").forEach((link) => {
      const asset = release.downloads?.[link.dataset.download];
      const url = safeGitHubUrl(asset?.url);
      if (!url) {
        missing = true;
        link.querySelector("[data-file-meta]").textContent = "查看可用发行文件";
        return;
      }
      link.href = url;
      link.setAttribute("aria-label", `下载 ${asset.name}`);
      link.querySelector("[data-file-meta]").textContent = `${release.tag} · ${fileSize(asset.size)}`;
    });
    if (missing) showReleaseNotice("部分格式在当前发行版中暂未提供。对应入口将带你前往 Releases 查看可用文件。");
  } catch {
    showReleaseNotice("暂时无法读取发行信息。下载入口仍可带你前往 GitHub Releases，选择适合设备的文件。");
  }
}
loadRelease();

document.querySelectorAll("[data-year]").forEach((element) => { element.textContent = String(new Date().getFullYear()); });
