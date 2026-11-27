#include "core/tasks/secure_fs.hpp"
#include "core/tasks/test_hooks.hpp"
#include "core/network/errors.hpp"
#include "core/network/hash.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace sd = nickel::tasks::detail;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action&& action, const char* message) {
    bool caught{};
    try { action(); } catch (const nickel::ErrorException&) { caught = true; }
    require(caught, message);
}
template<class Action> void rejects_code(Action&& action, std::string_view code, const char* message) {
    try { action(); }
    catch (const nickel::ErrorException& error) { require(error.error().code == code, message); return; }
    throw std::runtime_error(message);
}
bool path_exists(const fs::path& path) { return fs::exists(sd::io_path(path)); }
void write_new(const fs::path& path, std::string_view text) {
    auto file = sd::SecureFile::create_new(path);
    file.write(std::as_bytes(std::span(text))); file.flush();
}
std::string read(const fs::path& path) {
    const auto bytes = sd::read_bounded(path, 32 * 1024 * 1024);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
struct Root {
    fs::path path;
    sd::FileIdentity identity;
    explicit Root(const fs::path& parent) {
        { sd::ParentGuard pin(parent, true); }
        path = fs::absolute(parent) / sd::utf8_path("leaf-" + sd::unique_id());
        sd::create_directory_new(path); identity = sd::directory_identity(path);
    }
    ~Root() {
        try { sd::remove_owned_tree(path, identity); }
        catch (const std::exception& error) { std::cerr << "leaf fixture retained: " << error.what() << '\n'; }
    }
};
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
struct Hook {
    explicit Hook(std::function<void(std::string_view, const fs::path&)> action) {
        sd::testing::checkpoint = std::move(action);
    }
    ~Hook() { sd::testing::checkpoint = {}; }
};
#endif
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
void removal_commit_tests(const fs::path& parent) {
    Root root(parent);
    const auto before = root.path / "precommit";
    write_new(before, "precommit bytes");
    const auto before_token = sd::capture_regular_file(root.path, before);
    {
        bool cancelled{};
        Hook hook([&](auto point, const auto&) { if (point == "regular.validated") cancelled = true; });
        try {
            sd::remove_regular_file(before_token, [&] { return cancelled; });
            throw std::runtime_error("precommit cancellation was accepted");
        } catch (const nickel::ErrorException& error) {
            require(error.error().code == "cancelled" && !error.error().details.value("committed", false),
                    "precommit cancellation claimed a removal commit");
        }
    }
    require(read(before) == "precommit bytes", "precommit cancellation changed authorized bytes");
    {
        Hook hook([&](auto point, const auto&) {
            if (point == "regular.validated")
                throw std::system_error(std::make_error_code(std::errc::io_error), "private precommit diagnostic");
        });
        try {
            sd::remove_regular_file(before_token);
            throw std::runtime_error("precommit fault was ignored");
        } catch (const nickel::ErrorException& error) {
            require(error.error().code == "remove_failed" && !error.error().details.value("committed", false),
                    "precommit native failure claimed a removal commit");
            require(error.error().details.at("nativeError") == std::make_error_code(std::errc::io_error).value(),
                    "precommit failure lost its numeric native error");
        }
    }
    require(read(before) == "precommit bytes", "precommit failure changed authorized bytes");
    const auto late = root.path / "late-stop";
    write_new(late, "late stop bytes");
    const auto late_token = sd::capture_regular_file(root.path, late);
    bool late_cancelled{};
    {
        Hook hook([&](auto point, const auto&) { if (point == "regular.committed") late_cancelled = true; });
        sd::remove_regular_file(late_token, [&] { return late_cancelled; });
    }
    require(late_cancelled && !path_exists(late), "late cancellation hid or reversed committed removal");
    for (const auto size : {std::size_t{0}, std::size_t{17}}) {
        for (int fault = 0; fault != 4; ++fault) {
            const auto leaf = root.path / ("committed-" + std::to_string(size) + "-" + std::to_string(fault));
            write_new(leaf, std::string(size, 'x'));
            const auto token = sd::capture_regular_file(root.path, leaf);
            bool reached{}, caught{};
            {
                Hook hook([&](auto point, const auto& path) {
                    if (point != "regular.committed") return;
                    require(path == leaf, "commit hook used a different authorization path");
                    reached = true;
                    if (fault == 0)
                        throw std::system_error(std::make_error_code(std::errc::io_error), "private durability diagnostic");
                    if (fault == 1)
                        throw nickel::ErrorException(nickel::Error{"cancelled", "private callback diagnostic",
                            nickel::Json{{"nativeError", "private-value"}, {"privatePath", "private-path"}}});
                    if (fault == 2) throw std::runtime_error("private runtime diagnostic");
                    throw 17;
                });
                try {
                    if (fault & 1) {
                        auto file = sd::SecureFile::open_publication(leaf, token.file_identity);
                        file.remove_verified(token);
                    } else sd::remove_regular_file(token);
                } catch (const nickel::ErrorException& error) {
                    caught = true;
                    const auto& value = error.error();
                    require(value.code == "remove_persistence_pending" &&
                            value.message == "Regular file removal committed; durability is pending",
                            "postcommit failure hid the stable committed-removal error");
                    require(value.details.at("committed") == true && value.details.at("removed") == true &&
                            value.details.at("removedBytes") == token.size && value.details.at("durabilityPending") == true,
                            "postcommit failure lost authorized removal facts");
                    require(value.details.size() == static_cast<std::size_t>(fault == 0 ? 5 : 4),
                            "postcommit failure leaked non-contract details");
                    if (fault == 0)
                        require(value.details.at("nativeError") == std::make_error_code(std::errc::io_error).value(),
                                "postcommit failure lost numeric native error");
                    require(std::string_view(error.what()).find("private") == std::string_view::npos &&
                            value.details.dump().find("private") == std::string::npos,
                            "postcommit failure exposed private nested diagnostics");
                }
            }
            require(reached && caught && !path_exists(leaf), "postcommit fault did not leave committed removal visible");
            write_new(leaf, "foreign replacement");
            rejects_code([&] { sd::remove_regular_file(token); }, "revision_conflict", "old authorization removed replacement");
            require(read(leaf) == "foreign replacement", "committed failure restored or retried against a replacement");
        }
    }
    std::cout << "PASS removal precommit cancel/native error and committed late-stop/system/typed/runtime/opaque fault facts, zero/nonzero bytes, no retry, private diagnostics\n";
}
#endif
void bounded_capture_tests(const fs::path& root, const fs::path& leaf) {
    rejects_code([&] { sd::capture_regular_file(root, leaf, 2); }, "file_limits", "capture byte quota has wrong error code");
    require(sd::capture_regular_file(root, leaf, 3).size == 3, "exact capture byte quota rejected");
    const auto empty = root / "empty";
    write_new(empty, {});
    sd::remove_regular_file(sd::capture_regular_file(root, empty, 0));
    rejects_code([&] { sd::capture_regular_file(root, leaf, 3, [] { return true; }); }, "cancelled", "capture cancellation has wrong error code");
    rejects_code([&] { sd::capture_regular_file(root, root.parent_path() / "outside-authorization", 3); }, "outside_root", "capture outside-root has wrong error code");
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    std::uint64_t hashes{};
    Hook hook([&](auto point, const auto&) { if (point == "regular.hash") ++hashes; });
    rejects_code([&] { sd::capture_regular_file(root, leaf, 2); }, "file_limits", "over-budget capture accepted");
    require(hashes == 0, "capture byte budget was checked after hashing");
#endif
}
void run(const fs::path& parent) {
    Root root(parent);
    const auto leaf = root.path / "authorized";
    write_new(leaf, "abc");
    auto token = sd::capture_regular_file(root.path, leaf);
    require(token.size == 3 && token.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "capture did not hash complete fixed-handle bytes");
    require(token.root_identity == root.identity && token.parent_identity == root.identity,
            "capture parent identity was not fixed");
    bounded_capture_tests(root.path, leaf);
    {
        auto input = sd::SecureFile::open_read(leaf);
        rejects_code([&] { input.remove_verified(token); }, "revision_conflict", "ordinary read handle was accepted for deletion");
#ifdef _WIN32
        rejects_code([&] { sd::remove_regular_file(token); }, "revision_conflict", "non-DELETE-shared read handle did not block deletion");
#endif
    }
    sd::remove_regular_file(token); require(!path_exists(leaf), "authorized leaf was not deleted");
    rejects([&] { sd::remove_regular_file(token); }, "missing authorized leaf was accepted twice");
    write_new(leaf, "abc"); token = sd::capture_regular_file(root.path, leaf);
    fs::rename(sd::io_path(leaf), sd::io_path(root.path / "saved-original"));
    write_new(leaf, "abc");
    rejects([&] { sd::remove_regular_file(token); }, "same-content identity substitution was deleted");
    require(read(leaf) == "abc" && read(root.path / "saved-original") == "abc", "identity conflict lost content");
    token = sd::capture_regular_file(root.path, leaf);
    const auto modified = fs::last_write_time(sd::io_path(leaf));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    {
        auto writer = sd::SecureFile::open_owned(leaf, token.file_identity);
        writer.seek(0); writer.write(std::as_bytes(std::span("xyz", 3))); writer.flush();
    }
    fs::last_write_time(sd::io_path(leaf), modified);
    rejects([&] { sd::remove_regular_file(token); }, "same-size restored-mtime mutation was deleted");
    require(read(leaf) == "xyz", "content conflict lost modified file");
    token = sd::capture_regular_file(root.path, leaf);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    fs::last_write_time(sd::io_path(leaf), modified + std::chrono::seconds(1));
    fs::last_write_time(sd::io_path(leaf), modified);
    rejects([&] { sd::remove_regular_file(token); }, "native change revision was replaced by mtime-only acceptance");
    const auto linked = root.path / "linked";
    fs::create_hard_link(sd::io_path(leaf), sd::io_path(linked));
    rejects([&] { sd::capture_regular_file(root.path, leaf); }, "hardlinked capture accepted");
    rejects([&] { sd::remove_regular_file(token); }, "hardlinked deletion accepted");
    require(path_exists(leaf) && path_exists(linked), "hardlink conflict removed content");
    fs::remove(sd::io_path(linked));
    token = sd::capture_regular_file(root.path, leaf);
    auto invalid = token; invalid.path = root.path.parent_path() / "outside-authorization";
    rejects_code([&] { sd::remove_regular_file(invalid); }, "outside_root", "out-of-root authorization has wrong error code");
    rejects_code([&] { sd::remove_regular_file(token, [] { return true; }); }, "cancelled", "cancelled deletion has wrong error code");
    require(read(leaf) == "xyz", "cancelled deletion changed content");
    const auto directory = root.path / "sub";
    sd::create_directory_new(directory); write_new(directory / "file", "parent bytes");
    const auto parent_token = sd::capture_regular_file(root.path, directory / "file");
    fs::rename(sd::io_path(directory), sd::io_path(root.path / "old-sub"));
    sd::create_directory_new(directory); write_new(directory / "file", "foreign parent bytes");
    rejects([&] { sd::remove_regular_file(parent_token); }, "replacement parent directory accepted");
    require(read(directory / "file") == "foreign parent bytes" && read(root.path / "old-sub/file") == "parent bytes",
            "parent substitution lost content");
    const auto unicode = root.path / sd::utf8_path("\xe6\xb8\x85\xe7\x90\x86-\xc3\xa9.bin");
    write_new(unicode, "unicode"); sd::remove_regular_file(sd::capture_regular_file(root.path, unicode));
    require(!path_exists(unicode), "Unicode leaf deletion failed");
    const auto large = root.path / "large.bin";
    {
        auto writer = sd::SecureFile::create_new(large);
        std::array<std::byte, 64 * 1024> block{}; block.fill(std::byte{0x5a});
        for (int at = 0; at < 145; ++at) writer.write(block);
        writer.flush();
    }
    rejects_code([&] { sd::capture_regular_file(root.path, large, 8ULL * 1024 * 1024); }, "file_limits", "large capture exceeded byte quota");
    std::uint64_t checks{};
    rejects([&] { sd::capture_regular_file(root.path, large, [&] { return ++checks >= 4; }); },
            "capture cancellation during full-file hashing ignored");
    require(path_exists(large), "cancelled capture changed large file");
    const auto large_token = sd::capture_regular_file(root.path, large);
    require(large_token.size > 8 * 1024 * 1024 && large_token.sha256 ==
            nickel::transport::detail::hash_file(large, {nickel::transport::HashAlgorithm::Sha256}).front().hex,
            "large capture used an 8 MiB hash shortcut");
    checks = 0;
    rejects([&] { sd::remove_regular_file(large_token, [&] { return ++checks >= 5; }); },
            "deletion cancellation during hashing ignored");
    require(path_exists(large), "hashing cancellation removed large file");
    sd::remove_regular_file(large_token); require(!path_exists(large), "large verified deletion failed");
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    token = sd::capture_regular_file(root.path, leaf);
    bool cancel{};
    {
        Hook hook([&](auto point, const auto&) { if (point == "regular.validated") cancel = true; });
        rejects([&] { sd::remove_regular_file(token, [&] { return cancel; }); }, "post-validation cancellation ignored");
    }
    require(read(leaf) == "xyz", "post-validation cancellation deleted content");
#ifndef _WIN32
    for (const auto* checkpoint : {"regular.validated", "regular.quarantining"}) {
        token = sd::capture_regular_file(root.path, leaf);
        const auto saved = root.path / (std::string("swap-") + checkpoint);
        {
            Hook hook([&](auto point, const auto& path) {
                if (point != checkpoint) return;
                fs::rename(path, saved); write_new(path, "foreign swap");
            });
            rejects_code([&] { sd::remove_regular_file(token); }, "revision_conflict", "concurrent leaf swap deleted unauthorized content");
        }
        require(read(leaf) == "foreign swap" && read(saved) == "xyz", "swap conflict lost content");
        sd::remove_regular_file(sd::capture_regular_file(root.path, leaf)); fs::rename(saved, leaf);
    }
    token = sd::capture_regular_file(root.path, leaf);
    const int writer = ::open(leaf.c_str(), O_WRONLY | O_CLOEXEC);
    require(writer >= 0, "concurrent writer fixture open failed");
    {
        Hook hook([&](auto point, const auto&) {
            if (point == "regular.quarantined") {
                require(::pwrite(writer, "changed data", 12, 0) == 12, "concurrent writer fixture write failed");
                require(::fsync(writer) == 0, "concurrent writer fixture sync failed");
            }
        });
        rejects_code([&] { sd::remove_regular_file(token); }, "revision_conflict", "concurrent quarantined writer deleted changed content");
    }
    ::close(writer); require(read(leaf) == "changed data", "writer conflict was not restored safely");
    token = sd::capture_regular_file(root.path, leaf);
    {
        Hook hook([&](auto point, const auto&) {
            if (point != "regular.quarantined") return;
            write_new(leaf, "foreign replacement"); cancel = true;
        });
        rejects_code([&] { sd::remove_regular_file(token, [&] { return cancel; }); }, "revision_conflict", "occupied original path was overwritten by restoration");
    }
    require(read(leaf) == "foreign replacement", "no-replace restoration overwrote foreign file");
    bool retained{};
    for (const auto& entry : fs::directory_iterator(root.path)) {
        if (entry.path().filename().string().starts_with(".nickel-delete-")) {
            require(read(entry.path() / "leaf") == "changed data", "conflict quarantine lost captured bytes");
            retained = true;
        }
    }
    require(retained, "restoration conflict did not retain quarantine");
    std::cout << "PASS POSIX validated/quarantining swap and quarantined writer/no-replace restore retention; hostile same-UID final unlink is outside the OS guarantee\n";
#endif
#endif
    std::cout << "PASS fixed identity/revision/full SHA256/guarded leaf delete/stale hash+mtime/native change/parent swap/hardlink/cancel/Unicode/large file\n";
}
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
void hold(const fs::path& root, const fs::path& leaf) {
    const auto token = sd::capture_regular_file(root, leaf);
    Hook hook([&](auto point, const auto&) {
        if (point != "regular.validated") return;
        std::cout << "READY\n" << std::flush;
        std::string command; std::getline(std::cin, command);
        require(command == "continue", "external mutation fixture did not authorize continuation");
    });
    sd::remove_regular_file(token);
    std::cout << "PASS external guarded deletion\n";
}
#endif
}
int main(int argc, char** argv) {
    try {
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        if (argc == 4 && std::string_view(argv[1]) == "--hold") {
            hold(sd::utf8_path(argv[2]), sd::utf8_path(argv[3])); return 0;
        }
#endif
        const auto parent = argc > 1 ? sd::utf8_path(argv[1]) : fs::path("native/tests/transport/build-full/tmp");
        run(parent);
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        removal_commit_tests(parent);
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL leaf: " << error.what() << '\n'; return 1; }
}
