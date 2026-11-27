#include "mods.hpp"
#include "utility.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

namespace nickel::resources {
namespace {

const Json& field(const Json& object, const char* key) {
    static const Json absent;
    const auto found = object.find(key);
    return found == object.end() ? absent : *found;
}

std::string cleanText(std::string_view raw, std::size_t maximum = 16384) {
    if (!validUtf8(raw)) fail("invalid_response", "Mod metadata is not valid UTF-8");
    std::string value;
    value.reserve(std::min(raw.size(), maximum));
    for (std::size_t i = 0; i < raw.size();) {
        const auto c = std::uint8_t(raw[i]);
        const auto length = c < 0x80 ? 1U : c < 0xe0 ? 2U : c < 0xf0 ? 3U : 4U;
        if (length > maximum - value.size()) break;
        if (c == 0x7f || (c < 0x20 && c != '\t' && c != '\n' && c != '\r')) { ++i; continue; }
        value.append(raw.substr(i, length));
        i += length;
    }
    return value;
}

std::string jsonText(const Json& object, const char* key, std::size_t maximum = 16384) {
    const auto& value = field(object, key);
    return value.is_string() ? cleanText(value.get_ref<const std::string&>(), maximum) : std::string{};
}

std::string normalizeIdentity(std::string_view raw) {
    auto value = lower(trim(cleanText(raw, 160)));
    std::string result;
    bool whitespace = false;
    for (const auto c : value) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { whitespace = !result.empty(); continue; }
        if (whitespace) result += ' ';
        whitespace = false;
        result += c;
    }
    return result;
}

void addTag(Strings& tags, std::string_view raw) {
    const auto tag = trim(raw);
    if (tag.empty() || tag.size() > 64 || tags.size() >= 32 || !validUtf8(tag) || contains(tags, tag, true)) return;
    for (const auto c : tag) if (std::uint8_t(c) < 0x20 || c == 0x7f) return;
    tags.push_back(tag);
}

void readTags(Strings& tags, const Json& object) {
    for (const auto key : {"categories", "category", "tags", "keywords"}) {
        const auto& value = field(object, key);
        if (value.is_string()) addTag(tags, value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& item : value) {
            if (item.is_string()) addTag(tags, item.get_ref<const std::string&>());
            else if (item.is_object()) addTag(tags, jsonText(item, item.contains("name") ? "name" : "title", 64));
        }
    }
}

std::string displayName(std::string_view name) {
    const auto split = splitModFileName(name);
    auto value = split ? split->first : std::string(name);
    for (const std::string_view suffix : {".jar", ".zip", ".litemod"}) if (lower(value).ends_with(suffix)) return value.substr(0, value.size() - suffix.size());
    return value;
}

struct ModFile { std::string name; std::string base; ModState state; };
using ModGroups = std::map<std::string, std::vector<ModFile>>;

ModGroups enumerate(const Path& rawRoot, const ModLimits& limits, const Cancellation& cancel) {
    cancel.check();
    std::error_code error;
    const auto status = std::filesystem::symlink_status(rawRoot, error);
    if (error && error != std::errc::no_such_file_or_directory) fail("io", "Cannot inspect Mods root");
    if (!std::filesystem::exists(status)) {
        detail::checkedDirectory(rawRoot.parent_path());
        return {};
    }
    const auto root = detail::checkedDirectory(rawRoot);
    ModGroups groups;
    std::size_t count = 0;
    auto iterator = std::filesystem::directory_iterator(root, error);
    if (error) fail("io", "Cannot enumerate Mods root");
    for (const auto& entry : iterator) {
        cancel.check();
        if (++count > limits.maximumFiles) fail("too_large", "Mods directory exceeds file budget");
        const auto name = utf8Path(entry.path().filename());
        const auto split = splitModFileName(name);
        if (!split) continue;
        try { detail::checkedLeaf(root, name); }
        catch (const ErrorException& failure) { if (failure.error().code == "policy_denied") continue; throw; }
        groups[lower(split->first)].push_back({name, split->first, split->second});
    }
    for (auto& [key, files] : groups) {
        (void)key;
        std::stable_sort(files.begin(), files.end(), [](const auto& first, const auto& second) {
            return first.state != second.state ? first.state < second.state : lower(first.name) < lower(second.name);
        });
    }
    return groups;
}

std::vector<ModEntry> listLocal(const Path& root, const ModLimits& limits, const Cancellation& cancel) {
    std::vector<ModEntry> result;
    for (const auto& [key, files] : enumerate(root, limits, cancel)) {
        (void)key;
        if (files.empty()) continue;
        ModEntry entry;
        entry.fileName = files.front().name;
        for (const auto& file : files) entry.physicalFiles.push_back(file.name);
        entry.name = displayName(entry.fileName);
        entry.description = entry.name;
        entry.enabled = files.front().state == ModState::Enabled;
        entry.revision = detail::fileRevision(detail::checkedLeaf(root, entry.fileName));
        result.push_back(std::move(entry));
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& first, const auto& second) { return lower(first.fileName) < lower(second.fileName); });
    return result;
}

void validateArchive(const std::vector<ArchiveEntry>& entries) {
    if (entries.empty()) fail("integrity", "Mod is not a supported archive");
    if (entries.size() > 10000) fail("too_large", "Mod archive exceeds entry budget");
    std::set<std::string> names;
    for (const auto& entry : entries) {
        auto key = lower(entry.name);
        if (key.ends_with('/')) key.pop_back();
        if (!validArchiveRelativePath(entry.name) || !names.insert(key).second || (!entry.regular && !entry.directory)) fail("integrity", "Mod archive contains unsafe, linked or duplicate entries");
    }
}

std::optional<ModMetadata> localMetadata(const Path& root, const std::string& name, const std::string& loader,
                                        const ModServices& services, const ModLimits& limits, const Cancellation& cancel) {
    if (!services.archiveEntries || !services.readArchiveEntry) fail("unsupported", "Shared bounded archive reader is unavailable");
    const auto path = detail::checkedLeaf(root, name);
    detail::FileReader pin(path, limits.maximumArchiveBytes);
    const auto entries = services.archiveEntries(path, cancel);
    validateArchive(entries);
    static const std::set<std::string> known{"fabric.mod.json", "quilt.mod.json", "mcmod.info", "META-INF/mods.toml", "META-INF/neoforge.mods.toml", "META-INF/MANIFEST.MF"};
    std::map<std::string, Bytes> manifests;
    for (const auto& entry : entries) {
        cancel.check();
        if (!known.contains(entry.name)) continue;
        if (entry.directory || !entry.regular) fail("integrity", "Mod manifest is not regular");
        if (entry.expandedBytes > limits.maximumManifestBytes) fail("too_large", "Mod manifest exceeds read budget");
        auto bytes = services.readArchiveEntry(path, entry.name, limits.maximumManifestBytes, cancel);
        if (bytes.size() > limits.maximumManifestBytes || bytes.size() != entry.expandedBytes) fail("integrity", "Archive reader returned an incorrect bounded manifest");
        manifests.emplace(entry.name, std::move(bytes));
    }
    pin.verifyUnchanged();
    cancel.check();
    try { return readModMetadata(manifests, name, loader); }
    catch (const ErrorException& failure) {
        if (failure.error().code == "not_found" || failure.error().code == "invalid_response") return {};
        throw;
    }
}

Strings selected(const Path& root, const Strings& names, const ModLimits& limits, const Cancellation& cancel) {
    if (names.empty() || names.size() > limits.maximumSelection) fail("invalid_input", "Select a bounded non-empty set of Mod files");
    std::set<std::string> seen;
    for (const auto& name : names) {
        cancel.check();
        detail::checkedLeaf(root, name);
        if (!seen.insert(lower(name)).second) fail("invalid_input", "Mod selection contains case-colliding files");
    }
    return names;
}

bool filesEqual(const Path& first, const Path& second, const ModLimits& limits, const Cancellation& cancel) {
    detail::FileReader left(first, limits.maximumArchiveBytes), right(second, limits.maximumArchiveBytes);
    if (left.size() != right.size()) return false;
    std::array<std::uint8_t, 65536> a{}, b{};
    for (;;) {
        const auto firstCount = left.read(a, cancel);
        const auto secondCount = right.read(b, cancel);
        if (firstCount != secondCount || !std::equal(a.begin(), a.begin() + firstCount, b.begin())) return false;
        if (!firstCount) break;
    }
    left.verifyUnchanged();
    right.verifyUnchanged();
    return true;
}

struct Rename { std::string from; std::string to; };

void applyRenames(const Path& root, const std::vector<Rename>& changes, const Cancellation& cancel) {
    std::vector<Rename> committed;
    committed.reserve(changes.size());
    try {
        for (const auto& change : changes) {
            cancel.check();
            detail::renameExclusive(root, change.from, change.to);
            committed.push_back(change);
        }
        cancel.check();
    } catch (...) {
        const auto original = std::current_exception();
        bool restored = true;
        for (auto it = committed.rbegin(); it != committed.rend(); ++it) {
            try { detail::renameExclusive(root, it->to, it->from); }
            catch (...) { restored = false; }
        }
        if (!restored) fail("io", "Mod state transaction failed; recovery files remain in the Mods root");
        std::rethrow_exception(original);
    }
}

std::shared_ptr<void> mutationLease(const ResourceServices& services, const InstanceContext& context) {
    if (!services.acquireMutation) fail("unsupported", "Shared instance mutation service is unavailable");
    if (context.running) fail("busy", "Stop the instance before modifying Mods");
    auto lease = services.acquireMutation(context.identity, context.revision);
    if (!lease) fail("busy", "Instance mutation lease was not acquired");
    return lease;
}


std::string manifestVersion(const std::map<std::string, Bytes>& manifests) {
    const auto found = manifests.find("META-INF/MANIFEST.MF");
    if (found == manifests.end()) return {};
    const auto& bytes = found->second;
    if (bytes.size() > 256 * 1024) fail("too_large", "JAR manifest exceeds budget");
    const std::string_view input(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!validUtf8(input)) return {};
    std::string current, version;
    auto remaining = input;
    const auto finish = [&] {
        if (lower(current).starts_with("implementation-version:")) version = trim(cleanText(current.substr(23), 256));
    };
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        auto line = remaining.substr(0, newline);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with(' ')) current.append(line.substr(1));
        else { finish(); current = std::string(line); }
        if (newline == std::string_view::npos) break;
        remaining.remove_prefix(newline + 1);
    }
    finish();
    return version;
}

std::string iconPath(const Json& object) {
    auto icon = jsonText(object, "icon", 1024);
    const auto& icons = field(object, "icon");
    if (icons.is_object()) {
        unsigned best = 0;
        for (auto it = icons.begin(); it != icons.end(); ++it) {
            if (!it.value().is_string()) continue;
            unsigned size = 0;
            const auto parsed = std::from_chars(it.key().data(), it.key().data() + it.key().size(), size);
            const bool numeric = parsed.ec == std::errc{} && parsed.ptr == it.key().data() + it.key().size();
            if (icon.empty() || (numeric && size <= 512 && size >= best)) {
                icon = cleanText(it.value().get_ref<const std::string&>(), 1024);
                if (numeric) best = size;
            }
        }
    }
    if (icon.empty()) icon = jsonText(object, "logoFile", 1024);
    if (icon.empty()) icon = jsonText(object, "logoFileName", 1024);
    return validArchiveRelativePath(icon) && !icon.ends_with('/') ? icon : std::string{};
}

bool sameContext(const InstanceContext& first, const InstanceContext& second) {
    return first.identity == second.identity && first.revision == second.revision && first.modsRoot == second.modsRoot &&
        first.minecraftVersion == second.minecraftVersion && first.loader == second.loader && !second.running && !second.updatesDisabled;
}

ResourceFileHashes hashIdentity(const ModServices& services, const Path& path, const ModLimits& limits, const Cancellation& cancel) {
    if (!services.hashes) fail("unsupported", "Shared streaming hash service is unavailable");
    detail::FileReader pin(path, limits.maximumArchiveBytes);
    auto result = services.hashes(path, cancel, limits.maximumArchiveBytes);
    cancel.check();
    pin.verifyUnchanged();
    if (result.sizeBytes != pin.size() || checkedHash(result.sha1, 20).empty() || checkedHash(result.sha512, 64).empty() || checkedHash(result.sha256, 32).empty()) fail("integrity", "Hash service returned invalid resource identity");
    result.sha1 = lower(result.sha1);
    result.sha512 = lower(result.sha512);
    result.sha256 = lower(result.sha256);
    return result;
}

} // namespace


ModMetadata readModMetadata(const std::map<std::string, Bytes>& manifests, const std::string& fileName, const std::string& loader) {
    Strings order;
    if (lower(loader) == "forge") order = {"META-INF/mods.toml", "META-INF/neoforge.mods.toml", "fabric.mod.json", "quilt.mod.json", "mcmod.info"};
    else if (lower(loader) == "neoforge") order = {"META-INF/neoforge.mods.toml", "META-INF/mods.toml", "fabric.mod.json", "quilt.mod.json", "mcmod.info"};
    else if (lower(loader) == "quilt") order = {"quilt.mod.json", "fabric.mod.json", "mcmod.info", "META-INF/neoforge.mods.toml", "META-INF/mods.toml"};
    else order = {"fabric.mod.json", "quilt.mod.json", "mcmod.info", "META-INF/neoforge.mods.toml", "META-INF/mods.toml"};
    for (const auto& name : order) {
        const auto entry = manifests.find(name);
        if (entry == manifests.end()) continue;
        const auto& bytes = entry->second;
        if (bytes.size() > 256 * 1024) fail("too_large", "Mod manifest exceeds parsing budget");
        if (bytes.empty()) continue;
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!validUtf8(text)) continue;
        std::vector<Json> objects;
        if (name.ends_with(".toml")) {
            try {
                const auto table = toml::parse(text);
                const auto* mods = table["mods"].as_array();
                if (!mods || mods->size() > 64) continue;
                for (const auto& raw : *mods) {
                    const auto* mod = raw.as_table();
                    if (!mod) continue;
                    Json object = Json::object();
                    for (const auto& [key, output] : std::array<std::pair<const char*, const char*>, 5>{{{"modId", "id"}, {"displayName", "name"}, {"description", "description"}, {"version", "version"}, {"logoFile", "logoFile"}}}) {
                        if (const auto value = (*mod)[key].value<std::string>()) object[output] = *value;
                    }
                    objects.push_back(std::move(object));
                }
            } catch (const toml::parse_error&) { continue; }
        } else {
            Json document;
            try { document = detail::parseBoundedJson(bytes, 256 * 1024); }
            catch (const ErrorException& failure) { if (failure.error().code == "invalid_response") continue; throw; }
            if (name == "quilt.mod.json") {
                const auto& quilt = field(document, "quilt_loader");
                const auto& meta = field(quilt, "metadata");
                auto metadata = meta.is_object() ? meta : Json::object();
                metadata["id"] = field(quilt, "id");
                metadata["version"] = field(quilt, "version");
                objects.push_back(std::move(metadata));
            } else if (name == "mcmod.info") {
                const auto& items = document.is_array() ? document : field(document, "modList");
                if (!items.is_array() || items.size() > 64) continue;
                for (const auto& item : items) if (item.is_object()) objects.push_back(item);
            } else if (document.is_object()) objects.push_back(std::move(document));
        }
        ModMetadata candidate;
        for (const auto& object : objects) {
            const auto id = trim(jsonText(object, object.contains("id") ? "id" : object.contains("modid") ? "modid" : "modId", 160));
            if (!id.empty() && !contains(candidate.ids, id)) candidate.ids.push_back(id);
            if (candidate.name.empty()) candidate.name = jsonText(object, "name", 512);
            if (candidate.description.empty()) candidate.description = jsonText(object, "description");
            if (candidate.version.empty()) candidate.version = jsonText(object, "version", 256);
            readTags(candidate.tags, object);
            if (candidate.iconEntry.empty()) candidate.iconEntry = iconPath(object);
        }
        if (candidate.name.empty() && candidate.ids.empty()) continue;
        if (candidate.name.empty()) candidate.name = displayName(fileName);
        if (candidate.version.find("${") != std::string::npos) candidate.version = manifestVersion(manifests);
        Strings normalized;
        for (const auto& id : candidate.ids) normalized.push_back(normalizeIdentity(id));
        std::sort(normalized.begin(), normalized.end());
        std::string joined;
        for (const auto& id : normalized) { if (!joined.empty()) joined += '+'; joined += id; }
        candidate.identityKey = joined.empty() ? "name:" + normalizeIdentity(candidate.name) : "id:" + joined;
        return candidate;
    }
    fail("not_found", "No supported Mod metadata manifest");
}


ModsService::ModsService(ModServices services, CommunityService* community, ModLimits limits)
    : services_(std::move(services)), community_(community), limits_(limits) {
    if (!limits_.maximumFiles || limits_.maximumFiles > 10000 || !limits_.maximumSelection || limits_.maximumSelection > 1024 ||
        !limits_.maximumManifestBytes || limits_.maximumManifestBytes > 256 * 1024 || !limits_.maximumIconBytes || limits_.maximumIconBytes > 512 * 1024 ||
        !limits_.maximumArchiveBytes || limits_.maximumArchiveBytes > 512ULL * 1024 * 1024 || !limits_.maximumConcurrent || limits_.maximumConcurrent > 8) fail("invalid_input", "Invalid bounded Mod service limits");
}

InstanceContext ModsService::context(const InstanceIdentity& instance) const {
    if (!services_.resources.context) fail("unsupported", "Shared instance context service is unavailable");
    auto value = services_.resources.context(instance);
    if (value.identity != instance || value.modsRoot.empty() || value.revision.empty()) fail("policy_denied", "Invalid resolved Mods context");
    return value;
}

std::vector<ModEntry> ModsService::list(const InstanceIdentity& instance, const Cancellation& cancel) {
    return listLocal(context(instance).modsRoot, limits_, cancel);
}

std::vector<ModEntry> ModsService::scan(const InstanceIdentity& instance, const Cancellation& cancel) {
    const auto selectedContext = context(instance);
    auto entries = listLocal(selectedContext.modsRoot, limits_, cancel);
    std::vector<std::string> identities(entries.size());
    std::atomic<std::size_t> next{0};
    std::exception_ptr failure;
    std::mutex mutex;
    Cancellation stopped;
    std::stop_callback parentStopped(cancel.stop_token(), [&] { stopped.cancel(); });
    std::vector<std::jthread> workers;
    const auto workerCount = std::min<std::size_t>(limits_.maximumConcurrent, entries.size());
    for (std::size_t worker = 0; worker < workerCount; ++worker) workers.emplace_back([&] {
        try {
            for (;;) {
                stopped.check();
                const auto index = next.fetch_add(1);
                if (index >= entries.size()) return;
                auto& entry = entries[index];
                for (const auto& physical : entry.physicalFiles) {
                    const auto parsed = localMetadata(selectedContext.modsRoot, physical, selectedContext.loader, services_, limits_, stopped);
                    if (!parsed) continue;
                    entry.name = parsed->name;
                    entry.description = parsed->description;
                    entry.version = parsed->version;
                    entry.modIds = parsed->ids;
                    entry.tags = parsed->tags;
                    entry.iconEntry = parsed->iconEntry;
                    entry.metadataLoaded = true;
                    identities[index] = parsed->identityKey;
                    break;
                }
            }
        } catch (...) { std::lock_guard lock(mutex); if (!failure) failure = std::current_exception(); stopped.cancel(); }
    });
    for (auto& worker : workers) worker.join();
    cancel.check();
    if (failure) std::rethrow_exception(failure);
    std::map<std::string, std::size_t> counts;
    for (const auto& identity : identities) if (!identity.empty()) ++counts[identity];
    for (std::size_t i = 0; i < entries.size(); ++i) if (counts[identities[i]] > 1) entries[i].duplicateGroup = identities[i];
    if (context(instance).revision != selectedContext.revision) fail("conflict", "Instance changed during Mod scan");
    return entries;
}

std::optional<ModMetadata> ModsService::metadata(const InstanceIdentity& instance, const std::string& name, const Cancellation& cancel) {
    const auto selectedContext = context(instance);
    return localMetadata(selectedContext.modsRoot, name, selectedContext.loader, services_, limits_, cancel);
}

Bytes ModsService::icon(const InstanceIdentity& instance, const std::string& name, const std::string& iconEntry, const Cancellation& cancel) {
    if (!validArchiveRelativePath(iconEntry) || iconEntry.ends_with('/')) fail("policy_denied", "Unapproved archive icon path");
    const auto selectedContext = context(instance);
    const auto metadata = localMetadata(selectedContext.modsRoot, name, selectedContext.loader, services_, limits_, cancel);
    if (!metadata || metadata->iconEntry != iconEntry) fail("policy_denied", "Icon was not approved by the current Mod manifest");
    const auto path = detail::checkedLeaf(selectedContext.modsRoot, name);
    detail::FileReader pin(path, limits_.maximumArchiveBytes);
    auto bytes = services_.readArchiveEntry(path, iconEntry, limits_.maximumIconBytes, cancel);
    if (bytes.empty() || bytes.size() > limits_.maximumIconBytes) fail("too_large", "Mod icon exceeds byte budget");
    pin.verifyUnchanged();
    cancel.check();
    return bytes;
}

Strings ModsService::setEnabled(const InstanceIdentity& instance, const Strings& fileNames, bool enabled, const Cancellation& cancel) {
    const auto selectedContext = context(instance);
    const auto lease = mutationLease(services_.resources, selectedContext);
    const auto root = detail::checkedDirectory(selectedContext.modsRoot);
    const auto selection = selected(root, fileNames, limits_, cancel);
    const auto groups = enumerate(root, limits_, cancel);
    std::set<std::string> processed, removals;
    std::vector<Rename> changes;
    Strings result;
    for (const auto& name : selection) {
        const auto key = lower(splitModFileName(name)->first);
        if (!processed.insert(key).second) continue;
        const auto found = groups.find(key);
        if (found == groups.end()) fail("conflict", "Mod logical group disappeared");
        const auto& group = found->second;
        std::vector<ModFile> active, disabled, old;
        for (const auto& file : group) (file.state == ModState::Enabled ? active : file.state == ModState::Disabled ? disabled : old).push_back(file);
        if (active.size() > 1 || disabled.size() > 1 || old.size() > 1) fail("conflict", "Mod has ambiguous case-colliding physical copies");
        if (enabled) {
            const auto& source = !disabled.empty() ? disabled.front() : !old.empty() ? old.front() : active.front();
            const auto target = !active.empty() ? active.front().name : source.base;
            if (!active.empty() && source.name != active.front().name) {
                if (!filesEqual(root / pathFromUtf8(source.name), root / pathFromUtf8(active.front().name), limits_, cancel)) fail("conflict", "Enabled and disabled Mod copies contain different versions");
                removals.insert(source.name);
            } else if (active.empty()) changes.push_back({source.name, target});
            for (const auto& file : group) {
                if (file.name == source.name || file.name == target || removals.contains(file.name)) continue;
                if (filesEqual(root / pathFromUtf8(source.name), root / pathFromUtf8(file.name), limits_, cancel)) removals.insert(file.name);
            }
            result.push_back(target);
        } else {
            if (active.empty()) { result.push_back(name); continue; }
            const auto& source = active.front();
            auto target = source.base + ".disabled";
            if (!disabled.empty()) {
                if (filesEqual(root / pathFromUtf8(source.name), root / pathFromUtf8(disabled.front().name), limits_, cancel)) { removals.insert(source.name); target = disabled.front().name; }
                else if (!old.empty()) {
                    if (!filesEqual(root / pathFromUtf8(source.name), root / pathFromUtf8(old.front().name), limits_, cancel)) fail("conflict", "Three different Mod copies cannot be disabled without data loss");
                    removals.insert(source.name);
                    target = disabled.front().name;
                } else { target = source.base + ".old"; changes.push_back({source.name, target}); }
            } else changes.push_back({source.name, target});
            for (const auto& file : group) {
                if (file.name == source.name || file.name == target || removals.contains(file.name)) continue;
                if (filesEqual(root / pathFromUtf8(source.name), root / pathFromUtf8(file.name), limits_, cancel)) removals.insert(file.name);
            }
            result.push_back(target);
        }
    }
    std::vector<Rename> all;
    Strings trash;
    for (const auto& name : removals) { const auto temporary = detail::temporaryName("dedup"); all.push_back({name, temporary}); trash.push_back(temporary); }
    all.insert(all.end(), changes.begin(), changes.end());
    if (context(instance).revision != selectedContext.revision) fail("conflict", "Instance changed before Mod state commit");
    applyRenames(root, all, cancel);
    // Duplicate deletion occurs only after the complete desired state is committed.
    for (const auto& name : trash) detail::removeRegular(root, name);
    return result;
}

std::uint64_t ModsService::remove(const InstanceIdentity& instance, const Strings& fileNames, const Cancellation& cancel) {
    const auto selectedContext = context(instance);
    const auto lease = mutationLease(services_.resources, selectedContext);
    const auto root = detail::checkedDirectory(selectedContext.modsRoot);
    const auto selection = selected(root, fileNames, limits_, cancel);
    const auto groups = enumerate(root, limits_, cancel);
    std::set<std::string> files;
    for (const auto& name : selection) for (const auto& file : groups.at(lower(splitModFileName(name)->first))) files.insert(file.name);
    if (files.size() > limits_.maximumSelection) fail("too_large", "Physical Mod deletion selection exceeds budget");
    std::vector<Rename> moves;
    for (const auto& name : files) moves.push_back({name, detail::temporaryName("delete")});
    if (context(instance).revision != selectedContext.revision) fail("conflict", "Instance changed before Mod deletion commit");
    applyRenames(root, moves, cancel);
    for (const auto& move : moves) detail::removeRegular(root, move.to);
    return files.size();
}

Strings ModsService::installLocal(const InstanceIdentity& instance, const std::vector<Path>& nativeDialogPaths, const Cancellation& cancel) {
    const auto selectedContext = context(instance);
    const auto lease = mutationLease(services_.resources, selectedContext);
    if (nativeDialogPaths.empty() || nativeDialogPaths.size() > limits_.maximumSelection) fail("invalid_input", "Select a bounded non-empty set of local Mods");
    if (!services_.archiveEntries) fail("unsupported", "Shared bounded archive reader is unavailable");
    const auto root = detail::checkedDirectory(selectedContext.modsRoot, true);
    Strings names;
    std::vector<Path> sources;
    for (const auto& raw : nativeDialogPaths) {
        cancel.check();
        const auto path = detail::checkedRegularFile(raw);
        const auto name = utf8Path(path.filename());
        if (!splitModFileName(name) || contains(names, name, true)) fail("invalid_input", "Invalid or case-colliding local Mod filenames");
        detail::checkedMissingLeaf(root, name);
        detail::FileReader pin(path, limits_.maximumArchiveBytes);
        validateArchive(services_.archiveEntries(path, cancel));
        pin.verifyUnchanged();
        names.push_back(name);
        sources.push_back(path);
    }
    Strings staged;
    std::vector<Rename> changes;
    try {
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto temporary = detail::temporaryName("install");
            detail::copyExclusive(sources[i], root, temporary, limits_.maximumArchiveBytes, cancel);
            staged.push_back(temporary);
            detail::FileReader pin(root / pathFromUtf8(temporary), limits_.maximumArchiveBytes);
            validateArchive(services_.archiveEntries(root / pathFromUtf8(temporary), cancel));
            pin.verifyUnchanged();
            changes.push_back({temporary, names[i]});
        }
        if (context(instance).revision != selectedContext.revision) fail("conflict", "Instance changed before local Mod installation commit");
        applyRenames(root, changes, cancel);
    } catch (...) {
        const auto failure = std::current_exception();
        for (const auto& name : staged) {
            std::error_code error;
            if (std::filesystem::exists(root / pathFromUtf8(name), error)) detail::removeRegular(root, name);
        }
        std::rethrow_exception(failure);
    }
    return names;
}

Strings ModsService::copyTo(const InstanceIdentity& instance, const Strings& fileNames, const Path& nativeDialogDirectory, const Cancellation& cancel) {
    const auto selectedContext = context(instance);
    const auto root = detail::checkedDirectory(selectedContext.modsRoot);
    const auto target = detail::checkedDirectory(nativeDialogDirectory);
    const auto selection = selected(root, fileNames, limits_, cancel);
    for (const auto& name : selection) detail::checkedMissingLeaf(target, name);
    Strings staged;
    std::vector<Rename> changes;
    try {
        for (const auto& name : selection) {
            const auto temporary = detail::temporaryName("copy");
            detail::copyExclusive(detail::checkedLeaf(root, name), target, temporary, limits_.maximumArchiveBytes, cancel);
            staged.push_back(temporary);
            changes.push_back({temporary, name});
        }
        applyRenames(target, changes, cancel);
    } catch (...) {
        const auto failure = std::current_exception();
        for (const auto& name : staged) {
            std::error_code error;
            if (std::filesystem::exists(target / pathFromUtf8(name), error)) detail::removeRegular(target, name);
        }
        std::rethrow_exception(failure);
    }
    return selection;
}

std::vector<ModUpdate> ModsService::checkUpdates(const InstanceIdentity& instance, const Strings& fileNames, const Cancellation& cancel) {
    const auto selectedContext = context(instance);
    if (!community_ || selectedContext.updatesDisabled || selectedContext.minecraftVersion.empty() || defaultSearchLoader(selectedContext.loader).empty()) return {};
    const auto root = detail::checkedDirectory(selectedContext.modsRoot);
    const auto names = selected(root, fileNames, limits_, cancel);
    std::vector<ModUpdate> result(names.size());
    std::atomic<std::size_t> next{0};
    Cancellation stopped;
    std::stop_callback parentStopped(cancel.stop_token(), [&] { stopped.cancel(); });
    std::exception_ptr failure;
    std::mutex mutex;
    std::vector<std::jthread> workers;
    for (std::size_t worker = 0; worker < std::min<std::size_t>(limits_.maximumConcurrent, names.size()); ++worker) workers.emplace_back([&] {
        try {
            for (;;) {
                stopped.check();
                const auto index = next.fetch_add(1);
                if (index >= names.size()) return;
                const auto hashes = hashIdentity(services_, detail::checkedLeaf(root, names[index]), limits_, stopped);
                auto& update = result[index];
                update.fileName = names[index];
                update.originalSha256 = hashes.sha256;
                update.instance = instance;
                update.instanceRevision = selectedContext.revision;
                const auto probe = [&](Source source) -> ModUpdate {
                    ModUpdate candidate = update;
                    try {
                        std::optional<ProjectVersion> current;
                        if (source == Source::Modrinth) current = community_->lookupModrinthHash(hashes.sha1, stopped);
                        else current = community_->lookupCurseForgeFingerprint(hashes.curseForgeFingerprint, stopped);
                        if (!current) return candidate;
                        const bool exact = std::any_of(current->files.begin(), current->files.end(), [&](const auto& file) {
                            return file.sha1 == hashes.sha1 && (file.sha512.empty() || file.sha512 == hashes.sha512) && file.sizeBytes == hashes.sizeBytes;
                        });
                        if (!exact) return candidate;
                        candidate.identified = true;
                        const auto versions = community_->allVersions(source, ResourceKind::Mod, current->projectId, stopped, true);
                        std::optional<ProjectVersion> newest;
                        for (const auto& version : versions) {
                            if (version.projectId != current->projectId || version.id == current->id || version.publishedAt.empty() || current->publishedAt.empty() ||
                                version.publishedAt <= current->publishedAt || !contains(version.gameVersions, selectedContext.minecraftVersion) ||
                                !contains(version.loaders, selectedContext.loader, true)) continue;
                            if (!newest || version.publishedAt > newest->publishedAt || (version.publishedAt == newest->publishedAt && version.id < newest->id)) newest = version;
                        }
                        if (newest && std::none_of(newest->files.begin(), newest->files.end(), [&](const auto& file) { return file.sha1 == hashes.sha1; })) candidate.available = community_->prepareDownload(source, ResourceKind::Mod, newest->projectId, newest->id, stopped);
                    } catch (const ErrorException& error) {
                        if (error.error().code == "cancelled" || error.error().code == "integrity" || error.error().code == "policy_denied") throw;
                        candidate.providerErrors.emplace(source, error.error());
                    }
                    return candidate;
                };
                const auto first = probe(Source::Modrinth);
                const auto second = probe(Source::CurseForge);
                // Deterministic preference never depends on which provider replies first.
                update = first.available ? first : second.available ? second : first.identified ? first : second;
                update.providerErrors.insert(first.providerErrors.begin(), first.providerErrors.end());
                update.providerErrors.insert(second.providerErrors.begin(), second.providerErrors.end());
            }
        } catch (...) { std::lock_guard lock(mutex); if (!failure) failure = std::current_exception(); stopped.cancel(); }
    });
    for (auto& worker : workers) worker.join();
    cancel.check();
    if (failure) std::rethrow_exception(failure);
    if (!sameContext(selectedContext, context(instance))) fail("conflict", "Instance compatibility changed during Mod update identification");
    return result;
}

std::vector<InstallReceipt> ModsService::update(const InstanceIdentity& instance, const Strings& fileNames,
                                               const Cancellation& cancel, const ProgressCallback& progress) {
    return updateConfirmed(instance, checkUpdates(instance, fileNames, cancel), cancel, progress);
}

void ModsService::validateConfirmedUpdates(const InstanceContext& selected, const std::vector<ModUpdate>& updates) const {
    if (updates.empty() || updates.size() > limits_.maximumSelection)
        fail("invalid_input", "Select a bounded non-empty set of confirmed updates");
    std::set<std::string> selectedNames;
    for (const auto& update : updates) {
        if (update.instance != selected.identity || update.instanceRevision != selected.revision ||
            checkedHash(update.originalSha256, 32).empty() || !splitModFileName(update.fileName) ||
            !selectedNames.insert(lower(update.fileName)).second)
            fail("conflict", "Confirmed update identity, revision or selection is invalid");
    }
}

std::shared_ptr<void> ModsService::acquireConfirmedMutation(const InstanceContext& selected) const {
    return mutationLease(services_.resources, selected);
}


DownloadPlan ModsService::prepareConfirmedUpdate(const InstanceContext& selected, const ModUpdate& update,
                                                const Cancellation& cancel,
                                                const std::function<DownloadPlan(const DownloadPlan&)>& prepare) const {
    cancel.check();
    if (!update.available) fail("not_found", "No compatible update was confirmed for this Mod");
    if (!sameContext(selected, context(selected.identity)))
        fail("conflict", "Instance compatibility changed before Mod replacement");
    const auto root = detail::checkedDirectory(selected.modsRoot);
    const auto path = detail::checkedLeaf(root, update.fileName);
    const auto currentHash = hashIdentity(services_, path, limits_, cancel);
    if (currentHash.sha256 != update.originalSha256) fail("conflict", "Mod changed after its update was identified");
    auto plan = prepare ? prepare(*update.available) : community_->prepareDownload(update.available->source,
        ResourceKind::Mod, update.available->projectId, update.available->versionId, cancel);
    if (plan.file.sha1 != update.available->file.sha1 || plan.file.sha512 != update.available->file.sha512 ||
        plan.file.sizeBytes != update.available->file.sizeBytes)
        fail("conflict", "Provider changed the confirmed update file");
    const auto split = splitModFileName(update.fileName);
    if (split->second == ModState::Disabled) plan.file.fileName += ".disabled";
    else if (split->second == ModState::Old) plan.file.fileName += ".old";
    if (!validResourceFileName(plan.file.fileName)) fail("integrity", "Updated Mod state filename is invalid");
    cancel.check();
    return plan;
}

std::vector<InstallReceipt> ModsService::updateConfirmed(const InstanceIdentity& instance, const std::vector<ModUpdate>& updates,
                                                        const Cancellation& cancel, const ProgressCallback& progress) {
    cancel.check();
    if (!services_.replace || !services_.hashes || !community_)
        fail("unsupported", "Shared verified Mod replacement transaction is unavailable");
    const auto selected = context(instance);
    if (selected.updatesDisabled || selected.running)
        fail("policy_denied", "Mod updates are disabled or the instance is running");
    validateConfirmedUpdates(selected, updates);
    const auto lease = acquireConfirmedMutation(selected);
    std::vector<InstallReceipt> receipts;
    receipts.reserve(updates.size());
    for (const auto& update : updates) {
        cancel.check();
        if (!update.available) continue;
        auto plan = prepareConfirmedUpdate(selected, update, cancel);
        // The retained authority spans preparation, download and final identity checks.
        receipts.push_back(services_.replace(selected, update.fileName, update.originalSha256, plan, lease, cancel, progress));
    }
    if (receipts.empty()) fail("not_found", "No compatible updates were confirmed for the selected Mods");
    return receipts;
}

} // namespace nickel::resources
