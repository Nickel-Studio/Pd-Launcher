#include "core/archive/archive.hpp"
#include "core/archive/zip_index.hpp"
#include "core/common/json.hpp"
#include "core/tasks/secure_fs.hpp"
#include "core/network/control.hpp"
#include "core/network/errors.hpp"
#include "core/tasks/tasks.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <archive.h>
#include <archive_entry.h>

namespace nickel::archive {
namespace {
namespace fs = std::filesystem;
namespace sd = tasks::detail;
using Clock = std::chrono::steady_clock;
using CancelCheck = transport::detail::CancelCheck;
struct Policy {
    Limits limits;
    CancelCheck cancelled;
    Clock::time_point started{Clock::now()};
    std::uint64_t archive_bytes{};
    std::uint64_t entries{};
    std::uint64_t expanded{};
    void check() const {
        transport::detail::check_cancel(cancelled);
        if (Clock::now() - started > limits.max_duration) throw std::runtime_error("archive operation deadline exceeded");
    }
    void count(std::uint64_t size) {
        if (++entries > limits.max_entries || size > limits.max_entry_bytes ||
            size > limits.max_total_bytes - expanded) throw std::runtime_error("archive quota exceeded");
        if (limits.max_expansion_ratio && limits.max_total_bytes > 1024 * 1024 &&
            size > 1024 * 1024 && size / std::max<std::uint64_t>(archive_bytes, 1) > limits.max_expansion_ratio)
            throw std::runtime_error("archive expansion ratio exceeded");
    }
    void consume(std::uint64_t bytes) {
        if (bytes > limits.max_total_bytes - expanded) throw std::runtime_error("archive total quota exceeded");
        expanded += bytes;
        if (limits.max_expansion_ratio && expanded > 1024 * 1024 &&
            expanded / std::max<std::uint64_t>(archive_bytes, 1) > limits.max_expansion_ratio)
            throw std::runtime_error("archive expansion ratio exceeded");
    }
};
void validate_limits(const Limits& limits) {
    if (!limits.max_entries || limits.max_entries > 1000000 || !limits.max_entry_bytes ||
        !limits.max_total_bytes || !limits.max_archive_bytes || !limits.max_depth ||
        limits.max_depth > 256 || !limits.max_path_bytes || limits.max_path_bytes > 32768 ||
        limits.max_duration.count() <= 0 || limits.max_duration > std::chrono::hours(24))
        throw std::runtime_error("invalid archive limits");
}
struct Component { std::string path; bool directory{}; };
using Components = std::unordered_map<std::string, Component>;
void register_path(Components& map, std::string_view name, bool directory) {
    std::size_t at{};
    for (;;) {
        auto end = name.find('/', at);
        const bool last = end == std::string_view::npos;
        if (last) end = name.size();
        const auto full = std::string(name.substr(0, end));
        const auto key = sd::case_key(full);
        const bool dir = !last || directory;
        if (auto old = map.find(key); old != map.end()) {
            if (old->second.path != full || old->second.directory != dir)
                throw std::runtime_error("archive path case or file/directory conflict");
        } else map.emplace(key, Component{full, dir});
        if (last) break;
        at = end + 1;
    }
}
struct Reader {
    transport::SharedBudget::Reservation memory;
    sd::SecureFile file;
    struct archive* archive{};
    Policy* policy;
    sd::RelativePathPolicy name_policy;
    std::array<std::byte, 64 * 1024> io_buffer, data_buffer;
    std::exception_ptr failure;
    std::vector<detail::ZipEntry> zip_entries;
    bool zip{};
    std::size_t zip_index{};
    std::uint64_t position{};

    static la_ssize_t read_callback(struct archive*, void* context, const void** data) noexcept {
        auto& reader = *static_cast<Reader*>(context);
        try {
            reader.policy->check();
            const auto count = reader.file.read(reader.io_buffer);
            reader.position += count;
            *data = reader.io_buffer.data(); return static_cast<la_ssize_t>(count);
        } catch (...) { reader.failure = std::current_exception(); return -1; }
    }
    static la_int64_t seek_callback(struct archive*, void* context, la_int64_t offset, int origin) noexcept {
        auto& reader = *static_cast<Reader*>(context);
        try {
            reader.policy->check();
            const auto base = origin == SEEK_SET ? 0ULL : origin == SEEK_CUR ? reader.position :
                origin == SEEK_END ? reader.file.size() : std::numeric_limits<std::uint64_t>::max();
            if (base == std::numeric_limits<std::uint64_t>::max() || offset == std::numeric_limits<la_int64_t>::min())
                throw std::runtime_error("invalid archive seek origin or offset");
            std::uint64_t target{};
            if (offset < 0) {
                if (static_cast<std::uint64_t>(-offset) > base) throw std::runtime_error("archive seek precedes input");
                target = base - static_cast<std::uint64_t>(-offset);
            } else {
                if (static_cast<std::uint64_t>(offset) > reader.file.size() - base) throw std::runtime_error("archive seek exceeds input");
                target = base + static_cast<std::uint64_t>(offset);
            }
            reader.file.seek(target); reader.position = target;
            return static_cast<la_int64_t>(target);
        } catch (...) { reader.failure = std::current_exception(); return ARCHIVE_FATAL; }
    }
    Reader(const fs::path& source, Policy& state, bool buffer_reserved = false,
           sd::RelativePathPolicy names = sd::RelativePathPolicy::DiskOutput,
           sd::GuardMode mode = sd::GuardMode::ReadOnly)
        : memory(buffer_reserved ? transport::SharedBudget::Reservation{} :
                 transport::SharedBudget::process()->reserve_buffer(1024 * 1024, state.cancelled)),
          file(sd::SecureFile::open_read(source, mode)), policy(&state), name_policy(names) {
        policy->archive_bytes = file.size();
        if (!file.size() || file.size() > policy->limits.max_archive_bytes) throw std::runtime_error("archive size exceeds limit");
        std::array<std::byte, 4> signature{};
        const auto count = file.read(signature); file.seek(0);
        zip = count == 4 && (std::memcmp(signature.data(), "PK\x03\x04", 4) == 0 ||
                             std::memcmp(signature.data(), "PK\x05\x06", 4) == 0);
        if (zip) zip_entries = detail::inspect_zip(file, policy->limits, policy->cancelled, name_policy);
        file.seek(0);
        archive = archive_read_new();
        if (!archive) throw std::runtime_error("archive handle allocation failed");
        try {
            if (archive_read_support_filter_none(archive) != ARCHIVE_OK ||
                archive_read_support_filter_gzip(archive) != ARCHIVE_OK ||
                archive_read_support_filter_bzip2(archive) != ARCHIVE_OK ||
                archive_read_support_filter_xz(archive) != ARCHIVE_OK ||
                archive_read_support_filter_zstd(archive) != ARCHIVE_OK ||
                archive_read_support_format_zip(archive) != ARCHIVE_OK ||
                archive_read_support_format_tar(archive) != ARCHIVE_OK ||
                archive_read_set_seek_callback(archive, &seek_callback) != ARCHIVE_OK)
                fail("required in-process archive formats or filters are unavailable");
            if (archive_read_open(archive, this, nullptr, &read_callback, nullptr) != ARCHIVE_OK)
                fail("archive open failed");
        } catch (...) { archive_read_free(archive); archive = nullptr; throw; }
    }
    ~Reader() { if (archive) archive_read_free(archive); }
    [[noreturn]] void fail(const char* operation) const {
        if (failure) std::rethrow_exception(failure);
        // Do not expose attacker-controlled archive strings or source paths through errors.
        throw std::runtime_error(operation);
    }
    std::optional<Entry> next() {
        policy->check();
        struct archive_entry* entry{};
        const auto result = archive_read_next_header(archive, &entry);
        if (failure) std::rethrow_exception(failure);
        if (result == ARCHIVE_EOF) {
            if (zip && zip_index != zip_entries.size()) throw std::runtime_error("ZIP indexed entry count mismatch");
            return {};
        }
        if (result != ARCHIVE_OK) fail("invalid archive header");
        const auto format = archive_format(archive) & ARCHIVE_FORMAT_BASE_MASK;
        if (format != ARCHIVE_FORMAT_ZIP && format != ARCHIVE_FORMAT_TAR) throw std::runtime_error("unsupported archive format");
        if ((format == ARCHIVE_FORMAT_ZIP) != zip) throw std::runtime_error("archive format signature mismatch");
        if (archive_entry_is_encrypted(entry) || archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            archive_entry_sparse_count(entry) > 0) throw std::runtime_error("archive links, encryption and sparse entries rejected");
        const auto type = archive_entry_filetype(entry);
        if (type != AE_IFREG && type != AE_IFDIR) throw std::runtime_error("archive special file rejected");
        const auto* name = archive_entry_pathname_utf8(entry);
        if (!name) name = archive_entry_pathname(entry);
        if (!name) throw std::runtime_error("archive path missing");
        Entry out;
        out.name = sd::validate_relative(name, policy->limits.max_depth, policy->limits.max_path_bytes, name_policy);
        out.directory = type == AE_IFDIR;
        if (!archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0) throw std::runtime_error("archive entry size missing");
        out.size = static_cast<std::uint64_t>(archive_entry_size(entry));
        if (out.directory && out.size) throw std::runtime_error("archive directory carries data");
        if (zip) {
            if (zip_index >= zip_entries.size()) throw std::runtime_error("ZIP unindexed entry rejected");
            const auto& expected = zip_entries[zip_index++];
            if (expected.name != out.name || expected.size != out.size || expected.directory != out.directory)
                throw std::runtime_error("ZIP streaming and central metadata disagree");
        }
        policy->count(out.size); return out;
    }
    std::uint64_t copy(const Entry& entry, const std::function<void(std::span<const std::byte>)>& sink) {
        std::uint64_t total{};
        std::uint32_t crc = 0xffffffffU;
        for (;;) {
            policy->check();
            const auto count = archive_read_data(archive, data_buffer.data(), data_buffer.size());
            if (failure) std::rethrow_exception(failure);
            if (count < 0) fail("archive data or CRC invalid");
            if (!count) break;
            const auto size = static_cast<std::size_t>(count);
            if (size > entry.size - total || size > policy->limits.max_entry_bytes - total)
                throw std::runtime_error("archive entry expansion exceeds declared size");
            policy->consume(size); total += size;
            const auto bytes = std::span(data_buffer).first(size);
            if (zip) crc = detail::crc32_update(crc, bytes);
            if (sink) sink(bytes);
        }
        if (total != entry.size) throw std::runtime_error("archive entry size mismatch");
        if (zip && (crc ^ 0xffffffffU) != zip_entries[zip_index - 1].crc)
            throw std::runtime_error("ZIP CRC mismatch");
        return total;
    }
};
std::vector<Entry> validate_reader(Reader& reader, const std::function<void(const Entry&, Reader&)>& action = {}) {
    std::vector<Entry> entries;
    std::unordered_set<std::string> names;
    Components components;
    while (auto entry = reader.next()) {
        if (!names.insert(sd::case_key(entry->name)).second) throw std::runtime_error("duplicate archive entry");
        register_path(components, entry->name, entry->directory);
        if (action) action(*entry, reader);
        else reader.copy(*entry, {});
        entries.push_back(std::move(*entry));
    }
    return entries;
}
std::vector<std::byte> read_entry_impl(const fs::path& source, std::string_view name, std::uint64_t max_bytes,
                                      const Cancellation& cancel, const Limits& limits, sd::RelativePathPolicy names) {
    validate_limits(limits);
    if (max_bytes > 64ULL * 1024 * 1024) throw std::runtime_error("archive memory read exceeds 64 MiB");
    const auto requested = sd::validate_relative(name, limits.max_depth, limits.max_path_bytes, names);
    if (names == sd::RelativePathPolicy::ArchiveMember && requested != name)
        throw std::runtime_error("archive member name must be exact and canonical");
    Policy policy{limits, transport::detail::cancellation_check(cancel)};
    Reader reader(source, policy, false, names);
    transport::SharedBudget::Reservation payload_memory;
    std::vector<std::byte> result;
    bool found{};
    validate_reader(reader, [&](const Entry& entry, Reader& input) {
        if (entry.name == requested && !entry.directory) {
            if (entry.size > max_bytes) throw std::runtime_error("archive entry exceeds memory read limit");
            payload_memory = transport::SharedBudget::process()->reserve_buffer(entry.size, policy.cancelled);
            result.reserve(static_cast<std::size_t>(entry.size)); found = true;
            input.copy(entry, [&](auto bytes) {
                if (bytes.size() > entry.size - result.size() || bytes.size() > max_bytes - result.size())
                    throw std::runtime_error("archive entry exceeds memory read limit");
                result.insert(result.end(), bytes.begin(), bytes.end());
            });
        } else input.copy(entry, {});
    });
    if (!found) throw std::runtime_error("archive requested entry missing");
    return result;
}
bool protected_path(std::string_view path, const std::vector<std::string>& extras) {
    static const std::array<std::string_view, 10> builtins = {
        "nickel.profile.json", "nickel.instance.json", "nickel.pack.json", "versions", "libraries",
        "assets", "runtime", "gamedata.json", "launcher_profiles.json", "nickel.display.json"};
    const auto key = sd::case_key(path);
    const auto matches = [&](std::string_view value) {
        const auto reserved = sd::case_key(value);
        return key == reserved || key.starts_with(reserved + "/");
    };
    for (const auto item : builtins) if (matches(item)) return true;
    for (const auto& item : extras) if (matches(item)) return true;
    return false;
}
struct Selected { Entry entry; std::string relative; };

struct Writer {
    sd::SecureFile file;
    struct archive* archive{};
    std::exception_ptr failure;
    Policy* policy;
    static la_ssize_t write_callback(struct archive*, void* context, const void* data, std::size_t length) noexcept {
        auto& writer = *static_cast<Writer*>(context);
        try {
            writer.policy->check(); writer.file.write({static_cast<const std::byte*>(data), length});
            if (writer.file.size() > writer.policy->limits.max_archive_bytes) throw std::runtime_error("ZIP output quota exceeded");
            return static_cast<la_ssize_t>(length);
        } catch (...) { writer.failure = std::current_exception(); return -1; }
    }
    Writer(const fs::path& output, Policy& state) : file(sd::SecureFile::create_new(output)), policy(&state) {
        archive = archive_write_new();
        if (!archive) throw std::runtime_error("ZIP writer allocation failed");
        if (archive_write_set_format_zip(archive) != ARCHIVE_OK ||
            archive_write_set_format_option(archive, "zip", "compression", "deflate") != ARCHIVE_OK ||
            archive_write_set_bytes_per_block(archive, 0) != ARCHIVE_OK ||
            archive_write_open(archive, this, nullptr, &write_callback, nullptr) != ARCHIVE_OK) {
            archive_write_free(archive); archive = nullptr; throw std::runtime_error("ZIP writer setup failed");
        }
    }
    ~Writer() { if (archive) archive_write_free(archive); }
    void check(int result) {
        if (failure) std::rethrow_exception(failure);
        if (result != ARCHIVE_OK) throw std::runtime_error("ZIP writer operation failed");
    }
};
} // namespace

std::string Archive::validate_relative_path(std::string_view name) {
    return transport::detail::boundary("archive_path", "Archive path is unsafe", [&] { return sd::validate_relative(name); });
}
std::vector<Entry> Archive::list(const fs::path& source, const Limits& limits, const Cancellation& cancel) {
    return transport::detail::boundary("archive_failed", "Archive inspection failed", [&] {
        validate_limits(limits);
        Policy policy{limits, transport::detail::cancellation_check(cancel)};
        Reader reader(source, policy); return validate_reader(reader);
    });
}
std::vector<Entry> Archive::inspect(const fs::path& source, const Limits& limits, const Cancellation& cancel) {
    return list(source, limits, cancel);
}
void Archive::validate(const fs::path& source, const Limits& limits, const Cancellation& cancel) { (void)list(source, limits, cancel); }
std::vector<std::byte> Archive::read_entry(const fs::path& source, std::string_view name,
                                           std::uint64_t max_bytes, const Cancellation& cancel,
                                           const Limits& limits) {
    return transport::detail::boundary("archive_failed", "Archive entry read failed", [&] {
        return read_entry_impl(source, name, max_bytes, cancel, limits, sd::RelativePathPolicy::DiskOutput);
    });
}
std::vector<Entry> Archive::inspect_members(const fs::path& source, const Limits& limits, const Cancellation& cancel) {
    return transport::detail::boundary("archive_failed", "Archive member inspection failed", [&] {
        validate_limits(limits);
        Policy policy{limits, transport::detail::cancellation_check(cancel)};
        Reader reader(source, policy, false, sd::RelativePathPolicy::ArchiveMember);
        return validate_reader(reader);
    });
}
std::vector<std::byte> Archive::read_member_exact(const fs::path& source, std::string_view name,
                                                std::uint64_t max_bytes, const Cancellation& cancel,
                                                const Limits& limits) {
    return transport::detail::boundary("archive_failed", "Archive member read failed", [&] {
        return read_entry_impl(source, name, max_bytes, cancel, limits, sd::RelativePathPolicy::ArchiveMember);
    });
}
ExtractionReceipt Archive::extract(const fs::path& source, const fs::path& destination,
                                    const ExtractOptions& options, const Cancellation& cancel,
                                    ProgressCallback progress) {
    return transport::detail::boundary("archive_failed", "Archive extraction failed", [&] {
        validate_limits(options.limits);
        const auto cancelled = transport::detail::cancellation_check(cancel);
        transport::detail::check_cancel(cancelled);
        const auto target = sd::checked_absolute(destination);
        auto registry = tasks::Registry::process();
        tasks::TaskSpec spec; spec.target = target; spec.kind = tasks::TaskKind::Modpack;
        spec.label = "Archive extraction"; spec.cancellation = cancel;
        auto task = registry->begin(spec);
        Policy policy{options.limits, cancelled};
        Reader reader(source, policy, false, sd::RelativePathPolicy::DiskOutput, sd::GuardMode::NamespaceMutation);
        const auto source_identity = reader.file.identity();
        const auto entries = validate_reader(reader);
        std::vector<Selected> selected;
        Components paths;
        std::unordered_set<std::string> actual_paths, owned_files;
        for (const auto& raw : options.existing_owned_files) {
            const auto name = sd::validate_relative(raw, options.limits.max_depth, options.limits.max_path_bytes);
            if (!owned_files.insert(name).second) throw std::runtime_error("duplicate owned extraction path");
        }
        if (options.subtrees.empty()) {
            for (const auto& entry : entries) selected.push_back({entry, entry.name});
        } else {
            std::unordered_set<std::string> trees;
            for (const auto& raw : options.subtrees) {
                const auto subtree = sd::validate_relative(raw, options.limits.max_depth, options.limits.max_path_bytes);
                if (!trees.insert(sd::case_key(subtree)).second) throw std::runtime_error("duplicate extraction subtree");
                const auto prefix = subtree + "/";
                for (const auto& entry : entries) if (entry.name.starts_with(prefix))
                    selected.push_back({entry, entry.name.substr(prefix.size())});
            }
        }
        for (const auto& item : selected) {
            const auto relative = sd::validate_relative(item.relative, options.limits.max_depth, options.limits.max_path_bytes);
            if (protected_path(relative, options.protected_paths)) throw std::runtime_error("archive targets launcher control paths");
            register_path(paths, relative, item.entry.directory);
            if (!actual_paths.insert(sd::case_key(relative)).second) throw std::runtime_error("extraction subtree outputs overlap");
        }
        const bool existed = fs::exists(sd::io_path(target));
        std::unique_ptr<sd::ParentGuard> target_pin;
        sd::FileIdentity target_identity{};
        std::unordered_map<std::string, tasks::FileLease> leases;
        if (existed) {
            target_identity = sd::directory_identity(target);
            target_pin = std::make_unique<sd::ParentGuard>(target);
            std::uint64_t count{};
            for (const auto& entry : fs::recursive_directory_iterator(sd::io_path(target))) {
                transport::detail::check_cancel(cancelled);
                if (++count > options.limits.max_entries) throw std::runtime_error("extraction target entry budget exceeded");
                const auto path = sd::logical_path(entry.path());
                sd::reject_links(path);
                const auto name = sd::validate_relative(sd::path_utf8(path.lexically_relative(target)));
                register_path(paths, name, entry.is_directory());
                if (!entry.is_regular_file() && !entry.is_directory()) throw std::runtime_error("extraction target special file");
            }
            for (const auto& item : selected) {
                const auto path = target / sd::utf8_path(item.relative);
                if (!item.entry.directory && fs::exists(sd::io_path(path))) {
                    if (!options.allow_owned_overlay || !owned_files.contains(item.relative))
                        throw std::runtime_error("extraction refuses existing unleased target file");
                    leases.emplace(item.relative, tasks::lease_file(path, cancel));
                }
            }
            target_pin.reset();
            target_pin = std::make_unique<sd::ParentGuard>(target, false, sd::GuardMode::NamespaceMutation);
            if (sd::directory_identity(target) != target_identity) throw std::runtime_error("extraction target identity changed");
        }
        std::unordered_map<std::string, const Selected*> by_entry;
        std::vector<tasks::PublishFile> publications;
        std::vector<fs::path> directories;
        ExtractionReceipt receipt;
        for (const auto& item : selected) {
            by_entry.emplace(item.entry.name, &item);
            const auto output = target / sd::utf8_path(item.relative);
            if (item.entry.directory) directories.push_back(output);
            else {
                tasks::PublishFile publication; publication.source_relative = item.relative; publication.destination = output;
                if (auto lease = leases.find(item.relative); lease != leases.end()) publication.previous = lease->second;
                publications.push_back(std::move(publication)); receipt.files.push_back(output);
            }
        }
        const auto started = policy.started;
        const auto& staging = task->staging();
        {
            Policy extract_policy{options.limits, cancelled}; extract_policy.started = started;
            Reader input(source, extract_policy, false, sd::RelativePathPolicy::DiskOutput, sd::GuardMode::NamespaceMutation);
            if (input.file.identity() != source_identity) throw std::runtime_error("archive source changed between passes");
            std::uint64_t files_done{};
            validate_reader(input, [&](const Entry& entry, Reader& data) {
                auto found = by_entry.find(entry.name);
                if (found == by_entry.end()) { data.copy(entry, {}); return; }
                const auto& selected_entry = *found->second;
                if (entry.size != selected_entry.entry.size || entry.directory != selected_entry.entry.directory)
                    throw std::runtime_error("archive changed between inspection and extraction");
                const auto path = staging / sd::utf8_path(selected_entry.relative);
                if (entry.directory) { sd::ParentGuard directory(path, true, sd::GuardMode::NamespaceMutation); data.copy(entry, {}); }
                else {
                    sd::ParentGuard parents(path.parent_path(), true, sd::GuardMode::NamespaceMutation);
                    auto output = sd::SecureFile::create_new(path);
                    data.copy(entry, [&](auto bytes) { output.write(bytes); });
                    output.flush(); output.close(); ++files_done;
                }
                if (progress) progress({entry.name, files_done, publications.size(), extract_policy.expanded});
            });
            receipt.expanded_bytes = extract_policy.expanded;
        }
        transport::detail::check_cancel(cancelled);
        if (existed) {
            target_pin->verify();
            if (sd::directory_identity(target) != target_identity) throw std::runtime_error("extraction target identity changed");
        }
        if (!existed) task->commit_directory();
        else task->commit_files(publications, directories);
        return receipt;
    });
}
void Archive::write_zip(const fs::path& destination, const std::vector<ZipSource>& sources,
                         const Limits& limits, const Cancellation& cancel, ProgressCallback progress) {
    transport::detail::boundary("archive_failed", "ZIP export failed", [&] {
        validate_limits(limits);
        const auto cancelled = transport::detail::cancellation_check(cancel);
        Policy policy{limits, cancelled}; policy.archive_bytes = limits.max_total_bytes;
        if (sources.size() > limits.max_entries) throw std::runtime_error("ZIP source count exceeds quota");
        std::unordered_set<std::string> names;
        Components components;
        std::uint64_t total{};
        struct SourceState { std::string name; sd::FileIdentity identity; std::uint64_t size; };
        std::vector<SourceState> states;
        for (const auto& source : sources) {
            policy.check();
            const auto name = sd::validate_relative(source.name, limits.max_depth, limits.max_path_bytes);
            if (!names.insert(sd::case_key(name)).second) throw std::runtime_error("ZIP source path conflict");
            register_path(components, name, false);
            auto file = sd::SecureFile::open_read(source.source);
            const auto size = file.size();
            if (size > limits.max_entry_bytes || size > limits.max_total_bytes - total)
                throw std::runtime_error("ZIP source quota exceeded");
            states.push_back({name, file.identity(), size}); total += size;
        }
        const auto target = sd::checked_absolute(destination);
        tasks::TaskSpec spec; spec.target = target; spec.kind = tasks::TaskKind::Export;
        spec.label = "ZIP export"; spec.cancellation = cancel;
        auto task = tasks::Registry::process()->begin(spec);
        if (fs::exists(sd::io_path(target))) throw std::runtime_error("ZIP export refuses existing destination");
        const auto staging = task->staging() / "export.zip";
        auto memory = transport::SharedBudget::process()->reserve_buffer(1024 * 1024, cancelled);
        {
            Writer writer(staging, policy);
            std::array<std::byte, 64 * 1024> buffer;
            for (std::size_t at = 0; at < sources.size(); ++at) {
                policy.check();
                auto file = sd::SecureFile::open_read(sources[at].source);
                if (file.identity() != states[at].identity || file.size() != states[at].size)
                    throw std::runtime_error("ZIP source revision changed between passes");
                auto* raw = archive_entry_new();
                if (!raw) throw std::runtime_error("ZIP entry allocation failed");
                std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(raw, &archive_entry_free);
                archive_entry_set_pathname_utf8(entry.get(), states[at].name.c_str());
                archive_entry_set_filetype(entry.get(), AE_IFREG); archive_entry_set_perm(entry.get(), 0644);
                archive_entry_set_size(entry.get(), static_cast<la_int64_t>(states[at].size));
                archive_entry_set_mtime(entry.get(), 0, 0);
                writer.check(archive_write_header(writer.archive, entry.get()));
                std::uint64_t actual{};
                for (;;) {
                    policy.check(); const auto count = file.read(buffer); if (!count) break;
                    if (count > states[at].size - actual) throw std::runtime_error("ZIP source exceeded declared size");
                    actual += count; policy.consume(count);
                    const auto wrote = archive_write_data(writer.archive, buffer.data(), count);
                    if (writer.failure) std::rethrow_exception(writer.failure);
                    if (wrote < 0 || static_cast<std::size_t>(wrote) != count) throw std::runtime_error("ZIP content write failed");
                }
                if (actual != states[at].size) throw std::runtime_error("ZIP source changed while exporting");
                writer.check(archive_write_finish_entry(writer.archive));
                if (progress) progress({states[at].name, at + 1, sources.size(), policy.expanded});
            }
            writer.check(archive_write_close(writer.archive)); writer.file.flush();
        }
        memory = {};
        validate(staging, limits, cancel);
        task->commit_files({{"export.zip", target, {}, {}}});
    });
}
void Archive::validate_executable_jar(const fs::path& source,
                                       const std::vector<std::string>& required_entries,
                                       const Cancellation& cancel, const Limits& limits) {
    transport::detail::boundary("archive_failed", "Executable JAR validation failed", [&] {
        const auto entries = list(source, limits, cancel);
        const auto manifest = read_entry(source, "META-INF/MANIFEST.MF", 1024 * 1024, cancel, limits);
        const std::string text(reinterpret_cast<const char*>(manifest.data()), manifest.size());
        std::vector<std::string> lines;
        for (std::size_t start = 0; start <= text.size();) {
            cancel.check();
            const auto end = text.find('\n', start);
            auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line.starts_with(' ')) {
                if (lines.empty()) throw std::runtime_error("manifest continuation has no header");
                lines.back() += line.substr(1);
            } else lines.push_back(std::move(line));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        std::string main;
        for (const auto& line : lines) {
            const auto colon = line.find(':');
            if (colon == std::string::npos || colon + 1 >= line.size() || line[colon + 1] != ' ')
                throw std::runtime_error("invalid JAR manifest header");
            auto name = line.substr(0, colon);
            for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (name == "main-class") {
                if (!main.empty()) throw std::runtime_error("duplicate JAR Main-Class");
                main = line.substr(colon + 2);
            }
        }
        if (main.empty() || main.front() == '.' || main.back() == '.' || main.find("..") != std::string::npos ||
            std::any_of(main.begin(), main.end(), [](unsigned char c) { return c <= 0x20 || c == '/' || c == '\\' || c == ':'; }))
            throw std::runtime_error("JAR Main-Class missing or invalid");
        std::replace(main.begin(), main.end(), '.', '/'); main += ".class";
        auto required = required_entries; required.push_back(main);
        for (const auto& name : required) {
            cancel.check();
            (void)sd::validate_relative(name);
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.name == name && !entry.directory && entry.size; });
            if (found == entries.end()) throw std::runtime_error("JAR required entry missing or empty");
        }
    });
}
} // namespace nickel::archive
