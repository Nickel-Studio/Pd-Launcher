#include "native_services.hpp"
#include "native_service_detail.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

namespace nickel::packs {
namespace {
archive::Limits archive_limits(const ArchiveLimits& input) {
    if (!input.entries || !input.file_bytes || !input.total_bytes)
        throw instances::Error("limit", "Native archive requires nonzero bounded quotas");
    archive::Limits result;
    result.max_entries = input.entries;
    result.max_entry_bytes = input.file_bytes;
    result.max_total_bytes = input.total_bytes;
    result.max_archive_bytes = input.total_bytes;
    result.max_path_bytes = 1024;
    return result;
}

class NativeArchive final : public ArchiveBackend {
    struct Tree {
        std::unique_ptr<detail::Workspace> workspace;
        fs::path directory;
    };
    struct Source {
        explicit Source(fs::path input) : path(std::move(input)), pin(path, detail::NativeFile::Mode::read) {}
        fs::path path;
        detail::NativeFile pin;
        ArchiveLimits limits;
        std::vector<ArchiveEntry> entries;
        std::map<std::string, Tree> trees;
    };
public:
    std::vector<ArchiveEntry> inspect(const fs::path& input, const ArchiveLimits& limits, std::stop_token stop) override {
        std::lock_guard lock(mutex_);
        detail::io::check_cancelled(stop);
        if (!input.is_absolute()) throw instances::Error("unsafe_path", "Native archive source must be absolute");
        auto state = std::make_unique<Source>(input);
        const auto policy = archive_limits(limits);
        if (state->pin.size() > policy.max_archive_bytes) throw instances::Error("limit", "Archive input exceeds file quota");
        detail::NativeCancellation cancellation(stop);
        const auto entries = detail::native_service_call([&] {
            return archive::Archive::inspect(input, policy, cancellation.get());
        }, "archive", "Native archive inspection failed");
        state->limits = limits;
        for (const auto& entry : entries) state->entries.push_back({entry.name, entry.size, 0, entry.directory, false});
        validate_archive_entries(state->entries, limits);
        state->pin.unchanged();
        source_ = std::move(state);
        return source_->entries;
    }

    std::string read(const fs::path& input, std::string_view name, std::uint64_t maximum, std::stop_token stop) override {
        std::lock_guard lock(mutex_);
        detail::io::check_cancelled(stop);
        validate_archive_path(name);
        if (maximum > 64ULL * 1024 * 1024) throw instances::Error("limit", "Native archive memory read exceeds 64 MiB");
        auto& state = source(input);
        const auto found = entry(state, name);
        if (found.size > maximum) throw instances::Error("limit", "Native archive entry exceeds bounded memory read");
        detail::NativeCancellation cancellation(stop);
        const auto bytes = detail::native_service_call([&] {
            return archive::Archive::read_entry(input, name, maximum, cancellation.get(), archive_limits(state.limits));
        }, "archive", "Native archive entry read failed");
        state.pin.unchanged();
        if (bytes.size() != found.size) throw instances::Error("source_changed", "Native archive entry changed after inspection");
        if (bytes.empty()) return {};
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    void extract_file(const fs::path& input, std::string_view name, const fs::path& destination,
                      std::uint64_t size, std::stop_token stop) override {
        std::lock_guard lock(mutex_);
        detail::io::check_cancelled(stop);
        auto& state = source(input);
        const auto found = entry(state, name);
        if (found.size != size || size > state.limits.file_bytes)
            throw instances::Error("integrity", "Native archive extraction size differs from the selected entry");
        const auto slash = name.find('/');
        const auto tree_name = std::string(name.substr(0, slash));
        if (slash == std::string_view::npos || (tree_name != "overrides" && tree_name != "client-overrides"))
            throw instances::Error("protected_path", "Native pack extraction requires an override subtree");
        const auto relative = name.substr(slash + 1);
        validate_pack_path(relative);
        detail::reject_links(destination);
        if (!destination.is_absolute() || detail::io::path_exists(destination))
            throw instances::Error("conflict", "Native archive extraction refuses an existing or relative target");
        if (!state.trees.contains(tree_name)) {
            Tree staged;
            staged.workspace = std::make_unique<detail::Workspace>(input.parent_path());
            staged.directory = staged.workspace->directory() / "files";
            archive::ExtractOptions options;
            options.limits = archive_limits(state.limits);
            options.subtrees = {tree_name};
            detail::NativeCancellation cancellation(stop);
            // The shared streaming extractor stages one override layer once; nothing enters the reserved instance here.
            const auto receipt = detail::native_service_call([&] {
                return archive::Archive::extract(input, staged.directory, options, cancellation.get());
            }, "archive", "Native override staging failed");
            std::set<fs::path> expected;
            const auto prefix = tree_name + '/';
            for (const auto& item : state.entries) if (!item.directory && item.name.starts_with(prefix))
                expected.insert((staged.directory / detail::path_from_utf8(item.name.substr(prefix.size()))).lexically_normal());
            for (const auto& file : receipt.files) {
                if (!expected.erase(file.lexically_normal())) throw instances::Error("archive", "Native staging receipt contains an unexpected file");
                detail::reject_links(file);
            }
            if (!expected.empty()) throw instances::Error("archive", "Native staging receipt is incomplete");
            state.pin.unchanged();
            state.trees.emplace(tree_name, std::move(staged));
        }
        const auto staged_file = state.trees.at(tree_name).directory / detail::path_from_utf8(relative);
        detail::io::create_directories(destination.parent_path());
        detail::Workspace publication(destination.parent_path());
        const auto copied = publication.directory() / "file";
        if (detail::copy_file(staged_file, copied, size, stop) != size)
            throw instances::Error("integrity", "Native staged override size differs from its entry");
        state.pin.unchanged();
        const auto digest = hash_file(copied, "sha256", stop);
        detail::io::copy_no_replace(copied, destination, size, stop, digest);
    }

    void write(const fs::path& target, std::span<const ArchiveInput> inputs,
               const ArchiveLimits& limits, std::stop_token stop) override {
        detail::io::check_cancelled(stop);
        if (!target.is_absolute()) throw instances::Error("unsafe_path", "Native ZIP target must be absolute");
        const auto policy = archive_limits(limits);
        if (inputs.size() > limits.entries) throw instances::Error("limit", "Native ZIP source count exceeds quota");
        detail::reject_links(target);
        if (detail::io::path_exists(target)) throw instances::Error("conflict", "Native ZIP refuses existing output");
        detail::io::create_directories(target.parent_path());
        detail::Workspace workspace(target.parent_path());
        std::vector<archive::ZipSource> sources;
        std::vector<ArchiveEntry> entries;
        std::vector<std::unique_ptr<detail::NativeFile>> pins;
        std::uint64_t inline_bytes = 0;
        for (const auto& input : inputs) {
            detail::io::check_cancelled(stop);
            validate_archive_path(input.name);
            fs::path file;
            std::uint64_t size;
            if (input.file) {
                if (!input.bytes.empty()) throw instances::Error("archive", "Native ZIP input contains two competing sources");
                file = *input.file;
                auto pin = std::make_unique<detail::NativeFile>(file, detail::NativeFile::Mode::read);
                size = pin->size();
                pins.push_back(std::move(pin));
            } else {
                if (input.bytes.size() > 64ULL * 1024 * 1024 - inline_bytes)
                    throw instances::Error("limit", "Native ZIP inline sources exceed 64 MiB");
                inline_bytes += input.bytes.size();
                file = workspace.directory() / detail::path_from_utf8(std::to_string(sources.size()));
                detail::io::write_exclusive(file, input.bytes);
                size = input.bytes.size();
            }
            entries.push_back({input.name, size, 0, false, false});
            sources.push_back({input.name, std::move(file)});
        }
        validate_archive_entries(entries, limits);
        detail::NativeCancellation cancellation(stop);
        const auto staged = workspace.directory() / "archive.zip";
        // Source checks must still be able to fail before the caller's target is committed.
        detail::native_service_call([&] { archive::Archive::write_zip(staged, sources, policy, cancellation.get()); },
                                    "archive", "Native ZIP export failed");
        for (const auto& pin : pins) pin->unchanged();
        detail::io::check_cancelled(stop);
        const auto digest = hash_file(staged, "sha256", stop);
        detail::io::copy_no_replace(staged, target, policy.max_archive_bytes, stop, digest);
        // Final publication is terminal; only noexcept pin/workspace release remains.
        pins.clear();
    }

    void release(const fs::path& input) noexcept override {
        try {
            std::lock_guard lock(mutex_);
            if (source_ && source_->path.lexically_normal() == input.lexically_normal()) source_.reset();
        } catch (...) {}
    }
private:
    Source& source(const fs::path& input) {
        if (!source_ || source_->path.lexically_normal() != input.lexically_normal())
            throw instances::Error("archive", "Native archive must be inspected before entry operations");
        source_->pin.unchanged();
        return *source_;
    }
    static ArchiveEntry entry(const Source& state, std::string_view name) {
        const auto found = std::find_if(state.entries.begin(), state.entries.end(), [&](const auto& item) { return item.name == name && !item.directory; });
        if (found == state.entries.end()) throw instances::Error("archive", "Selected native archive file is missing");
        return *found;
    }
    std::mutex mutex_;
    std::unique_ptr<Source> source_;
};
} // namespace

std::shared_ptr<ArchiveBackend> native_archive_backend() { return std::make_shared<NativeArchive>(); }
} // namespace nickel::packs
