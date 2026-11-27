#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <mutex>
#include <new>
#include <thread>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace {
std::atomic<bool> bounded_allocations{false};
std::atomic<std::size_t> largest_allocation{0};
}
void* operator new(std::size_t bytes) {
    if (bounded_allocations.load()) {
        auto previous = largest_allocation.load();
        while (previous < bytes && !largest_allocation.compare_exchange_weak(previous, bytes)) {}
        if (bytes > 1024 * 1024) throw std::bad_alloc();
    }
    if (auto* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace nickel;
using namespace nickel::foundation_detail;
namespace fs = std::filesystem;
constexpr std::uint64_t fixture_size = 9 * 1024 * 1024 + 137;
constexpr std::string_view fixture_sha = "742d5616d1ee424e54d8de4bf2ae59057c46829baf1e2c37d6a911c28ec13a34";

void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
std::string failure(const auto& callback) {
    try { callback(); } catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Restricted copy unexpectedly succeeded");
}
fs::path raw_native(fs::path path) {
#ifdef _WIN32
    path.make_preferred();
    const auto text = path.native();
    if (text.starts_with(L"\\\\?\\")) return path;
    return fs::path(text.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + text.substr(2) : L"\\\\?\\" + text);
#else
    return path;
#endif
}
void raw_write(const fs::path& path, std::string_view text) {
    fs::create_directories(raw_native(path.parent_path()));
    std::ofstream file(raw_native(path), std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(file), "Independent fixture write failed");
}
std::string raw_read(const fs::path& path) {
    std::ifstream file(raw_native(path), std::ios::binary);
    require(static_cast<bool>(file), "Independent fixture read failed");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool raw_exists(const fs::path& path) { return fs::exists(raw_native(path)); }
void no_temporaries(const fs::path& directory) {
    for (const auto& item : fs::directory_iterator(raw_native(directory))) {
        require(!path_to_utf8(item.path().filename()).starts_with(".nickel-copy-"), "Owned copy temporary was not cleaned");
    }
}
struct Root {
    fs::path top = fs::current_path() / ("foundation-streamcopy-" + unique_token());
    fs::path deep = top;
    Root() {
        require(fs::space(raw_native(fs::current_path())).available >= 128 * 1024 * 1024,
            "Insufficient fixture space: refused before writing");
        while (deep.native().size() < 520) deep /= path_from_utf8("copy-\xE6\xB5\x8B\xE8\xAF\x95 space-" + std::string(40, 'a'));
        fs::create_directories(raw_native(deep));
    }
    ~Root() { std::error_code error; fs::remove_all(raw_native(top), error); }
};
void make_large_file(const fs::path& file) {
    fs::create_directories(raw_native(file.parent_path()));
    std::ofstream stream(raw_native(file), std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    for (std::size_t i = 0; i < buffer.size(); ++i) buffer[i] = static_cast<char>(i % 251);
    std::uint64_t left = fixture_size;
    while (left) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
        stream.write(buffer.data(), static_cast<std::streamsize>(count));
        left -= count;
    }
    require(static_cast<bool>(stream), "Independent >8 MiB fixture write failed");
}
void check_large_file(const fs::path& file) {
    std::ifstream stream(raw_native(file), std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        for (std::size_t i = 0; i < count; ++i) require(static_cast<unsigned char>(buffer[i]) == i % 251, "Copied bytes changed");
        total += count;
    }
    require(stream.eof() && total == fixture_size, "Copy size or EOF changed");
}
#ifdef _WIN32
void directory_link(const fs::path& link, const fs::path& target) {
    fs::create_directory(raw_native(link));
    const auto handle = CreateFileW(raw_native(link).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Junction fixture handle unavailable");
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{handle};
    auto preferred = target;
    preferred.make_preferred();
    const auto substitute = L"\\??\\" + preferred.native();
    const auto print = preferred.native();
    const auto strings = substitute + std::wstring(1, L'\0') + print + std::wstring(1, L'\0');
    std::vector<std::uint8_t> data(16 + strings.size() * sizeof(wchar_t));
    auto put16 = [&](std::size_t offset, std::uint16_t value) { std::memcpy(data.data() + offset, &value, sizeof(value)); };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(data.data(), &tag, sizeof(tag));
    put16(4, static_cast<std::uint16_t>(data.size() - 8));
    put16(8, 0);
    put16(10, static_cast<std::uint16_t>(substitute.size() * sizeof(wchar_t)));
    put16(12, static_cast<std::uint16_t>((substitute.size() + 1) * sizeof(wchar_t)));
    put16(14, static_cast<std::uint16_t>(print.size() * sizeof(wchar_t)));
    std::memcpy(data.data() + 16, strings.data(), strings.size() * sizeof(wchar_t));
    DWORD written = 0;
    require(DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data.data(), static_cast<DWORD>(data.size()), nullptr, 0, &written, nullptr) != 0,
        "Junction fixture could not be set");
}
#else
void directory_link(const fs::path& link, const fs::path& target) { fs::create_directory_symlink(target, link); }
#endif

void test_stream(const Root& root) {
    const auto source = root.top / "source" / "pack.mrpack";
    const auto destination = root.deep / "destination" / "pack.mrpack";
    make_large_file(source);
    largest_allocation = 0;
    struct Bound { Bound() { bounded_allocations = true; } ~Bound() { bounded_allocations = false; } };
    bool copied = false;
    { Bound bound; copied = atomic_copy_no_replace(source, destination, fixture_size, nullptr, fixture_sha); }
    require(copied && largest_allocation <= 1024 * 1024, "Copy allocated a whole-file buffer");
    check_large_file(source);
    check_large_file(destination);
    require(!atomic_copy_no_replace(source, destination, fixture_size), "Copy overwrote an existing destination");
    const auto overflow_safe = root.deep / "max-uint64.data";
    require(atomic_copy_no_replace(source, overflow_safe, UINT64_MAX, nullptr, fixture_sha), "UINT64 limit overflowed");
    check_large_file(overflow_safe);
    require(failure([&] { (void)atomic_copy_no_replace(source, root.deep / "too-large.data", fixture_size - 1); }) == "LimitExceeded",
        "Copy ignored source size bound");
    require(failure([&] { (void)atomic_copy_no_replace(source, root.deep / "mismatch.data", fixture_size, nullptr, std::string(64, '0')); }) == "CorruptData",
        "Copy published hash mismatch");
    require(failure([&] { (void)atomic_copy_no_replace(source, root.deep / "invalid.data", fixture_size, nullptr, "bad"); }) == "InvalidArgument",
        "Malformed expected hash accepted");
    Cancellation cancel;
    cancel.cancel();
    require(failure([&] { (void)atomic_copy_no_replace(source, root.deep / "cancelled.data", fixture_size, &cancel); }) == "Cancelled",
        "Pre-cancelled copy published");
    for (const auto* name : {"too-large.data", "mismatch.data", "invalid.data", "cancelled.data"}) require(!raw_exists(root.deep / name), "Failed copy left a destination");
    no_temporaries(root.deep);
    no_temporaries(destination.parent_path());
    std::cout << "PASS bounded copy: source=" << fixture_size << " bytes, destination units=" << destination.native().size()
        << ", largest C++ allocation=" << largest_allocation << ", independent SHA256 and source preserved\n";
}
void test_small_and_contenders(const Root& root) {
    const auto source = root.top / "small" / "abc";
    raw_write(source, "abc");
    const auto destination = root.deep / "contenders" / "winner";
    std::latch start(1);
    std::atomic<unsigned> winners{0};
    std::exception_ptr error;
    std::mutex error_mutex;
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
        start.wait();
        try { if (atomic_copy_no_replace(source, destination, 3)) ++winners; }
        catch (...) { std::scoped_lock lock(error_mutex); error = std::current_exception(); }
    });
    start.count_down();
    for (auto& worker : workers) worker.join();
    if (error) std::rethrow_exception(error);
    require(winners == 1 && raw_read(destination) == "abc" && raw_read(source) == "abc", "Concurrent copy consumed source or replaced winner");
    no_temporaries(destination.parent_path());
    std::string uppercase = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
    require(atomic_copy_no_replace(source, root.deep / "uppercase", 3, nullptr, uppercase), "Uppercase SHA256 rejected");
    const auto empty = root.top / "empty";
    raw_write(empty, "");
    require(atomic_copy_no_replace(empty, root.deep / "empty", 0, nullptr,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "Empty bounded copy failed");
    require(raw_read(root.deep / "empty").empty(), "Empty copy gained bytes");
    require(!atomic_copy_no_replace(source, source, 3), "Same-object destination was replaced");
    std::cout << "PASS copy edge cases: zero/UINT64 size, uppercase hash, eight concurrent contenders and no-replace\n";
}
void test_links(const Root& root) {
    const auto outside = root.top / "outside";
    const auto source = outside / "source";
    raw_write(source, "outside-preserved");
    const auto source_link = outside / "alias";
    fs::create_hard_link(raw_native(source), raw_native(source_link));
    require(failure([&] { (void)atomic_copy_no_replace(source, root.deep / "unsafe-source", 64); }) == "UnsafePath", "Hard-linked copy source accepted");
    fs::remove(raw_native(source_link));
    const auto target_link = root.deep / "unsafe-target";
    fs::create_hard_link(raw_native(source), raw_native(target_link));
    require(failure([&] { (void)atomic_copy_no_replace(root.top / "small" / "abc", target_link, 3); }) == "UnsafePath", "Hard-linked destination accepted");
    require(raw_read(target_link) == "outside-preserved", "Hard-linked external target changed");
    fs::remove(raw_native(target_link));
    const auto ancestor = root.top / "linked-parent";
    directory_link(ancestor, outside);
    require(failure([&] { (void)atomic_copy_no_replace(ancestor / "source", root.deep / "linked-source", 64); }) == "UnsafePath", "Linked source ancestor accepted");
    require(failure([&] { (void)atomic_copy_no_replace(root.top / "small" / "abc", ancestor / "new", 3); }) == "UnsafePath", "Linked destination ancestor accepted");
    const auto leaf = root.top / "linked-leaf";
    directory_link(leaf, outside);
    require(failure([&] { (void)atomic_copy_no_replace(root.top / "small" / "abc", leaf, 3); }) == "UnsafePath", "Reparse destination leaf accepted");
#ifndef _WIN32
    const auto symlink = outside / "symbolic-file";
    fs::create_symlink(source, symlink);
    require(failure([&] { (void)atomic_copy_no_replace(symlink, root.deep / "symbolic-source", 64); }) == "UnsafePath", "Symbolic file source error contract changed");
#endif
    require(raw_read(source) == "outside-preserved" && !raw_exists(outside / "new"), "Link rejection changed external content");
    no_temporaries(root.deep);
    std::cout << "PASS copy path security: hardlinks, source/destination ancestor links, reparse leaf and unchanged external objects\n";
}
}

namespace {
void test_incremental_sha256() {
    for (const auto& [text, expected] : std::array<std::pair<std::string_view, std::string_view>, 3>{{
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}
    }}) {
        Sha256 hash;
        hash.update({});
        for (const auto& byte : text) hash.update({reinterpret_cast<const std::uint8_t*>(&byte), 1});
        require(hash.finish() == expected && sha256_text(text) == expected, "Independent SHA256 known-answer vector failed");
        require(failure([&] { hash.update({}); }) == "InvalidArgument", "Finished hash accepted another update");
        require(failure([&] { (void)hash.finish(); }) == "InvalidArgument", "Finished hash accepted another finalization");
    }
    Sha256 million;
    std::array<std::uint8_t, 1000> block{};
    block.fill('a');
    for (unsigned i = 0; i < 1000; ++i) million.update(block);
    require(million.finish() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "Million-byte SHA256 vector failed");
    std::cout << "PASS incremental SHA256: independent empty/abc/long/million-a vectors and single-use lifetime\n";
}
}
int main() {
    try {
        std::cout << std::unitbuf;
        test_incremental_sha256();
        Root root;
        test_stream(root);
        test_small_and_contenders(root);
        test_links(root);
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << " details=" << error.error().details.dump() << '\n';
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    return 1;
}
