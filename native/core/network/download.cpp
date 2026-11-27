#include "core/network/download.hpp"
#include "core/network/control.hpp"
#include "core/network/errors.hpp"
#include "core/network/secure_memory.hpp"
#include "core/tasks/secure_fs.hpp"
#include "core/archive/archive.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace nickel::transport {
namespace {
namespace fs = std::filesystem;
namespace sd = tasks::detail;
using Json = nlohmann::json;
struct RangeUnavailable : ErrorException {
    explicit RangeUnavailable(std::string message) : ErrorException(Error{"range_unavailable", std::move(message)}) {}
};
std::string transfer_key(const DownloadRequest& request) {
    detail::IncrementalHash hash(HashAlgorithm::Sha256);
    auto add = [&](std::string_view text) { hash.update(std::as_bytes(std::span(text.data(), text.size()))); };
    add(sd::path_utf8(sd::checked_absolute(request.destination)));
    add(":" + std::to_string(request.expected_size.value_or(std::numeric_limits<std::uint64_t>::max())));
    add(":" + std::to_string(request.max_bytes));
    for (const auto& digest : request.digests) add(":" + std::to_string(static_cast<int>(digest.algorithm)) + ":" + digest.hex);
    return hash.finish().substr(0, 32);
}
Json identity_json(sd::FileIdentity value) { return {{"volume", value.volume}, {"index", value.index}}; }
sd::FileIdentity identity_from(const Json& value) {
    return {value.at("volume").get<std::uint64_t>(), value.at("index").get<std::uint64_t>()};
}
std::string as_text(const std::vector<std::byte>& bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }
std::vector<HashAlgorithm> algorithms(const std::vector<Digest>& digests) {
    std::vector<HashAlgorithm> out;
    for (const auto& digest : digests) out.push_back(digest.algorithm);
    if (out.empty()) out.push_back(HashAlgorithm::Sha256);
    return out;
}
// The staged file still carries the .part suffix, so archive validation keys off the
// requested destination extension; a custom validator replaces that generic check.
void verify(const DownloadRequest& request, const fs::path& path, const Cancellation& cancel,
             const std::vector<Digest>& actual) {
    detail::check_cancel(detail::cancellation_check(cancel));
    auto file = sd::SecureFile::open_read(path);
    const auto size = file.size();
    if (size > request.max_bytes || (request.expected_size && size != *request.expected_size))
        throw std::runtime_error("artifact size verification failed");
    for (const auto& expected : request.digests) {
        auto found = std::find_if(actual.begin(), actual.end(), [&](const auto& item) { return item.algorithm == expected.algorithm; });
        if (found == actual.end() || !detail::digest_equal(expected.hex, found->hex))
            throw std::runtime_error("artifact checksum verification failed");
    }
    file.close();
    if (request.validator) request.validator(path, cancel);
    const auto extension = path.extension().string();
    const auto requested_extension = request.destination.extension().string();
    (void)extension;
    if (!request.validator && (requested_extension == ".jar" || requested_extension == ".zip" || requested_extension == ".mrpack")) {
        archive::Limits limits;
        limits.max_archive_bytes = request.max_bytes;
        archive::Archive::validate(path, limits, cancel);
    }
    detail::check_cancel(detail::cancellation_check(cancel));
}
struct Range {
    std::uint64_t start{}, end{}, total{};
};
Range parse_range(std::string_view value) {
    if (!value.starts_with("bytes ")) throw RangeUnavailable("range Content-Range missing");
    value.remove_prefix(6);
    const auto dash = value.find('-'), slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || slash <= dash) throw RangeUnavailable("malformed Content-Range");
    const auto parse = [](std::string_view text) {
        std::uint64_t out{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
        if (error != std::errc{} || end != text.data() + text.size()) throw RangeUnavailable("malformed Content-Range number");
        return out;
    };
    Range range{parse(value.substr(0, dash)), parse(value.substr(dash + 1, slash - dash - 1)), parse(value.substr(slash + 1))};
    if (range.start > range.end || range.end >= range.total) throw RangeUnavailable("invalid Content-Range bounds");
    return range;
}
void verify_range(const ResponseHead& response, Range expected, std::string_view etag) {
    if (response.status != 206) throw RangeUnavailable("server did not honor a range");
    const auto actual = parse_range(response.header("content-range"));
    if (actual.start != expected.start || actual.end != expected.end || actual.total != expected.total ||
        (response.content_length && *response.content_length != expected.end - expected.start + 1) ||
        (!etag.empty() && response.header("etag") != etag)) throw RangeUnavailable("range metadata or entity changed");
}
struct DownloadProgress {
    std::mutex mutex;
    std::atomic<std::uint64_t> transferred{}, completed{};
    std::optional<std::uint64_t> total;
    Progress callback;
    std::chrono::steady_clock::time_point sample{std::chrono::steady_clock::now()};
    std::uint64_t sampled{};
    void bytes(std::uint64_t transferred_delta, std::uint64_t completed_delta) {
        transferred.fetch_add(transferred_delta); completed.fetch_add(completed_delta);
        if (!callback) return;
        std::lock_guard lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - sample).count();
        const auto bytes = transferred.load();
        const auto speed = elapsed > 0.1 ? (bytes - sampled) / elapsed : 0.0;
        std::optional<std::chrono::milliseconds> remaining;
        if (total && speed > 0 && completed.load() < *total)
            remaining = std::chrono::milliseconds(static_cast<std::int64_t>((*total - completed.load()) * 1000 / speed));
        callback({bytes, completed.load(), total, speed, remaining});
        if (elapsed > 0.1) { sample = now; sampled = bytes; }
    }
};
// Workspace reuse is authorised by the journal plus the recorded directory/file
// identities; a foreign or replaced workspace is refused rather than cleaned up.
struct Stage {
    fs::path workspace, file_path, journal_path;
    sd::FileIdentity workspace_identity, file_identity;
    std::unique_ptr<sd::ParentGuard> parent;
    Json journal;
    bool owns{}, preserve{};
    Stage(const DownloadRequest& request) {
        const auto destination = sd::checked_absolute(request.destination);
        parent = std::make_unique<sd::ParentGuard>(destination.parent_path(), false, sd::GuardMode::NamespaceMutation);
        workspace = destination.parent_path() / sd::utf8_path(".nickel-download-" + transfer_key(request));
        file_path = workspace / "artifact.part"; journal_path = workspace / "journal.json";
        if (fs::exists(sd::io_path(workspace))) {
            journal = Json::parse(as_text(sd::read_bounded(journal_path, 1024 * 1024)));
            if (journal.at("schema") != 1 || journal.at("kind") != "download" ||
                journal.at("destination").get<std::string>() != sd::path_utf8(destination))
                throw std::runtime_error("refusing foreign download workspace");
            workspace_identity = identity_from(journal.at("workspaceIdentity"));
            if (sd::directory_identity(workspace) != workspace_identity) throw std::runtime_error("download workspace identity changed");
            file_identity = identity_from(journal.at("fileIdentity"));
            auto file = sd::SecureFile::open_owned(file_path, file_identity);
            if (journal.value("state", std::string()) == "committing") throw std::runtime_error("download publication requires recovery");
            if (!request.resume) throw std::runtime_error("existing partial download requires resume or explicit owned cleanup");
        } else {
            sd::create_directory_new(workspace); workspace_identity = sd::directory_identity(workspace);
            auto file = sd::SecureFile::create_new(file_path); file_identity = file.identity();
            journal = {{"schema", 1}, {"kind", "download"}, {"destination", sd::path_utf8(destination)},
                {"workspaceIdentity", identity_json(workspace_identity)}, {"fileIdentity", identity_json(file_identity)},
                {"state", "partial"}, {"url", ""}, {"etag", ""}, {"size", 0}};
            file.flush(); file.close(); save();
        }
        owns = true;
    }
    ~Stage() {
        if (owns && !preserve) {
            try { sd::remove_owned_tree(workspace, workspace_identity); } catch (...) { }
        }
    }
    void save() { sd::write_atomic(journal_path, journal.dump()); }
    sd::SecureFile open() { return sd::SecureFile::open_owned(file_path, file_identity); }
    void reset() { auto file = open(); file.truncate(0); file.flush(); journal["url"] = ""; journal["etag"] = ""; journal["size"] = 0; save(); }
};

// Resume is refused on URL change, absent/weak ETag or an oversized partial; the server
// must answer 206 with the same strong ETag, otherwise RangeUnavailable forces a restart.
void single_download(HttpClient& client, const NetworkSnapshot& snapshot, const RequestCandidate& candidate,
                      const DownloadRequest& request, Stage& stage, const Cancellation& cancel,
                      DownloadProgress& progress, bool& resumed, std::vector<Digest>& hashes) {
    auto file = stage.open();
    std::uint64_t offset = file.size();
    if (offset > request.max_bytes || (request.expected_size && offset > *request.expected_size)) offset = 0;
    const auto saved_url = stage.journal.value("url", std::string());
    const auto saved_etag = stage.journal.value("etag", std::string());
    if (!request.resume || saved_url != candidate.url || saved_etag.empty() || saved_etag.starts_with("W/")) offset = 0;
    if (!offset) file.truncate(0);
    std::vector<detail::IncrementalHash> digests;
    for (auto algorithm : algorithms(request.digests)) digests.emplace_back(algorithm);
    if (offset) {
        std::array<std::byte, 64 * 1024> buffer;
        file.seek(0); std::uint64_t read{};
        while (read < offset) {
            detail::check_cancel(detail::cancellation_check(cancel));
            const auto count = file.read(std::span(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), offset - read))));
            if (!count) throw std::runtime_error("partial artifact truncated");
            for (auto& digest : digests) digest.update(std::span(buffer).first(count)); read += count;
        }
        progress.completed = offset; resumed = true;
    }
    file.seek(offset);
    HttpRequest http; detail::EraseGuard erased_http(http);
    http.url = candidate.url; detail::copy_sensitive(http.headers, candidate.headers); http.trust = request.trust;
    http.max_response_bytes = request.max_bytes - offset; http.retry.max_attempts = 1;
    if (offset) {
        http.headers.push_back({"Range", "bytes=" + std::to_string(offset) + "-"});
        http.headers.push_back({"If-Range", saved_etag});
    }
    std::uint64_t bytes{}, actual_total{};
    try {
        const auto response = client.stream(http, snapshot, cancel,
            [&](auto data) {
                file.write(data); for (auto& digest : digests) digest.update(data);
                bytes += data.size(); progress.bytes(data.size(), data.size());
            }, {}, [&](const ResponseHead& head) {
                if (offset) {
                    if (head.status != 206) throw RangeUnavailable("resume entity unavailable");
                    const auto range = parse_range(head.header("content-range"));
                    if (range.start != offset || range.end + 1 != range.total || head.header("etag") != saved_etag ||
                        (request.expected_size && range.total != *request.expected_size)) throw RangeUnavailable("resume metadata changed");
                    actual_total = range.total;
                } else {
                    if (head.status != 200) throw std::runtime_error("artifact HTTP status is not 200");
                    actual_total = head.content_length.value_or(0);
                    if (request.expected_size && head.content_length && *head.content_length != *request.expected_size)
                        throw std::runtime_error("artifact response length mismatch");
                    stage.journal["url"] = candidate.url;
                    const auto etag = head.header("etag");
                    stage.journal["etag"] = etag.starts_with("W/") ? "" : etag;
                    stage.journal["total"] = head.content_length ? Json(*head.content_length) : Json(nullptr);
                    stage.save();
                }
            });
        (void)response;
    } catch (...) {
        file.flush(); stage.journal["size"] = file.size(); stage.save(); throw;
    }
    file.flush(); stage.journal["size"] = file.size(); stage.save();
    if ((actual_total && file.size() != actual_total) || (request.expected_size && file.size() != *request.expected_size))
        throw std::runtime_error("artifact response size incomplete");
    file.close();
    hashes.clear(); const auto types = algorithms(request.digests);
    for (std::size_t at = 0; at < types.size(); ++at) hashes.push_back({types[at], digests[at].finish()});
}

// A one-byte probe obtains a strong ETag before any parallel work; every chunk is
// validated against the same total/ETag, and any failure cancels the whole group.
bool ranged_download(HttpClient& client, const NetworkSnapshot& snapshot, const RequestCandidate& candidate,
                      const DownloadRequest& request, Stage& stage, const Cancellation& cancel,
                      DownloadProgress& progress) {
    if (!request.expected_size || *request.expected_size < request.range_threshold || request.parallel_ranges < 2) return false;
    const auto total = *request.expected_size;
    HttpRequest probe; detail::EraseGuard erased_probe(probe);
    probe.url = candidate.url; detail::copy_sensitive(probe.headers, candidate.headers);
    probe.trust = request.trust; probe.max_response_bytes = 1; probe.headers.push_back({"Range", "bytes=0-0"});
    const auto response = client.stream(probe, snapshot, cancel, {}, {}, [&](const ResponseHead& head) {
        verify_range(head, {0, 0, total}, {});
    });
    const auto etag = response.header("etag");
    if (etag.empty() || etag.starts_with("W/")) return false;
    progress.bytes(response.transferred_bytes, 0);
    auto output = stage.open(); output.truncate(total);
    const auto workers = std::min<std::uint32_t>(request.parallel_ranges, 8);
    const auto chunk_size = std::max<std::uint64_t>(4ULL * 1024 * 1024, (total + workers - 1) / workers);
    Cancellation group;
    std::stop_callback propagate(cancel.stop_token(), [&] { group.cancel(); });
    std::mutex write_gate, failure_gate;
    std::exception_ptr failure;
    std::vector<std::future<void>> futures;
    for (std::uint64_t start = 0; start < total; start += chunk_size) {
        const auto end = std::min(total - 1, start + chunk_size - 1);
        futures.push_back(std::async(std::launch::async, [&, start, end] {
            try {
                HttpRequest request_http; detail::EraseGuard erased_http(request_http);
                request_http.url = candidate.url;
                detail::copy_sensitive(request_http.headers, candidate.headers); request_http.trust = request.trust;
                request_http.headers.push_back({"Range", "bytes=" + std::to_string(start) + "-" + std::to_string(end)});
                request_http.headers.push_back({"If-Range", etag}); request_http.max_response_bytes = end - start + 1;
                std::uint64_t offset = start;
                client.stream(request_http, snapshot, group,
                    [&](auto data) {
                        std::lock_guard lock(write_gate); group.check();
                        output.seek(offset); output.write(data); offset += data.size();
                        progress.bytes(data.size(), data.size());
                    }, {}, [&](const ResponseHead& head) { verify_range(head, {start, end, total}, etag); });
                if (offset != end + 1) throw RangeUnavailable("range body incomplete");
            } catch (...) {
                { std::lock_guard lock(failure_gate); if (!failure) failure = std::current_exception(); }
                group.cancel();
            }
        }));
    }
    for (auto& future : futures) future.get();
    output.flush(); output.close();
    if (failure) { stage.reset(); cancel.check(); std::rethrow_exception(failure); }
    stage.journal["size"] = total; stage.journal["url"] = candidate.url; stage.journal["etag"] = etag; stage.save();
    return true;
}
} // namespace

DownloadReceipt HttpClient::download(const DownloadRequest& request, const Cancellation& cancel, Progress callback) {
    detail::Erased<NetworkSnapshot> captured(this->snapshot());
    return download(request, captured.get(), cancel, std::move(callback));
}
DownloadReceipt HttpClient::download(const DownloadRequest& request, const NetworkSnapshot& input,
                                     const Cancellation& cancel, Progress callback) {
    return detail::boundary("download_failed", "Artifact download failed", [&]() -> DownloadReceipt {
        detail::Erased<NetworkSnapshot> captured(input);
        const auto& snapshot = captured.get();
        const auto cancelled = detail::cancellation_check(cancel);
        detail::check_cancel(cancelled);
        if (request.candidates.empty() || request.candidates.size() > 16 || !request.max_bytes ||
            request.max_bytes > 64ULL * 1024 * 1024 * 1024 || request.parallel_ranges > 8 ||
            !request.retry.max_attempts || request.retry.max_attempts > 8 ||
            request.retry.initial_delay.count() < 0 || request.retry.max_delay.count() < 0 ||
            (request.expected_size && *request.expected_size > request.max_bytes)) throw std::runtime_error("invalid artifact request bounds");
        detail::validate_digests(request.digests);
        if (request.require_digest && request.digests.empty()) throw std::runtime_error("artifact requires a checksum");
        const auto destination = sd::checked_absolute(request.destination);
        sd::ParentGuard parent(destination.parent_path(), false, sd::GuardMode::NamespaceMutation);
        DownloadProgress progress; progress.callback = std::move(callback); progress.total = request.expected_size;
        const auto hash_types = algorithms(request.digests);
        if (fs::exists(sd::io_path(destination))) {
            if (!request.reuse_verified_existing || request.digests.empty()) throw std::runtime_error("artifact destination already exists");
            const auto hashes = detail::hash_file(destination, hash_types, cancelled);
            verify(request, destination, cancel, hashes);
            auto file = sd::SecureFile::open_read(destination);
            progress.completed = file.size(); progress.bytes(0, 0);
            return {destination, file.size(), 0, hashes, true, false, false, "verified-cache"};
        }
        Stage stage(request);
        std::vector<const RequestCandidate*> candidates;
        candidates.reserve(request.candidates.size()); for (const auto& candidate : request.candidates) candidates.push_back(&candidate);
        const bool mirrors = snapshot.file_download_source == SourcePolicy::MirrorFirst;
        std::stable_sort(candidates.begin(), candidates.end(), [mirrors](const auto* a, const auto* b) { return (a->mirror == mirrors) > (b->mirror == mirrors); });
        std::exception_ptr last;
        bool ranges_allowed = request.parallel_ranges > 1;
        for (std::uint32_t attempt = 0; attempt < request.retry.max_attempts; ++attempt) {
            if (attempt) detail::interruptible_wait(std::min(request.retry.max_delay, request.retry.initial_delay * (1U << (attempt - 1))), cancelled);
            for (const auto* selected : candidates) {
                const auto& candidate = *selected;
                detail::check_cancel(cancelled);
                // Mirrors never receive platform credentials; reject the candidate before
                // building a request instead of relying on header sanitisation later.
                if (candidate.mirror && std::any_of(candidate.headers.begin(), candidate.headers.end(), [](const auto& header) {
                    auto name = header.name; for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    return name == "authorization" || name == "proxy-authorization" || name == "cookie" || name == "x-api-key" || name == "api-key" || name == "x-auth-token";
                })) throw std::runtime_error("mirror cannot carry platform credentials");
                bool used_ranges{}, resumed{};
                std::vector<Digest> hashes;
                try {
                    progress.completed = 0;
                    if (ranges_allowed) {
                        try { used_ranges = ranged_download(*this, snapshot, candidate, request, stage, cancel, progress); }
                        catch (const RangeUnavailable&) { stage.reset(); ranges_allowed = false; }
                    }
                    if (!used_ranges) {
                        try { single_download(*this, snapshot, candidate, request, stage, cancel, progress, resumed, hashes); }
                        catch (const RangeUnavailable&) {
                            stage.reset(); resumed = false; progress.completed = 0;
                            single_download(*this, snapshot, candidate, request, stage, cancel, progress, resumed, hashes);
                        }
                    } else hashes = detail::hash_file(stage.file_path, hash_types, cancelled);
                    verify(request, stage.file_path, cancel, hashes);
                    auto file = stage.open(); const auto size = file.size(); file.flush(); file.close();
                    auto gate = detail::cancellation_gate(cancel);
                    std::lock_guard commit_lock(*gate);
                    detail::check_cancel(cancelled);
                    stage.journal["state"] = "committing"; stage.save();
                    sd::rename_owned_no_replace(stage.file_path, destination, stage.file_identity);
                    stage.journal["state"] = "committed"; stage.save();
                    stage.owns = false;
                    sd::remove_owned_tree(stage.workspace, stage.workspace_identity);
                    return {destination, size, progress.transferred.load(), std::move(hashes), false, used_ranges, resumed, safe_origin(candidate.url)};
                } catch (...) {
                    last = std::current_exception();
                    if (cancelled()) { stage.preserve = request.resume && !request.digests.empty(); std::rethrow_exception(last); }
                    if (fs::exists(sd::io_path(destination))) std::rethrow_exception(last);
                    try {
                        auto file = stage.open();
                        const bool complete = file.size() == request.expected_size.value_or(std::numeric_limits<std::uint64_t>::max());
                        file.close();
                        if (complete) stage.reset();
                    } catch (...) { stage.preserve = true; std::rethrow_exception(last); }
                }
            }
        }
        detail::check_cancel(cancelled);
        if (last) std::rethrow_exception(last);
        throw std::runtime_error("artifact source retries exhausted");
    });
}
} // namespace nickel::transport
