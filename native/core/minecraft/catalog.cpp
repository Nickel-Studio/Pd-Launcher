#include "catalog.hpp"
#include "maven.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <stdexcept>

namespace nickel::minecraft {
namespace {

std::string text(const Json& value, std::string_view key, std::string fallback = {}) {
    if (!value.is_object()) return fallback;
    const auto it = value.find(std::string(key));
    if (it == value.end() || it->is_null()) return fallback;
    if (!it->is_string()) throw std::invalid_argument("Loader metadata string has an invalid type");
    return it->get<std::string>();
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string forge_mc(std::string_view value) {
    std::string result(value);
    std::replace(result.begin(), result.end(), '-', '_');
    return result;
}

void normalize(std::vector<LoaderBuild>& builds) {
    std::stable_sort(builds.begin(), builds.end(), [](const auto& a, const auto& b) {
        return compare_versions(a.version, b.version) > 0;
    });
    std::set<std::pair<LoaderKind, std::string>> seen;
    std::erase_if(builds, [&](const auto& build) { return !seen.emplace(build.kind, build.version).second; });
}

std::string fetch_text(const Services& services, std::string_view url, const CancelCheck& check) {
    if (!services.fetch) throw std::logic_error("Minecraft HTTP adapter is unavailable");
    check();
    auto result = services.fetch(url, 16 * 1024 * 1024, check);
    check();
    if (result.size() > 16 * 1024 * 1024) throw std::invalid_argument("Loader metadata exceeds the size limit");
    return result;
}

std::vector<std::string> hrefs(std::string_view input) {
    const std::string source(input);
    const std::regex pattern(R"rx(href\s*=\s*["']([^"']+)["'])rx", std::regex::icase);
    std::vector<std::string> result;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), pattern); it != std::sregex_iterator(); ++it) {
        std::string value = (*it)[1].str();
        for (auto at = value.find("&amp;"); at != std::string::npos; at = value.find("&amp;", at + 1)) value.replace(at, 5, "&");
        result.push_back(std::move(value));
    }
    return result;
}

std::vector<LoaderBuild> forge_files(std::string_view source, std::string_view mc, const CatalogEndpoints& endpoints) {
    std::vector<LoaderBuild> builds;
    const auto apiMc = forge_mc(mc);
    const auto input = trim(std::string(source));
    if (input.starts_with('[')) {
        const auto value = parse_metadata(input);
        for (const auto& entry : value) {
            const auto listedMc = text(entry, "mcversion", text(entry, "minecraft", std::string(mc)));
            if (forge_mc(listedMc) != apiMc) continue;
            auto version = text(entry, "version");
            const auto branch = text(entry, "branch");
            if (!branch.empty()) version += '-' + branch;
            if (version.empty()) continue;
            validate_version_id(version);
            LoaderBuild row;
            row.kind = LoaderKind::Forge;
            row.minecraftVersion = mc;
            row.version = version;
            row.releaseTime = text(entry, "modified");
            row.installable = false;
            if (entry.contains("files") && entry["files"].is_array()) {
                for (const auto& file : entry["files"]) {
                    if (lower(text(file, "category")) != "installer" || lower(text(file, "format")) != "jar") continue;
                    row.artifact.filename = text(file, "name", text(file, "filename", "forge-" + apiMc + '-' + version + "-installer.jar"));
                    const auto relative = validate_relative_path(row.artifact.filename);
                    if (relative.has_parent_path()) throw std::invalid_argument("Forge filename must be a basename");
                    row.artifact.url = text(file, "url", endpoints.forgeMaven + '/' + apiMc + '-' + version + '/' + row.artifact.filename);
                    Json artifact = file;
                    artifact["url"] = row.artifact.url;
                    const auto digest = text(file, "hash");
                    if (!artifact.contains("sha1") && digest.size() == 40) artifact["sha1"] = digest;
                    if (!artifact.contains("md5") && digest.size() == 32) artifact["md5"] = digest;
                    if (artifact.contains("size") && artifact["size"].is_string()) artifact.erase("size");
                    const auto parsed = artifact_download(artifact, relative, FileKind::Loader);
                    row.artifact.hashes = parsed.hashes;
                    row.artifact.size = parsed.size;
                    row.installable = true;
                    break;
                }
            }
            row.artifact.rawMetadata = entry;
            row.artifact.rawMetadata["coordinateVersion"] = apiMc + '-' + version;
            builds.push_back(std::move(row));
        }
    } else {
        const auto prefix = "forge-" + apiMc + '-';
        for (const auto& href : hrefs(input)) {
            const auto last = href.find_last_of('/');
            const auto filename = href.substr(last == std::string::npos ? 0 : last + 1);
            if (!filename.starts_with(prefix)) continue;
            const auto rest = filename.substr(prefix.size());
            const auto separator = rest.rfind('-');
            if (separator == std::string::npos) continue;
            const auto version = rest.substr(0, separator);
            const auto category = rest.substr(separator + 1);
            if (category != "installer.jar" && category != "universal.zip" && category != "client.jar" && category != "client.zip") continue;
            validate_version_id(version);
            auto found = std::find_if(builds.begin(), builds.end(), [&](const auto& build) { return build.version == version; });
            if (found == builds.end()) {
                LoaderBuild row;
                row.kind = LoaderKind::Forge;
                row.minecraftVersion = mc;
                row.version = version;
                row.installable = false;
                builds.push_back(std::move(row));
                found = builds.end() - 1;
            }
            if (category == "installer.jar") {
                found->installable = true;
                found->artifact.filename = filename;
                found->artifact.url = endpoints.forgeMaven + '/' + apiMc + '-' + version + '/' + filename;
                found->artifact.rawMetadata = Json{{"coordinateVersion", apiMc + '-' + version}, {"category", "installer"}};
            }
        }
    }
    if (builds.empty()) throw std::invalid_argument("Forge catalog has no actual build files");
    normalize(builds);
    return builds;
}

std::optional<std::string> forge_requirement(std::string value, bool present) {
    if (!present) return {};
    value = trim(std::move(value));
    if (value.empty() || value == "N/A" || value == "-" || value == "none") return std::string{};
    if (value.starts_with("Forge ")) value.erase(0, 6);
    if (std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; })) return value;
    return {};
}

LoaderBuild optifine_row(std::string filename, std::string date, std::optional<std::string> requirement) {
    validate_version_id(filename);
    if (!filename.ends_with(".jar")) throw std::invalid_argument("OptiFine artifact is not a JAR");
    auto value = filename;
    value.resize(value.size() - 4);
    if (value.starts_with("preview_")) value.erase(0, 8);
    if (!value.starts_with("OptiFine_")) throw std::invalid_argument("Unrecognized OptiFine filename");
    value.erase(0, 9);
    const auto at = value.find('_');
    if (at == std::string::npos) throw std::invalid_argument("OptiFine filename has no Minecraft identity");
    LoaderBuild row;
    row.kind = LoaderKind::OptiFine;
    row.minecraftVersion = canonical_minecraft_id(value.substr(0, at));
    row.version = filename.substr(0, filename.size() - 4);
    row.releaseTime = date;
    row.stable = lower(filename).find("pre") == std::string::npos;
    row.artifact.filename = filename;
    row.artifact.forgeRequirement = std::move(requirement);
    const auto typeEnd = value.find('_', at + 1);
    if (typeEnd != std::string::npos) {
        row.artifact.rawMetadata = Json{{"mcversion", value.substr(0, at)}, {"type", value.substr(at + 1, typeEnd - at - 1)}, {"patch", value.substr(typeEnd + 1)}};
    }
    return row;
}

std::string cell_text(const std::string& row, std::string_view name, bool& present) {
    const std::regex pattern("<td[^>]*class\\s*=\\s*[\"'][^\"']*" + std::string(name) + "[^\"']*[\"'][^>]*>([\\s\\S]*?)</td>", std::regex::icase);
    std::smatch match;
    present = std::regex_search(row, match, pattern);
    if (!present) return {};
    return trim(std::regex_replace(match[1].str(), std::regex("<[^>]*>"), ""));
}

}

std::vector<LoaderBuild> parse_optifine_catalog(std::string_view source) {
    std::vector<LoaderBuild> builds;
    const auto input = trim(std::string(source));
    if (input.starts_with('[')) {
        const auto items = parse_metadata(input);
        for (const auto& item : items) {
            const auto filename = text(item, "filename");
            if (filename.empty()) continue;
            auto row = optifine_row(filename, text(item, "releaseTime"), forge_requirement(text(item, "forge"), true));
            if (canonical_minecraft_id(text(item, "mcversion", row.minecraftVersion)) != row.minecraftVersion) throw std::invalid_argument("OptiFine catalog identity mismatch");
            row.artifact.rawMetadata = item;
            builds.push_back(std::move(row));
        }
    } else {
        const std::regex rows(R"rx(<tr[^>]*>[\s\S]*?</tr>)rx", std::regex::icase);
        const std::regex filename(R"rx((?:preview_)?OptiFine_[A-Za-z0-9_.+\-]+\.jar)rx");
        for (auto it = std::sregex_iterator(input.begin(), input.end(), rows); it != std::sregex_iterator(); ++it) {
            const auto row = it->str();
            std::smatch match;
            if (!std::regex_search(row, match, filename)) continue;
            bool hasForge = false, hasDate = false;
            const auto requirement = cell_text(row, "colForge", hasForge);
            const auto date = cell_text(row, "colDate", hasDate);
            builds.push_back(optifine_row(match.str(), date, forge_requirement(requirement, hasForge)));
        }
    }
    if (builds.empty()) throw std::invalid_argument("OptiFine catalog has no recognized entries");
    normalize(builds);
    return builds;
}

Json liteloader_profile(std::string_view mc, const Json& metadata) {
    validate_version_id(mc);
    const auto version = text(metadata, "version");
    const auto tweaker = text(metadata, "tweakClass");
    if (version.empty() || tweaker.empty()) throw std::invalid_argument("LiteLoader metadata lacks version or tweakClass");
    auto libraries = metadata.value("libraries", Json::array());
    if (!libraries.is_array()) throw std::invalid_argument("LiteLoader libraries must be an array");
    Json artifact = Json::object();
    const auto filename = text(metadata, "file");
    if (!filename.empty()) artifact["url"] = "https://dl.liteloader.com/versions/" + url_encode_segment(std::string(mc)) + '/' + url_encode_segment(filename);
    if (metadata.contains("md5")) artifact["md5"] = metadata["md5"];
    Json library{{"name", "com.mumfrey:liteloader:" + version}, {"url", "https://dl.liteloader.com/versions/"}};
    if (!artifact.empty()) library["downloads"]["artifact"] = artifact;
    libraries.push_back(std::move(library));
    if (std::none_of(libraries.begin(), libraries.end(), [](const auto& item) { return text(item, "name").starts_with("net.minecraft:launchwrapper:"); })) {
        libraries.push_back(Json{{"name", "net.minecraft:launchwrapper:1.12"}});
    }
    return Json{{"id", std::string(mc) + "-LiteLoader"}, {"inheritsFrom", mc}, {"jar", mc}, {"type", "release"},
                {"mainClass", "net.minecraft.launchwrapper.Launch"}, {"libraries", std::move(libraries)},
                {"arguments", {{"game", Json::array({"--tweakClass", tweaker})}}}, {"releaseTime", text(metadata, "timestamp")}};
}

std::vector<LoaderBuild> parse_liteloader_catalog(std::string_view source) {
    const auto value = parse_metadata(source);
    if (!value.contains("versions") || !value["versions"].is_object()) throw std::invalid_argument("LiteLoader catalog has no versions object");
    std::vector<LoaderBuild> builds;
    for (const auto& [mc, item] : value["versions"].items()) {
        if (mc.starts_with("1.5") || mc.starts_with("1.6")) continue;
        const auto section = item.contains("artefacts") ? "artefacts" : "snapshots";
        if (!item.contains(section) || !item[section].contains("com.mumfrey:liteloader")) continue;
        const auto& entries = item[section]["com.mumfrey:liteloader"];
        if (!entries.contains("latest")) continue;
        const auto& raw = entries["latest"];
        LoaderBuild row;
        row.kind = LoaderKind::LiteLoader;
        row.minecraftVersion = mc;
        row.version = text(raw, "version");
        row.releaseTime = text(raw, "timestamp");
        row.stable = lower(text(raw, "stream")) != "snapshot";
        row.artifact.rawMetadata = raw;
        row.artifact.profile = liteloader_profile(mc, raw);
        row.artifact.filename = mc + "-LiteLoader.json";
        builds.push_back(std::move(row));
    }
    if (builds.empty()) throw std::invalid_argument("LiteLoader catalog has no supported latest entries");
    normalize(builds);
    return builds;
}

Catalog::Catalog(Services services, CatalogEndpoints endpoints)
    : services_(std::move(services)), endpoints_(std::move(endpoints)) {}

std::vector<LoaderBuild> Catalog::builds(LoaderKind kind, std::string_view mc, const CancelCheck& check) const {
    validate_version_id(mc);
    std::vector<LoaderBuild> result;
    if (kind == LoaderKind::Vanilla) return result;
    if (kind == LoaderKind::Fabric || kind == LoaderKind::LegacyFabric || kind == LoaderKind::Quilt) {
        const auto& base = kind == LoaderKind::Fabric ? endpoints_.fabric : kind == LoaderKind::LegacyFabric ? endpoints_.legacyFabric : endpoints_.quilt;
        const auto metadata = fetch_metadata(services_, base + "/loader/" + url_encode_segment(fabric_minecraft_id(mc)), check);
        if (!metadata.is_array()) throw std::invalid_argument("Lightweight loader catalog must be an array");
        for (const auto& entry : metadata) {
            const auto& loader = entry.contains("loader") ? entry["loader"] : entry;
            LoaderBuild row;
            row.kind = kind;
            row.minecraftVersion = mc;
            row.version = text(loader, "version");
            if (row.version.empty()) continue;
            validate_version_id(row.version);
            row.stable = loader.value("stable", true);
            row.artifact.rawMetadata = entry;
            row.artifact.url = base + "/loader/" + url_encode_segment(fabric_minecraft_id(mc)) + '/' + url_encode_segment(row.version) + "/profile/json";
            row.artifact.filename = std::string(mc) + '-' + std::string(loader_name(kind)) + '-' + row.version + ".json";
            result.push_back(std::move(row));
        }
    } else if (kind == LoaderKind::Forge) {
        try {
            result = forge_files(fetch_text(services_, endpoints_.forgeFiles + "/index_" + forge_mc(mc) + ".html", check), mc, endpoints_);
        } catch (...) {
            check();
            result = forge_files(fetch_text(services_, endpoints_.forgeFilesMirror + '/' + url_encode_segment(forge_mc(mc)), check), mc, endpoints_);
        }
        try {
            const auto promotions = fetch_metadata(services_, endpoints_.forgePromotions, check);
            const auto recommended = text(promotions.value("promos", Json::object()), forge_mc(mc) + "-recommended");
            for (auto& build : result) build.recommended = build.version == recommended || build.version.starts_with(recommended + '-');
        } catch (...) { check(); }
    } else if (kind == LoaderKind::NeoForge) {
        const auto artifact = mc == "1.20.1" ? "forge" : "neoforge";
        const auto base = endpoints_.neoForgeMaven + '/' + artifact;
        for (const auto& version : parse_maven_versions(fetch_text(services_, base + "/maven-metadata.xml", check))) {
            if (!neoforge_matches_minecraft(mc, version) || version == "1.20.1-47.1.82") continue;
            LoaderBuild row;
            row.kind = kind;
            row.minecraftVersion = mc;
            row.version = version;
            row.stable = lower(version).find("beta") == std::string::npos;
            row.artifact.filename = std::string(artifact) + '-' + version + "-installer.jar";
            row.artifact.url = base + '/' + url_encode_segment(version) + '/' + row.artifact.filename;
            row.artifact.rawMetadata = Json{{"coordinateVersion", version}};
            result.push_back(std::move(row));
        }
    } else if (kind == LoaderKind::OptiFine) {
        try { result = parse_optifine_catalog(fetch_text(services_, endpoints_.optiFineIndex, check)); }
        catch (...) { check(); result = parse_optifine_catalog(fetch_text(services_, endpoints_.optiFineMirror, check)); }
        std::erase_if(result, [&](const auto& build) { return build.minecraftVersion != canonical_minecraft_id(mc); });
        for (auto& build : result) {
            const auto& raw = build.artifact.rawMetadata;
            const auto fileMc = text(raw, "mcversion", std::string(mc));
            const auto type = text(raw, "type");
            const auto patch = text(raw, "patch");
            if (!type.empty() && !patch.empty()) build.artifact.url = endpoints_.optiFineDownload + '/' + url_encode_segment(fileMc) + '/' + url_encode_segment(type) + '/' + url_encode_segment(patch);
        }
    } else if (kind == LoaderKind::LiteLoader) {
        result = parse_liteloader_catalog(fetch_text(services_, endpoints_.liteLoader, check));
        std::erase_if(result, [&](const auto& build) { return build.minecraftVersion != mc; });
    }
    normalize(result);
    check();
    return result;
}

LoaderBuild Catalog::resolve(const LoaderRequest& request, std::string_view mc, const CancelCheck& check) const {
    validate_version_id(mc);
    check();
    if (request.artifact) {
        LoaderBuild result;
        result.kind = request.kind;
        result.minecraftVersion = mc;
        result.version = request.version;
        result.artifact = *request.artifact;
        if (result.version == "latest" || result.version == "recommended" || result.version.empty()) {
            result.version = text(result.artifact.rawMetadata, "version");
            if (result.version.empty()) throw std::invalid_argument("Pinned loader artifact requires an exact version");
        }
        validate_version_id(result.version);
        return result;
    }
    auto available = builds(request.kind, mc, check);
    auto selector = request.version;
    const auto prefix = forge_mc(mc) + '-';
    if (request.kind == LoaderKind::Forge && selector.starts_with(prefix)) selector.erase(0, prefix.size());
    const auto found = std::find_if(available.begin(), available.end(), [&](const auto& build) {
        if (!build.installable) return false;
        if (selector == "latest") return true;
        if (selector == "recommended") return build.recommended || (request.kind != LoaderKind::Forge && build.stable);
        return build.version == selector;
    });
    if (found == available.end()) throw std::invalid_argument("No installable loader build matches the requested Minecraft version");
    auto result = *found;
    if (request.kind == LoaderKind::Fabric || request.kind == LoaderKind::LegacyFabric || request.kind == LoaderKind::Quilt) {
        result.artifact.profile = fetch_metadata(services_, result.artifact.url, check);
        const auto parent = text(result.artifact.profile, "inheritsFrom");
        if (parent != mc && parent != fabric_minecraft_id(mc)) throw std::invalid_argument("Loader profile inherits another Minecraft version");
    }
    if (request.kind == LoaderKind::OptiFine) {
        try {
            const auto page = fetch_text(services_, "https://optifine.net/adloadx?f=" + url_encode_segment(result.artifact.filename), check);
            const auto links = hrefs(page);
            const auto direct = std::find_if(links.begin(), links.end(), [](const auto& link) {
                return link.starts_with("downloadx?f=") || link.starts_with("https://optifine.net/downloadx?f=") || link.starts_with("/downloadx?f=");
            });
            if (direct != links.end()) {
                result.artifact.rawMetadata["fallbackUrls"] = Json::array({result.artifact.url});
                result.artifact.url = direct->starts_with("https://") ? *direct : "https://optifine.net/" + (direct->starts_with('/') ? direct->substr(1) : *direct);
            }
        } catch (...) { check(); }
    }
    if ((request.kind == LoaderKind::Forge || request.kind == LoaderKind::NeoForge) && result.artifact.hashes.empty()) {
        try {
            auto digest = trim(fetch_text(services_, result.artifact.url + ".sha1", check));
            digest = digest.substr(0, digest.find_first_of(" \t\r\n"));
            Json artifact{{"url", result.artifact.url}, {"sha1", digest}};
            result.artifact.hashes = artifact_download(artifact, std::filesystem::path(result.artifact.filename), FileKind::Loader).hashes;
        } catch (...) { check(); }
    }
    check();
    return result;
}

Json loader_builds_json(std::span<const LoaderBuild> builds) {
    Json result = Json::array();
    for (const auto& build : builds) {
        result.push_back(Json{{"kind", loader_name(build.kind)}, {"minecraftVersion", build.minecraftVersion},
                             {"version", build.version}, {"releaseTime", build.releaseTime}, {"stable", build.stable},
                             {"recommended", build.recommended}, {"installable", build.installable}});
    }
    return result;
}

}
