#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace posix_acceptance {
namespace fs = std::filesystem;
inline unsigned fixture_cleanup_failures{};
inline constexpr std::uint64_t fixture_limit = 1024 * 1024;

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
inline void os_check(bool condition, std::string_view operation) {
    if (!condition) throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const { return value_; }
private:
    int value_;
};

inline struct stat status(const fs::path& path) {
    struct stat value{};
    os_check(::lstat(path.c_str(), &value) == 0, "lstat fixture");
    return value;
}
inline bool exists(const fs::path& path) {
    struct stat value{};
    if (::lstat(path.c_str(), &value) == 0) return true;
    if (errno == ENOENT || errno == ENOTDIR) return false;
    os_check(false, "inspect fixture existence");
    return false;
}
inline bool same_inode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

class Fixture {
public:
    explicit Fixture(std::string_view label) {
        require(std::all_of(label.begin(), label.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        }), "invalid fixture label");
        std::string pattern = "/dev/shm/pd-posix-cleanup-" + std::string(label) + "-" + std::to_string(::getpid()) + "-XXXXXX";
        std::vector<char> text(pattern.begin(), pattern.end()); text.push_back('\0');
        auto* made = ::mkdtemp(text.data());
        os_check(made != nullptr, "allocate exact owned tmpfs fixture");
        root_ = made;
        root_fd_ = Fd(::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        os_check(root_fd_.get() >= 0, "pin fixture root");
        os_check(::fstat(root_fd_.get(), &root_identity_) == 0, "capture fixture identity");
        require(root_identity_.st_uid == ::geteuid() && (root_identity_.st_mode & 077) == 0, "fixture is not private");
        struct statfs filesystem{};
        os_check(::fstatfs(root_fd_.get(), &filesystem) == 0, "fixture filesystem");
        require(static_cast<unsigned long>(filesystem.f_type) == 0x01021994UL, "fixture is not real tmpfs");
        marker_ = "pd-posix-cleanup-acceptance:v1:pid=" + std::to_string(::getpid()) + ":dev=" +
                  std::to_string(root_identity_.st_dev) + ":ino=" + std::to_string(root_identity_.st_ino) + ":label=" + std::string(label) + "\n";
        write(root_ / ".owner", marker_, true);
        std::cout << "FIXTURE_CREATE " << root_.string() << " dev=" << root_identity_.st_dev << " ino=" << root_identity_.st_ino
                  << " uid=" << root_identity_.st_uid << " mode=0700 filesystem=tmpfs limit=" << fixture_limit << '\n';
    }
    ~Fixture() {
        try {
            verify_budget();
            const auto live = posix_acceptance::status(root_);
            require(S_ISDIR(live.st_mode) && same_inode(live, root_identity_), "fixture pathname identity changed");
            require(read(root_ / ".owner") == marker_, "fixture owner marker changed");
            clear_directory(root_fd_.get(), 0);
            const auto before_remove = posix_acceptance::status(root_);
            require(S_ISDIR(before_remove.st_mode) && same_inode(before_remove, root_identity_), "fixture identity changed before release");
            os_check(::rmdir(root_.c_str()) == 0, "release exact fixture root");
            std::cout << "FIXTURE_RELEASE " << root_.string() << " highwater_logical=" << highwater_logical_
                      << " highwater_allocated=" << highwater_allocated_ << " nofollow=true\n";
        } catch (const std::exception& error) {
            ++fixture_cleanup_failures;
            std::cout << "FIXTURE_CLEANUP_FAILED " << root_.string() << " reason=" << error.what() << '\n';
        }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    const fs::path& root() const { return root_; }
    fs::path path(const fs::path& relative) const {
        require(!relative.is_absolute(), "fixture path must be relative");
        check_relative(relative);
        return root_ / relative;
    }
    void directory(const fs::path& input) { (void)parent_fd(relative(input), true, false); }
    void write(const fs::path& input, std::string_view bytes, bool exclusive = false) {
        require(bytes.size() <= fixture_limit - written_bytes_, "fixture cumulative write budget exceeded");
        const auto relative_path = relative(input);
        auto parent = parent_fd(relative_path, true, true);
        const int flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (exclusive ? O_EXCL : O_TRUNC);
        Fd file(::openat(parent.get(), relative_path.filename().c_str(), flags, 0600));
        os_check(file.get() >= 0, "write nofollow fixture leaf");
        struct stat value{}; os_check(::fstat(file.get(), &value) == 0, "fixture write identity");
        require(S_ISREG(value.st_mode) && value.st_uid == ::geteuid(), "fixture write is not owned regular file");
        std::size_t offset{};
        while (offset < bytes.size()) {
            const auto count = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
            os_check(count > 0, "write fixture bytes"); offset += static_cast<std::size_t>(count);
        }
        written_bytes_ += bytes.size();
        verify_budget();
    }
    std::string read(const fs::path& input) const {
        const auto relative_path = relative(input);
        auto parent = parent_fd(relative_path, false, true);
        Fd file(::openat(parent.get(), relative_path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        os_check(file.get() >= 0, "read nofollow fixture leaf");
        std::string result; char bytes[4096];
        for (;;) {
            const auto count = ::read(file.get(), bytes, sizeof(bytes));
            os_check(count >= 0, "read fixture bytes");
            if (!count) break;
            require(static_cast<std::size_t>(count) <= fixture_limit - result.size(), "fixture read exceeds budget");
            result.append(bytes, static_cast<std::size_t>(count));
        }
        return result;
    }
    std::vector<fs::path> quarantines(const fs::path& parent) const {
        std::vector<fs::path> result;
        for (const auto& entry : fs::directory_iterator(parent)) {
            if (entry.path().filename().string().starts_with(".nickel-delete-") && fs::is_directory(entry.symlink_status()))
                result.push_back(entry.path());
        }
        std::sort(result.begin(), result.end());
        return result;
    }
    void verify_budget() const {
        std::uint64_t logical{}, allocated{};
        walk_budget(root_fd_.get(), 0, logical, allocated);
        require(logical <= fixture_limit && allocated <= fixture_limit, "fixture actual 1 MiB limit exceeded");
        highwater_logical_ = std::max(highwater_logical_, logical);
        highwater_allocated_ = std::max(highwater_allocated_, allocated);
    }
private:
    static void check_relative(const fs::path& path) {
        for (const auto& part : path) require(part != ".." && part != "/", "fixture path escape");
    }
    fs::path relative(const fs::path& input) const {
        auto result = input.is_absolute() ? input.lexically_relative(root_) : input;
        require(!result.empty() && !result.is_absolute(), "fixture input outside root");
        check_relative(result); return result;
    }
    Fd parent_fd(const fs::path& relative_path, bool create, bool omit_leaf) const {
        check_relative(relative_path);
        Fd current(::openat(root_fd_.get(), ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        os_check(current.get() >= 0, "reopen fixture root descriptor");
        const auto target = omit_leaf ? relative_path.parent_path() : relative_path;
        for (const auto& part : target) {
            if (part == "." || part.empty()) continue;
            if (create && ::mkdirat(current.get(), part.c_str(), 0700) != 0) os_check(errno == EEXIST, "mkdir fixture component");
            Fd next(::openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            os_check(next.get() >= 0, "pin nofollow fixture parent"); current = std::move(next);
        }
        return current;
    }
    template<class Action>
    static void entries(int directory, Action action) {
        const int copy = ::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        os_check(copy >= 0, "open fixture enumeration");
        auto* stream = ::fdopendir(copy);
        if (!stream) { const auto saved = errno; ::close(copy); errno = saved; os_check(false, "fixture fdopendir"); }
        try {
            for (;;) {
                errno = 0; auto* entry = ::readdir(stream);
                if (!entry) { os_check(errno == 0, "fixture readdir"); break; }
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                struct stat value{};
                os_check(::fstatat(directory, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0, "fixture entry identity");
                action(name, value);
            }
        } catch (...) { ::closedir(stream); throw; }
        ::closedir(stream);
    }
    static void walk_budget(int directory, unsigned depth, std::uint64_t& logical, std::uint64_t& allocated) {
        require(depth < 64, "fixture nesting bound exceeded");
        entries(directory, [&](const std::string& name, const struct stat& value) {
            if (S_ISDIR(value.st_mode)) {
                Fd child(::openat(directory, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                os_check(child.get() >= 0, "budget child descriptor"); walk_budget(child.get(), depth + 1, logical, allocated);
            } else {
                logical += static_cast<std::uint64_t>(value.st_size);
                allocated += static_cast<std::uint64_t>(value.st_blocks) * 512;
            }
        });
    }
    static void clear_directory(int directory, unsigned depth) {
        require(depth < 64, "fixture cleanup nesting bound exceeded");
        entries(directory, [&](const std::string& name, const struct stat& value) {
            if (S_ISDIR(value.st_mode)) {
                Fd child(::openat(directory, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                os_check(child.get() >= 0, "cleanup nofollow child");
                struct stat pinned{}; os_check(::fstat(child.get(), &pinned) == 0, "cleanup child identity");
                require(same_inode(value, pinned), "fixture cleanup child replaced");
                clear_directory(child.get(), depth + 1);
                struct stat current{};
                os_check(::fstatat(directory, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0, "cleanup child recheck");
                require(S_ISDIR(current.st_mode) && same_inode(current, pinned), "fixture cleanup pathname changed");
                os_check(::unlinkat(directory, name.c_str(), AT_REMOVEDIR) == 0, "remove exact fixture directory");
            } else {
                require(S_ISREG(value.st_mode) || S_ISLNK(value.st_mode), "unexpected special fixture object");
                os_check(::unlinkat(directory, name.c_str(), 0) == 0, "unlink owned fixture leaf without follow");
            }
        });
    }
    fs::path root_;
    Fd root_fd_;
    struct stat root_identity_{};
    std::string marker_;
    std::uint64_t written_bytes_{};
    mutable std::uint64_t highwater_logical_{};
    mutable std::uint64_t highwater_allocated_{};
};
} // namespace posix_acceptance
