#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/vault.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace nickel;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
enum class Mode { Unsupported, Winner, HalfCompletes, HalfPersistent, AtomicRace, Permission };
Mode mode = Mode::Unsupported;
std::jthread completing_writer;
std::exception_ptr completing_failure;
const VaultKey winner = [] { VaultKey key{}; key.fill(0x5A); return key; }();
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
#ifdef _WIN32
struct CompletingFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit CompletingFile(const fs::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        require(handle != INVALID_HANDLE_VALUE, "Synthetic completing writer unavailable");
    }
    ~CompletingFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    CompletingFile(const CompletingFile&) = delete;
    CompletingFile& operator=(const CompletingFile&) = delete;
    void append() {
        LARGE_INTEGER offset{};
        offset.QuadPart = 16;
        DWORD written = 0;
        require(SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN) &&
            WriteFile(handle, winner.data() + 16, 16, &written, nullptr) && written == 16 &&
            FlushFileBuffers(handle), "Synthetic completing key append failed");
    }
};
#else
void raw_write(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "Synthetic fallback fixture write failed");
}
#endif
std::string expected_failure(const auto& callback) {
    try { callback(); } catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Expected fallback failure did not occur");
}
struct Root {
    fs::path path = fs::current_path() / path_from_utf8("foundation-fallback-" + unique_token());
    Root() { fs::create_directories(path); }
    ~Root() { if (completing_writer.joinable()) completing_writer.join(); std::error_code error; fs::remove_all(path, error); }
};
}

namespace nickel::foundation_detail {
// Only this test target compiles vault.cpp with a renamed atomic-create call.
// The production library contains no runtime test switch or injected filesystem.
bool foundation_test_atomic_create_file(const std::filesystem::path& path,
                                        std::span<const std::uint8_t>, bool) {
    if (mode == Mode::Permission) fail("PermissionDenied", "Synthetic atomic-create permission failure");
    if (mode == Mode::Winner) create_file_exclusive(path, winner, true);
    if (mode == Mode::HalfCompletes || mode == Mode::HalfPersistent || mode == Mode::AtomicRace) {
        create_file_exclusive(path, std::span(winner).first(16), true);
        if (mode == Mode::HalfCompletes) {
            completing_failure = nullptr;
#ifdef _WIN32
            // Retain the writer before guarded reads can deny a pathname reopen.
            auto writer = std::make_unique<CompletingFile>(path);
            completing_writer = std::jthread([writer = std::move(writer)]() mutable {
                try {
                    std::this_thread::sleep_for(10ms);
                    writer->append();
                } catch (...) { completing_failure = std::current_exception(); }
                writer.reset();
            });
#else
            completing_writer = std::jthread([path] {
                try {
                    std::this_thread::sleep_for(10ms);
                    raw_write(path, winner);
                } catch (...) { completing_failure = std::current_exception(); }
            });
#endif
        }
    }
    if (mode == Mode::AtomicRace) return false;
    fail("Unsupported", "Synthetic filesystem without no-replace rename");
}
}

int main() {
    try {
        Root root;
        mode = Mode::Unsupported;
        Vault direct(root.path / "direct");
        const auto key = direct.load_or_create_key();
        require(direct.load_or_create_key() == key, "Fallback replaced an existing key");
        require(direct.open(direct.seal("synthetic-only")) == "synthetic-only", "Fallback key failed round trip");

        mode = Mode::Winner;
        require(Vault(root.path / "winner").load_or_create_key() == winner, "Fallback ignored concurrent winner");
        mode = Mode::HalfCompletes;
        std::optional<VaultKey> completed_key;
        std::exception_ptr reader_failure;
        try { completed_key = Vault(root.path / "half-completes").load_or_create_key(); }
        catch (...) { reader_failure = std::current_exception(); }
        if (completing_writer.joinable()) completing_writer.join();
        if (completing_failure) std::rethrow_exception(completing_failure);
        if (reader_failure) std::rethrow_exception(reader_failure);
        require(completed_key && *completed_key == winner, "Fallback half-write retry failed");
        mode = Mode::HalfPersistent;
        Vault partial(root.path / "half-persistent");
        require(expected_failure([&] { (void)partial.load_or_create_key(); }) == "AlreadyExists", "Fallback did not preserve creation-error precedence");
        require(read_file_bounded(partial.key_path(), 32) == std::vector<std::uint8_t>(winner.begin(), winner.begin() + 16), "Fallback altered persistent half-key");

        mode = Mode::AtomicRace;
        Vault raced(root.path / "atomic-race");
        require(expected_failure([&] { (void)raced.load_or_create_key(); }) == "AlreadyExists", "Atomic race replaced the original reservation error");
        require(read_file_bounded(raced.key_path(), 32).size() == 16, "Atomic race overwrote incomplete concurrent key");

        mode = Mode::Permission;
        Vault denied(root.path / "denied");
        require(expected_failure([&] { (void)denied.load_or_create_key(); }) == "PermissionDenied", "Non-unsupported error incorrectly triggered fallback");
        require(!fs::exists(denied.key_path()), "Permission failure created key through fallback");
        for (const auto& entry : fs::recursive_directory_iterator(root.path)) {
            require(path_to_utf8(entry.path().filename()).find(".tmp-") == std::string::npos, "Fallback left temporary keys");
        }
        std::cout << "PASS vault fallback: direct exclusive create, winner, completing/permanent half-key, write-error precedence, permission refusal\n";
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
