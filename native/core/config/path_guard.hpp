#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace nickel::foundation_detail {

enum class DirectoryGuardMode { ReadOnly, NamespaceMutation };

// Pins every ancestor without DELETE sharing. The default also denies writers.
// Mutation mode retains a private final-directory anchor and requires handle-
// relative nofollow I/O; allowing directory writers is not a path-safety proof.
class DirectoryGuard {
public:
    explicit DirectoryGuard(const std::filesystem::path& directory, bool create = false,
                            DirectoryGuardMode mode = DirectoryGuardMode::ReadOnly);
    ~DirectoryGuard();
    DirectoryGuard(DirectoryGuard&&) noexcept;
    DirectoryGuard& operator=(DirectoryGuard&&) noexcept;
    DirectoryGuard(const DirectoryGuard&) = delete;
    DirectoryGuard& operator=(const DirectoryGuard&) = delete;
    const std::filesystem::path& path() const noexcept;
    std::intptr_t native_handle() const noexcept;
    DirectoryGuardMode mode() const noexcept;
    void verify() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _WIN32
// Native NtCreateFile values; caller owns the returned HANDLE. Ordinary OS
// failure returns -1 and sets GetLastError(), keeping missing/conflict distinct.
// A single validated leaf resolves only against the retained parent handle.
std::intptr_t windows_open_relative(const DirectoryGuard& parent,
    const std::filesystem::path& leaf, std::uint32_t access, std::uint32_t sharing,
    std::uint32_t disposition, std::uint32_t create_options,
    void* security_descriptor = nullptr);
#endif

std::filesystem::path checked_absolute_path(const std::filesystem::path& path);
// Validates a logical path, then adds the Windows extended-length I/O prefix.
// Keep this representation at native filesystem boundaries, not in saved DTOs.
std::filesystem::path native_filesystem_path(const std::filesystem::path& path);
void reject_file_hardlinks(const std::filesystem::path& path);

} // namespace nickel::foundation_detail
