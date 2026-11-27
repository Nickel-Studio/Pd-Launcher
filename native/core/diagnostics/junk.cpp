#include "core/diagnostics/junk.hpp"

#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"
#include "core/tasks/secure_fs.hpp"
#include "core/tasks/test_hooks.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <system_error>
#include <utility>

namespace nickel::diagnostics {
namespace {
namespace fs = std::filesystem;
namespace safety = nickel::foundation_detail;
namespace secure = nickel::tasks::detail;
using Identity = secure::FileIdentity;
using Clock = std::chrono::steady_clock;
constexpr std::size_t metadataLimit = 8U * 1024U * 1024U;
constexpr std::size_t pathLimit = 32760;

[[noreturn]] void fail(std::string_view code, std::string_view message) {
    throw ErrorException(Error{std::string(code), std::string(message)});
}

template<class F>
decltype(auto) boundary(F&& action) {
    try {
        return std::forward<F>(action)();
    } catch (const ErrorException&) {
        throw;
    } catch (const std::exception&) {
        fail("cleanup_failed", "Safe cleanup could not complete");
    }
}

void checkCancelled(std::stop_token cancellation) {
    if (cancellation.stop_requested()) fail("Cancelled", "Cleanup preview was cancelled");
}

bool absent(const std::error_code& error) {
    return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
}

bool missing(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(safety::native_filesystem_path(path), error);
    if (absent(error) || status.type() == fs::file_type::not_found) return true;
    if (error) throw fs::filesystem_error("inspect cleanup path", path, error);
    return false;
}

std::string pathKey(const fs::path& path) {
    const auto text = path_to_utf8(path);
#ifdef _WIN32
    return secure::case_key(text);
#else
    return text;
#endif
}

fs::path logicalPath(const fs::path& input) {
    if (!input.is_absolute()) fail("cleanup_invalid_path", "Cleanup locations must be absolute");
    if (path_to_utf8(input).size() > pathLimit) fail("LimitExceeded", "Cleanup path exceeds its byte budget");
    auto result = safety::checked_absolute_path(input);
    while (result != result.root_path() && result.filename().empty()) result = result.parent_path();
    if (result == result.root_path()) fail("cleanup_invalid_path", "A filesystem root is not a cleanup target");
    return result;
}

struct Anchor {
    fs::path path;
    Identity identity;
};

Anchor directoryAnchor(const fs::path& input) {
    const auto path = logicalPath(input);
    safety::DirectoryGuard guard(path);
    reject_path_links(path, true);
    const auto identity = secure::directory_identity(path);
    guard.verify();
    return {path, identity};
}

void verifyAnchor(const Anchor& anchor) {
    reject_path_links(anchor.path, true);
    if (secure::directory_identity(anchor.path) != anchor.identity) {
        fail("Conflict", "Cleanup directory identity changed");
    }
}

bool regularMarker(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(safety::native_filesystem_path(path), error);
    if (absent(error) || status.type() == fs::file_type::not_found) return false;
    if (error) throw fs::filesystem_error("inspect game marker", path, error);
    if (!fs::is_regular_file(status)) return false;
    reject_path_links(path, false);
    safety::reject_file_hardlinks(path);
    return true;
}

bool gameLike(const Anchor& root) {
    safety::DirectoryGuard guard(root.path);
    verifyAnchor(root);
    bool versions = false;
    try {
        const auto marker = directoryAnchor(root.path / "versions");
        (void)marker;
        versions = true;
    } catch (const std::exception&) {
        // A different independent legacy marker can still authorize the real root.
    }
    const bool found = versions || regularMarker(root.path / "options.txt") || regularMarker(root.path / "level.dat");
    guard.verify();
    return found;
}

bool validVersion(std::string_view name) {
    if (name.empty() || name.size() > 160 || !valid_utf8(name) || name == "." ||
        name.find("..") != std::string_view::npos || name.back() == '.' ||
        name.front() == ' ' || name.back() == ' ') return false;
    for (const auto byte : name) {
        const auto code = static_cast<unsigned char>(byte);
        if (code < 0x20 || code == 0x7f || std::string_view("<>:\"/\\|?*").find(byte) != std::string_view::npos) return false;
    }
    try {
        const auto leaf = path_from_utf8(name);
        return leaf.filename() == leaf && !leaf.has_root_path();
    } catch (const std::exception&) {
        return false;
    }
}

struct Directory {
    Anchor root;
    Anchor effective;
    std::optional<Anchor> versions;
    bool isolated{};
};

void verifyDirectory(const Directory& directory) {
    verifyAnchor(directory.root);
    if (directory.versions) verifyAnchor(*directory.versions);
    verifyAnchor(directory.effective);
    if (directory.isolated && !missing(directory.effective.path / ".nickel-pack.pending")) {
        fail("Conflict", "Cleanup instance became pending");
    }
}

std::optional<JunkKind> rootKind(std::string_view name) {
    if (name == "authlib-injector.log") return JunkKind::AuthlibLog;
    if ((name.starts_with("hs_err_pid") || name.starts_with("replay_pid")) && name.ends_with(".log")) return JunkKind::JvmLog;
    return {};
}

bool configTemporary(std::string_view name) {
    return (name.starts_with(".nickel-") && name.ends_with(".part")) || name.starts_with("instances.json.tmp-");
}

bool currentLog(std::string_view name) {
#ifdef _WIN32
    return secure::case_key(name) == "nickel-launcher.log";
#else
    return name == "nickel-launcher.log";
#endif
}

void validateLimits(const JunkLimits& limits) {
    if (limits.maxRoots == 0 || limits.maxRoots > 256 || limits.maxInstances == 0 || limits.maxInstances > 4096 ||
        limits.maxScannedEntries == 0 || limits.maxScannedEntries > 100000 || limits.maxFiles == 0 || limits.maxFiles > 10000 ||
        limits.maxRetainedPreviews == 0 || limits.maxRetainedPreviews > 4 || limits.maxFileBytes == 0 ||
        limits.maxFileBytes > 1024ULL * 1024ULL * 1024ULL || limits.maxTotalBytes == 0 ||
        limits.maxTotalBytes > 4ULL * 1024ULL * 1024ULL * 1024ULL || limits.previewLifetime.count() <= 0 ||
        limits.previewLifetime > std::chrono::hours{1}) {
        fail("cleanup_invalid_limits", "Cleanup limits are outside bounded supported values");
    }
}

void accountMetadata(std::size_t& current, std::size_t bytes) {
    if (bytes > metadataLimit - current) fail("LimitExceeded", "Cleanup authorization metadata budget exceeded");
    current += bytes;
}

std::string issueCode(const std::exception& error) {
    if (const auto* common = dynamic_cast<const ErrorException*>(&error)) return common->error().code;
    return "cleanup_unsafe_or_unavailable_location";
}

struct Registered {
    JunkLocations locations;
    std::uint64_t revision{};
    std::vector<std::shared_ptr<const Directory>> games;
    std::shared_ptr<const Directory> config;
};

struct AuthorizedFile {
    JunkKind kind{};
    std::shared_ptr<const Directory> directory;
    Anchor parent;
    secure::RegularFileToken token;
};

struct PreviewRecord {
    JunkPreview publicPreview;
    std::shared_ptr<const Registered> registered;
    std::vector<AuthorizedFile> files;
    Clock::time_point expires;
};

AuthorizedFile capture(const fs::path& path, JunkKind kind, const std::shared_ptr<const Directory>& directory,
                       const Anchor& parent, const JunkLimits& limits, std::uint64_t remainingBytes,
                       std::stop_token cancellation) {
    checkCancelled(cancellation);
    verifyDirectory(*directory);
    verifyAnchor(parent);
    reject_path_links(path, true);
    safety::reject_file_hardlinks(path);
    auto token = [&] {
        try {
            return secure::capture_regular_file(directory->root.path, path,
                std::min(limits.maxFileBytes, remainingBytes), [&] {
                    checkCancelled(cancellation);
                    verifyDirectory(*directory);
                    verifyAnchor(parent);
                    return cancellation.stop_requested();
                });
        } catch (const ErrorException& error) {
            // Quotas and cancellation abort the whole preview instead of skipping a candidate.
            if (error.error().code == "file_limits") fail("LimitExceeded", "Cleanup file exceeds its remaining byte quota");
            if (error.error().code == "cancelled") fail("Cancelled", "Cleanup preview was cancelled");
            throw;
        }
    }();
    if (token.root != directory->root.path || token.path != path ||
        token.root_identity != directory->root.identity || token.parent_identity != parent.identity) {
        fail("Conflict", "Cleanup capture changed its authorized locations");
    }
    reject_path_links(path, true);
    safety::reject_file_hardlinks(path);
    verifyAnchor(parent);
    verifyDirectory(*directory);
    return {kind, directory, parent, std::move(token)};
}

enum class RemovalOutcome { Committed, PersistencePending };

RemovalOutcome removeAuthorizedFile(const AuthorizedFile& item, const std::function<bool()>& cancelled) {
    try {
        secure::remove_regular_file(item.token, cancelled);
        return RemovalOutcome::Committed;
    } catch (const ErrorException& error) {
        if (error.error().code != "remove_persistence_pending") throw;
        const auto& details = error.error().details;
        if (!details.is_object()) throw;
        const auto committed = details.find("committed");
        const auto removed = details.find("removed");
        const auto pending = details.find("durabilityPending");
        const auto bytes = details.find("removedBytes");
        if (committed == details.end() || !committed->is_boolean() || !committed->get<bool>() ||
            removed == details.end() || !removed->is_boolean() || !removed->get<bool>() ||
            pending == details.end() || !pending->is_boolean() || !pending->get<bool>() || bytes == details.end()) throw;
        bool matches = false;
        if (bytes->is_number_unsigned()) matches = bytes->get<std::uint64_t>() == item.token.size;
        else if (bytes->is_number_integer()) {
            const auto value = bytes->get<std::int64_t>();
            matches = value >= 0 && static_cast<std::uint64_t>(value) == item.token.size;
        }
        if (!matches) throw;
        return RemovalOutcome::PersistencePending;
    }
}

} // namespace

class JunkService::Impl {
public:
    JunkLimits limits;
    JunkLocationValidator validator;
    std::mutex mutex;
    std::shared_ptr<const Registered> registered;
    std::deque<PreviewRecord> previews;
    std::uint64_t revision{};

    explicit Impl(JunkLimits values, JunkLocationValidator callback) : limits(values), validator(std::move(callback)) {
        validateLimits(limits);
    }

    std::unique_lock<std::mutex> lock() {
        std::unique_lock result(mutex, std::try_to_lock);
        if (!result.owns_lock()) fail("cleanup_busy", "Another cleanup operation is in progress");
        return result;
    }

    bool sourceMatches(const Registered& value) const {
        if (!validator) return true;
        try { return validator(value.locations); }
        catch (...) { return false; }
    }

    void expire() {
        const auto now = Clock::now();
        std::erase_if(previews, [&](const PreviewRecord& value) { return value.expires <= now; });
    }
};

JunkService::JunkService(JunkLimits limits, JunkLocationValidator validator)
    : impl_(boundary([&] { return std::make_unique<Impl>(limits, std::move(validator)); })) {}
JunkService::~JunkService() = default;

JunkRegistration JunkService::registerLocations(const JunkLocations& locations) {
    return boundary([&] {
        const auto lock = impl_->lock();
        if (locations.gameRoots.size() > impl_->limits.maxRoots) fail("LimitExceeded", "Too many cleanup roots");
        std::size_t instances{}, metadata{};
        const auto pathBytes = [&](const fs::path& path) {
            const auto bytes = path_to_utf8(path).size();
            if (bytes > pathLimit) fail("LimitExceeded", "Cleanup location path is too long");
            accountMetadata(metadata, bytes);
        };
        for (const auto& root : locations.gameRoots) {
            pathBytes(root.root);
            if (root.instances.size() > impl_->limits.maxInstances - instances) fail("LimitExceeded", "Too many cleanup instance bindings");
            instances += root.instances.size();
            for (const auto& instance : root.instances) {
                pathBytes(instance.effectiveDirectory);
                accountMetadata(metadata, instance.versionId.size());
            }
        }
        if (locations.configRoot) pathBytes(*locations.configRoot);
        auto next = std::make_shared<Registered>();
        next->locations = locations;
        JunkRegistration result;
        std::set<std::string> seenRoots, seenGames;
        for (const auto& candidate : locations.gameRoots) {
            try {
                const auto root = directoryAnchor(candidate.root);
                if (!gameLike(root)) fail("cleanup_not_game_root", "Cleanup root has no supported game marker");
                const auto rootKey = pathKey(root.path);
                if (seenRoots.insert(rootKey).second) ++result.acceptedRootCount;
                if (seenGames.insert(rootKey).second) next->games.push_back(std::make_shared<Directory>(Directory{root, root, {}, false}));
                for (const auto& instance : candidate.instances) {
                    try {
                        if (!validVersion(instance.versionId)) fail("cleanup_invalid_version", "Cleanup version identity is invalid");
                        const auto versions = directoryAnchor(root.path / "versions");
                        const auto version = directoryAnchor(versions.path / path_from_utf8(instance.versionId));
                        if (!missing(version.path / ".nickel-pack.pending")) fail("cleanup_pending_instance", "Pending instances cannot be cleaned");
                        const auto effective = logicalPath(instance.effectiveDirectory);
                        const auto key = pathKey(effective);
                        if (key != rootKey && key != pathKey(version.path)) fail("cleanup_invalid_isolation", "Cleanup effective directory is not the registered root or exact version");
                        if (key != rootKey && seenGames.insert(key).second) {
                            next->games.push_back(std::make_shared<Directory>(Directory{root, version, versions, true}));
                        }
                    } catch (const std::exception& error) {
                        result.issues.push_back({instance.effectiveDirectory, issueCode(error)});
                    }
                }
            } catch (const std::exception& error) {
                ++result.skippedRootCount;
                result.issues.push_back({candidate.root, issueCode(error)});
            }
        }
        if (locations.configRoot) {
            try {
                const auto config = directoryAnchor(*locations.configRoot);
                next->config = std::make_shared<Directory>(Directory{config, config, {}, false});
            } catch (const std::exception& error) {
                result.issues.push_back({*locations.configRoot, issueCode(error)});
            }
        }
        if (impl_->revision == std::numeric_limits<std::uint64_t>::max()) fail("cleanup_revision_exhausted", "Cleanup registration revision is exhausted");
        result.revision = ++impl_->revision;
        result.effectiveDirectoryCount = next->games.size();
        next->revision = result.revision;
        impl_->registered = std::move(next);
        impl_->previews.clear();
        return result;
    });
}

JunkPreview JunkService::preview(std::stop_token cancellation) {
    return boundary([&] {
        const auto lock = impl_->lock();
        checkCancelled(cancellation);
        if (!impl_->registered) fail("cleanup_not_registered", "Cleanup locations have not been registered");
        if (!impl_->sourceMatches(*impl_->registered)) fail("Conflict", "Cleanup locations changed before preview");
        PreviewRecord record;
        record.registered = impl_->registered;
        record.publicPreview.revision = record.registered->revision;
        std::set<std::string> seenFiles;
        std::size_t inspected{}, metadata{};
        const auto scan = [&](const std::shared_ptr<const Directory>& directory, const fs::path& path, JunkKind kind) {
            checkCancelled(cancellation);
            if (missing(path)) return;
            try {
                safety::DirectoryGuard guard(path);
                verifyDirectory(*directory);
                const auto parent = directoryAnchor(path);
                for (const auto& entry : fs::directory_iterator(safety::native_filesystem_path(path))) {
                    checkCancelled(cancellation);
                    if (++inspected > impl_->limits.maxScannedEntries) fail("LimitExceeded", "Cleanup scan entry quota exceeded");
                    std::error_code error;
                    const auto status = entry.symlink_status(error);
                    if (error || !fs::is_regular_file(status)) {
                        if (error || fs::is_symlink(status)) ++record.publicPreview.skippedCount;
                        continue;
                    }
                    const auto name = path_to_utf8(entry.path().filename());
                    auto itemKind = kind;
                    if ((kind == JunkKind::GameLog || kind == JunkKind::ConfigLog) && currentLog(name)) continue;
                    if (kind == JunkKind::JvmLog) {
                        const auto matched = rootKind(name);
                        if (!matched) continue;
                        itemKind = *matched;
                    }
                    if (kind == JunkKind::ConfigTemporary && !configTemporary(name)) continue;
                    const auto filePath = logicalPath(path / entry.path().filename());
                    if (!seenFiles.insert(pathKey(filePath)).second) continue;
                    try {
                        if (record.files.size() >= impl_->limits.maxFiles) fail("LimitExceeded", "Cleanup file count quota exceeded");
                        const auto remaining = impl_->limits.maxTotalBytes - record.publicPreview.totalBytes;
                        auto item = capture(filePath, itemKind, directory, parent, impl_->limits, remaining, cancellation);
                        const auto& token = item.token;
                        accountMetadata(metadata, 4 * (path_to_utf8(token.path).size() + path_to_utf8(token.root).size() +
                                                       path_to_utf8(item.parent.path).size()) + 512);
                        record.publicPreview.totalBytes += token.size;
                        const auto revision = std::to_string(token.revision.modified) + ":" +
                                              std::to_string(token.revision.changed) + ":" + token.sha256;
                        record.publicPreview.items.push_back({token.path, item.kind, token.size, revision});
                        record.files.push_back(std::move(item));
                    } catch (const ErrorException& errorValue) {
                        if (errorValue.error().code == "LimitExceeded" || errorValue.error().code == "Cancelled") throw;
                        ++record.publicPreview.skippedCount;
                    } catch (const std::exception&) {
                        ++record.publicPreview.skippedCount;
                    }
                }
                guard.verify();
                verifyDirectory(*directory);
            } catch (const ErrorException& error) {
                if (error.error().code == "LimitExceeded" || error.error().code == "Cancelled") throw;
                ++record.publicPreview.skippedCount;
            } catch (const std::exception&) {
                ++record.publicPreview.skippedCount;
            }
        };
        for (const auto& directory : record.registered->games) {
            scan(directory, directory->effective.path / "logs", JunkKind::GameLog);
            scan(directory, directory->effective.path / "crash-reports", JunkKind::CrashReport);
            scan(directory, directory->effective.path, JunkKind::JvmLog);
        }
        if (const auto& config = record.registered->config) {
            scan(config, config->effective.path / "logs", JunkKind::ConfigLog);
            scan(config, config->effective.path, JunkKind::ConfigTemporary);
        }
        checkCancelled(cancellation);
        if (!impl_->sourceMatches(*record.registered)) fail("Conflict", "Cleanup locations changed during preview");
        record.publicPreview.fileCount = record.files.size();
        record.publicPreview.previewId = secure::unique_id();
        record.expires = Clock::now() + impl_->limits.previewLifetime;
        auto result = record.publicPreview;
        impl_->expire();
        while (impl_->previews.size() >= impl_->limits.maxRetainedPreviews) impl_->previews.pop_front();
        impl_->previews.push_back(std::move(record));
        return result;
    });
}

JunkCleanResult JunkService::clean(std::string_view previewId, std::stop_token cancellation) {
    return boundary([&] {
        const auto lock = impl_->lock();
        if (previewId.empty() || previewId.size() > 128) fail("cleanup_invalid_preview", "Cleanup preview identity is invalid");
        impl_->expire();
        const auto found = std::find_if(impl_->previews.begin(), impl_->previews.end(), [&](const PreviewRecord& value) {
            return value.publicPreview.previewId == previewId;
        });
        if (found == impl_->previews.end()) fail("cleanup_invalid_preview", "Cleanup preview is unknown, expired, or already consumed");
        auto record = std::move(*found);
        impl_->previews.erase(found);
        JunkCleanResult result;
        if (cancellation.stop_requested()) {
            result.cancelled = true;
            result.cancelledCount = record.files.size();
            return result;
        }
        if (record.registered->revision != impl_->revision || !impl_->sourceMatches(*record.registered)) {
            result.locationConflict = true;
            result.conflictCount = record.files.size();
            return result;
        }
        for (std::size_t index = 0; index < record.files.size(); ++index) {
            if (cancellation.stop_requested()) {
                result.cancelled = true;
                result.cancelledCount = record.files.size() - index;
                break;
            }
            if (!impl_->sourceMatches(*record.registered)) {
                result.locationConflict = true;
                result.conflictCount += record.files.size() - index;
                break;
            }
            const auto& item = record.files[index];
            bool committed = false;
            bool persistencePending = false;
            try {
                safety::DirectoryGuard guard(item.parent.path);
                verifyDirectory(*item.directory);
                verifyAnchor(item.parent);
                if (missing(item.token.path)) { ++result.missingCount; continue; }
                reject_path_links(item.token.path, true);
                safety::reject_file_hardlinks(item.token.path);
                const auto cancelled = [&] {
                    if (cancellation.stop_requested()) return true;
                    if (!impl_->sourceMatches(*record.registered)) {
                        fail("cleanup_location_conflict", "Cleanup locations changed during verified deletion");
                    }
                    guard.verify();
                    verifyDirectory(*item.directory);
                    verifyAnchor(item.parent);
                    return cancellation.stop_requested();
                };
                persistencePending = removeAuthorizedFile(item, cancelled) == RemovalOutcome::PersistencePending;
                ++result.removedCount;
                result.freedBytes += item.token.size;
                committed = true;
                if (persistencePending) ++result.failedCount;
                NICKEL_TRANSPORT_CHECKPOINT("cleanup.committed", item.token.path);
            } catch (const ErrorException& error) {
                // A publication hook cannot turn an already removed leaf into a deferred one.
                if (committed) {
                    if (!persistencePending) ++result.failedCount;
                    continue;
                }
                const auto& code = error.error().code;
                if (code == "Cancelled" || code == "cancelled") {
                    result.cancelled = true;
                    result.cancelledCount += record.files.size() - index;
                    break;
                }
                if (code == "cleanup_location_conflict") {
                    result.locationConflict = true;
                    result.conflictCount += record.files.size() - index;
                    break;
                }
                if (code == "Conflict" || code == "UnsafePath" || code == "revision_conflict" ||
                    code == "unsafe_path" || code == "outside_root") ++result.conflictCount;
                else if (code == "NotFound" || code == "not_found") ++result.missingCount;
                else ++result.failedCount;
            } catch (...) {
                if (!committed || !persistencePending) ++result.failedCount;
            }
        }
        return result;
    });
}

void JunkService::discardPreview(std::string_view previewId) {
    boundary([&] {
        const auto lock = impl_->lock();
        if (previewId.size() > 128) fail("cleanup_invalid_preview", "Cleanup preview identity is invalid");
        std::erase_if(impl_->previews, [&](const PreviewRecord& record) { return record.publicPreview.previewId == previewId; });
    });
}

} // namespace nickel::diagnostics
