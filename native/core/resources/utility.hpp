#pragma once

#include "types.hpp"

#include <span>

namespace nickel::resources::detail {

Path checkedDirectory(const Path& path, bool create = false);
Path checkedRegularFile(const Path& path);
Path checkedLeaf(const Path& root, std::string_view name, bool requireMod = true);
void checkedMissingLeaf(const Path& root, std::string_view name);
std::string fileRevision(const Path& path);
std::string temporaryName(std::string_view operation);

// Opens a non-linked regular file and keeps the same native handle for both passes.
// On Windows, write/delete sharing is denied while this reader is alive.
class FileReader final {
public:
    explicit FileReader(const Path& path, std::uint64_t maximumBytes);
    ~FileReader();
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    std::size_t read(std::span<std::uint8_t> bytes, const Cancellation& cancel);
    void rewind();
    std::uint64_t size() const;
    void verifyUnchanged() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};

void renameExclusive(const Path& root, std::string_view source, std::string_view target);
void removeRegular(const Path& root, std::string_view name);
void copyExclusive(const Path& source, const Path& root, std::string_view target,
                   std::uint64_t maximumBytes, const Cancellation& cancel);
Json parseBoundedJson(std::span<const std::uint8_t> bytes, std::size_t maximumBytes);

} // namespace nickel::resources::detail
