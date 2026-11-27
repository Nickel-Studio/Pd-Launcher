#include "archive_bridge.hpp"
#include "internal.hpp"
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <miniz.h>

namespace nickel::packs {
namespace {
using namespace detail;
struct AllocationBudget {
    struct alignas(std::max_align_t) Header { std::size_t bytes; };
    std::size_t bytes = 0;
    static constexpr std::size_t limit = 64 * 1024 * 1024;
    static void* allocate(void* opaque, std::size_t count, std::size_t size) noexcept {
        auto& state = *static_cast<AllocationBudget*>(opaque);
        if (count && size > std::numeric_limits<std::size_t>::max() / count) return nullptr;
        const auto wanted = count * size;
        if (wanted > limit - state.bytes) return nullptr;
        auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + wanted));
        if (!header) return nullptr;
        header->bytes = wanted;
        state.bytes += wanted;
        return header + 1;
    }
    static void free(void* opaque, void* address) noexcept {
        if (!address) return;
        auto& state = *static_cast<AllocationBudget*>(opaque);
        auto* header = static_cast<Header*>(address) - 1;
        state.bytes -= header->bytes;
        std::free(header);
    }
    static void* reallocate(void* opaque, void* address, std::size_t count, std::size_t size) noexcept {
        if (!address) return allocate(opaque, count, size);
        auto& state = *static_cast<AllocationBudget*>(opaque);
        if (count && size > std::numeric_limits<std::size_t>::max() / count) return nullptr;
        const auto wanted = count * size;
        auto* old = static_cast<Header*>(address) - 1;
        const auto previous = old->bytes;
        if (wanted > limit - state.bytes + previous) return nullptr;
        auto* next = static_cast<Header*>(std::realloc(old, sizeof(Header) + wanted));
        if (!next) return nullptr;
        next->bytes = wanted;
        state.bytes = state.bytes - previous + wanted;
        return next + 1;
    }
    void configure(mz_zip_archive& zip) {
        zip.m_pAlloc = allocate;
        zip.m_pFree = free;
        zip.m_pRealloc = reallocate;
        zip.m_pAlloc_opaque = this;
    }
};
struct FileCallback {
    NativeFile* file;
    std::stop_token cancel;
    std::exception_ptr failure;
    std::uint64_t maximum;
    std::uint64_t written = 0;
    static std::size_t read(void* opaque, mz_uint64 offset, void* buffer, std::size_t count) noexcept {
        auto& state = *static_cast<FileCallback*>(opaque);
        try {
            io::check_cancelled(state.cancel);
            if (offset > state.maximum) throw Error("archive", "Archive read outside file");
            const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(count, state.maximum - offset));
            return state.file->read_at(offset, buffer, available);
        } catch (...) { state.failure = std::current_exception(); return 0; }
    }
    static std::size_t write(void* opaque, mz_uint64 offset, const void* buffer, std::size_t count) noexcept {
        auto& state = *static_cast<FileCallback*>(opaque);
        try {
            io::check_cancelled(state.cancel);
            if (offset != state.written || offset > state.maximum || count > state.maximum - offset)
                throw Error("limit", "Archive output exceeds quota or is unordered");
            state.file->write_at(offset, buffer, count);
            state.written += count;
            return count;
        } catch (...) { state.failure = std::current_exception(); return 0; }
    }
    void check(bool success) {
        if (failure) std::rethrow_exception(failure);
        io::check_cancelled(cancel);
        if (!success) throw Error("archive", "ZIP integrity or bounded I/O validation failed");
    }
};
struct MemoryCallback {
    std::string* text;
    std::stop_token cancel;
    std::uint64_t maximum;
    std::exception_ptr failure;
    static std::size_t write(void* opaque, mz_uint64 offset, const void* buffer, std::size_t count) noexcept {
        auto& state = *static_cast<MemoryCallback*>(opaque);
        try {
            io::check_cancelled(state.cancel);
            if (offset != state.text->size() || offset > state.maximum || count > state.maximum - offset)
                throw Error("limit", "Archive document exceeds quota or is unordered");
            state.text->append(static_cast<const char*>(buffer), count);
            return count;
        } catch (...) { state.failure = std::current_exception(); return 0; }
    }
    static std::size_t read(void* opaque, mz_uint64 offset, void* buffer, std::size_t count) noexcept {
        auto& state = *static_cast<MemoryCallback*>(opaque);
        try {
            io::check_cancelled(state.cancel);
            if (offset > state.text->size()) throw Error("archive", "Invalid memory archive input offset");
            const auto available = std::min<std::size_t>(count, state.text->size() - static_cast<std::size_t>(offset));
            std::memcpy(buffer, state.text->data() + offset, available);
            return available;
        } catch (...) { state.failure = std::current_exception(); return 0; }
    }
};
struct ZipReader {
    fs::path path;
    AllocationBudget budget;
    NativeFile file;
    FileCallback callback;
    mz_zip_archive zip{};
    std::vector<ArchiveEntry> entries;
    std::unordered_map<std::string, mz_uint> indexes;
    ZipReader(const fs::path& source, const ArchiveLimits& limits, std::stop_token cancel)
        : path(source), file(source, NativeFile::Mode::read), callback{&file, cancel, {}, file.size()} {
        if (file.size() > max_pack_bytes) throw Error("limit", "Archive exceeds input quota");
        budget.configure(zip);
        zip.m_pRead = FileCallback::read;
        zip.m_pIO_opaque = &callback;
        try {
            callback.check(mz_zip_reader_init(&zip, file.size(), MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY));
            const auto count = mz_zip_reader_get_num_files(&zip);
            if (count > limits.entries) throw Error("limit", "Archive entry count exceeds quota");
            entries.reserve(count);
            for (mz_uint i = 0; i < count; ++i) {
                io::check_cancelled(cancel);
                mz_zip_archive_file_stat stat{};
                callback.check(mz_zip_reader_file_stat(&zip, i, &stat));
                if (stat.m_is_encrypted || !stat.m_is_supported) throw Error("archive", "Encrypted or unsupported ZIP entry");
                const auto name_size = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
                if (!name_size || name_size > 1026) throw Error("unsafe_path", "ZIP name exceeds bounded path");
                std::string name(name_size, '\0');
                if (mz_zip_reader_get_filename(&zip, i, name.data(), name_size) != name_size)
                    throw Error("archive", "Invalid ZIP entry name");
                name.pop_back();
                const auto type = (stat.m_external_attr >> 16) & 0170000;
                const bool link = (type && type != 0100000 && type != 0040000)
                    || (stat.m_external_attr & 0x400U)
                    || (type == 0040000 && !stat.m_is_directory)
                    || (type == 0100000 && stat.m_is_directory);
                entries.push_back({name, stat.m_uncomp_size, stat.m_comp_size, stat.m_is_directory != 0, link});
                indexes.emplace(name, i);
            }
            validate_archive_entries(entries, limits);
            file.unchanged();
        } catch (...) { if (zip.m_pState) mz_zip_reader_end(&zip); throw; }
    }
    ~ZipReader() { if (zip.m_pState) mz_zip_reader_end(&zip); }
    void begin(std::stop_token cancel) {
        file.unchanged();
        callback.cancel = cancel;
        callback.failure = {};
        io::check_cancelled(cancel);
    }
    mz_uint index(std::string_view name) const {
        const auto found = indexes.find(std::string(name));
        if (found == indexes.end() || entries[found->second].directory) throw Error("archive", "Required ZIP file is missing");
        return found->second;
    }
};
}
struct ZipArchiveBackend::State {
    std::mutex mutex;
    std::unique_ptr<ZipReader> reader;
    ZipReader& open(const fs::path& file, const ArchiveLimits& limits, std::stop_token cancel) {
        if (!reader || reader->path != file) reader = std::make_unique<ZipReader>(file, limits, cancel);
        reader->begin(cancel);
        validate_archive_entries(reader->entries, limits);
        return *reader;
    }
};
ZipArchiveBackend::ZipArchiveBackend() : state_(std::make_unique<State>()) {}
ZipArchiveBackend::~ZipArchiveBackend() = default;
std::shared_ptr<ArchiveBackend> make_zip_backend() { return std::make_shared<ZipArchiveBackend>(); }
void ZipArchiveBackend::release(const fs::path& path) noexcept {
    try { std::lock_guard lock(state_->mutex); if (state_->reader && state_->reader->path == path) state_->reader.reset(); }
    catch (...) {}
}
std::vector<ArchiveEntry> ZipArchiveBackend::inspect(const fs::path& file, const ArchiveLimits& limits, std::stop_token cancel) {
    std::lock_guard lock(state_->mutex);
    return state_->open(file, limits, cancel).entries;
}
std::string ZipArchiveBackend::read(const fs::path& file, std::string_view name, std::uint64_t maximum, std::stop_token cancel) {
    std::lock_guard lock(state_->mutex);
    auto& reader = state_->open(file, {}, cancel);
    const auto index = reader.index(name);
    const auto size = reader.entries[index].size;
    // Full reports verify bounded documents; pack JSON limits remain explicit at their call sites.
    if (size > maximum || size > 64ULL * 1024 * 1024) throw Error("limit", "ZIP document exceeds memory quota");
    std::string result;
    result.reserve(static_cast<std::size_t>(size));
    MemoryCallback output{&result, cancel, maximum, {}};
    const auto success = mz_zip_reader_extract_to_callback(&reader.zip, index, MemoryCallback::write, &output, 0);
    if (output.failure) std::rethrow_exception(output.failure);
    reader.callback.check(success);
    if (result.size() != size) throw Error("archive", "ZIP document length mismatch");
    reader.file.unchanged();
    return result;
}
void ZipArchiveBackend::extract_file(const fs::path& file, std::string_view name, const fs::path& target,
                                      std::uint64_t size, std::stop_token cancel) {
    std::lock_guard lock(state_->mutex);
    auto& reader = state_->open(file, {}, cancel);
    const auto index = reader.index(name);
    if (reader.entries[index].size != size) throw Error("source_changed", "ZIP entry size changed");
    reject_links(target);
    io::create_directories(target.parent_path());
    const auto temporary = target.parent_path() / (".nickel-unpack-" + io::random_id() + ".part");
    bool created = false;
    try {
        NativeFile output(temporary, NativeFile::Mode::exclusive);
        created = true;
        FileCallback callback{&output, cancel, {}, size};
        const auto success = mz_zip_reader_extract_to_callback(&reader.zip, index, FileCallback::write, &callback, 0);
        callback.check(success);
        reader.callback.check(success);
        if (callback.written != size) throw Error("archive", "ZIP extraction length mismatch");
        reader.file.unchanged();
        output.flush();
        output.close();
        io::check_cancelled(cancel);
        io::rename_no_replace(temporary, target);
    } catch (...) {
        if (created) { std::error_code ec; fs::remove(io::native_path(temporary), ec); }
        throw;
    }
}
void ZipArchiveBackend::write(const fs::path& target, std::span<const ArchiveInput> inputs,
                               const ArchiveLimits& limits, std::stop_token cancel) {
    io::check_cancelled(cancel);
    std::vector<ArchiveEntry> entries;
    entries.reserve(inputs.size());
    for (const auto& input : inputs) {
        if (input.file && !input.bytes.empty()) throw Error("invalid_schema", "Ambiguous ZIP input");
        const auto size = input.file ? NativeFile(*input.file, NativeFile::Mode::read).size() : input.bytes.size();
        entries.push_back({input.name, size, size, false, false});
    }
    validate_archive_entries(entries, limits);
    reject_links(target);
    io::create_directories(target.parent_path());
    bool created = false;
    mz_zip_archive zip{};
    AllocationBudget budget;
    try {
        NativeFile output(target, NativeFile::Mode::exclusive);
        created = true;
        const auto overhead = static_cast<std::uint64_t>(limits.entries) * 2048 + 1024 * 1024;
        if (limits.total_bytes > std::numeric_limits<std::uint64_t>::max() - overhead - limits.total_bytes / 8)
            throw Error("limit", "Invalid archive quota");
        FileCallback writer{&output, cancel, {}, limits.total_bytes + limits.total_bytes / 8 + overhead};
        budget.configure(zip);
        zip.m_pWrite = FileCallback::write;
        zip.m_pIO_opaque = &writer;
        writer.check(mz_zip_writer_init_v2(&zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64));
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            io::check_cancelled(cancel);
            const auto& input = inputs[i];
            bool success = false;
            if (input.file) {
                NativeFile source(*input.file, NativeFile::Mode::read);
                if (source.size() != entries[i].size) throw Error("source_changed", "ZIP input size changed");
                FileCallback reader{&source, cancel, {}, source.size()};
                success = mz_zip_writer_add_read_buf_callback(&zip, input.name.c_str(), FileCallback::read, &reader,
                    source.size(), nullptr, nullptr, 0, MZ_BEST_SPEED, nullptr, 0, nullptr, 0);
                reader.check(success);
                source.unchanged();
            } else {
                auto text = input.bytes;
                MemoryCallback reader{&text, cancel, text.size(), {}};
                success = mz_zip_writer_add_read_buf_callback(&zip, input.name.c_str(), MemoryCallback::read, &reader,
                    text.size(), nullptr, nullptr, 0, MZ_BEST_SPEED, nullptr, 0, nullptr, 0);
                if (reader.failure) std::rethrow_exception(reader.failure);
            }
            writer.check(success);
        }
        writer.check(mz_zip_writer_finalize_archive(&zip));
        const auto ended = mz_zip_writer_end(&zip);
        writer.check(ended);
        output.flush();
        io::check_cancelled(cancel);
    } catch (...) {
        if (zip.m_pState) mz_zip_writer_end(&zip);
        if (created) { std::error_code ec; fs::remove(io::native_path(target), ec); }
        throw;
    }
}
} // namespace nickel::packs
