#include "core/tasks/secure_fs.hpp"
#include "core/network/hash.hpp"
#include "core/network/budget.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace nickel;
namespace fs = std::filesystem;
namespace sd = tasks::detail;
using namespace std::chrono_literals;

void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Action> void rejected(Action action, const char* message) {
    bool threw{};
    try { action(); } catch (const std::exception&) { threw = true; }
    expect(threw, message);
}
void write_file(const fs::path& path, std::string_view text) {
    auto file = sd::SecureFile::create_new(path);
    file.write(std::as_bytes(std::span(text.data(), text.size())));
    file.flush();
}

// override_volume_case_sensitive() is a process-global test hook with manual reset and
// no RAII of its own; a thrown assertion between set and reset would leak the forced
// verdict into later checks. Scope every override so all exit paths restore the real
// volume decision.
class VolumeCaseOverride {
public:
    explicit VolumeCaseOverride(bool case_sensitive) {
        sd::override_volume_case_sensitive(case_sensitive);
    }
    ~VolumeCaseOverride() { sd::override_volume_case_sensitive(std::nullopt); }
    VolumeCaseOverride(const VolumeCaseOverride&) = delete;
    VolumeCaseOverride& operator=(const VolumeCaseOverride&) = delete;
};

int main() {
    fs::path root;
    try {
        const auto parent = fs::absolute("native/tests/transport/build-probe");
        { sd::ParentGuard pin(parent, true); }
        root = parent / sd::utf8_path("case-" + sd::unique_id());
        sd::create_directory_new(root);
        const auto owned = sd::directory_identity(root);
        for (const auto* name : {"../outside", "/absolute", "//server/share", "C:/drive", "a/../b", "a\\b", "a//b", "NUL.txt", "CON", "COM1.jar", "a./b", "a /b", "a:b", "a\x01"}) {
            rejected([&] { (void)sd::validate_relative(name); }, "unsafe path accepted");
        }
        expect(sd::validate_relative("mods/a.jar") == "mods/a.jar", "safe path rejected");
        // Collision keys follow the volume actually in use: folded on case-insensitive
        // volumes (Windows NTFS, macOS APFS default), byte-exact on case-sensitive ones
        // (Linux ext4, case-sensitive APFS). Assert the real host behavior first, then
        // force both decisions explicitly so every branch is covered on every host.
        const bool host_folds = !sd::default_volume_case_sensitive();
        if (host_folds) {
            expect(sd::case_key("Mods/A.JAR") == "mods/a.jar", "case folding failed");
            expect(sd::case_key("A.txt") == sd::case_key("a.txt"), "case key not folded");
            expect(sd::paths_overlap(root / "Mods", root / "mods/a.jar"), "overlap not case insensitive");
        } else {
            expect(sd::case_key("Mods/A.JAR") == "Mods/A.JAR", "case-sensitive key not byte-exact");
            expect(sd::case_key("A.txt") != sd::case_key("a.txt"), "case-sensitive key folded case");
            expect(!sd::paths_overlap(root / "Mods", root / "mods/a.jar"), "overlap not byte-exact");
        }
        // override_volume_case_sensitive() takes the volume verdict itself: true means a
        // case-sensitive volume (byte-exact keys), false means the volume folds names.
        // Pass the target semantics explicitly; deriving the argument from host_folds
        // inverted the forced verdict on Windows, where the host always folds.
        {
            const VolumeCaseOverride forced_byte_exact(true);
            expect(sd::case_key("Mods/A.JAR") == "Mods/A.JAR", "forced byte-exact key folded");
            expect(sd::case_key("A.txt") != sd::case_key("a.txt"), "forced byte-exact key collapsed");
            expect(!sd::paths_overlap(root / "Mods", root / "mods/a.jar"), "forced overlap not byte-exact");
        }
        {
            const VolumeCaseOverride forced_folding(false);
            expect(sd::case_key("Mods/A.JAR") == "mods/a.jar", "forced folding key not folded");
            expect(sd::case_key("A.txt") == sd::case_key("a.txt"), "forced folding lost");
            expect(sd::paths_overlap(root / "Mods", root / "mods/a.jar"), "forced folding lost overlap");
        }
        const std::string unicode = "config/\xe9\x85\x8d\xe7\xbd\xae.txt";
        expect(sd::validate_relative(unicode) == unicode, "non-ASCII relative path rejected");
        expect(sd::case_key(unicode) == unicode, "non-ASCII case key changed");
        expect(sd::case_key(unicode) != sd::case_key(std::string(unicode) + "x"),
               "case keys collided for distinct names");
        expect(!sd::paths_overlap(root / "mods", root / "modstuff"), "boundary overlap false positive");
        write_file(root / "artifact", "abc");
        const auto digests = transport::detail::hash_file(root / "artifact", {
            transport::HashAlgorithm::Sha1, transport::HashAlgorithm::Sha256,
            transport::HashAlgorithm::Sha512, transport::HashAlgorithm::Md5});
        expect(digests[0].hex == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA1 vector failed");
        expect(digests[1].hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 vector failed");
        expect(digests[2].hex == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", "SHA512 vector failed");
        expect(digests[3].hex == "900150983cd24fb0d6963f7d28e17f72", "MD5 vector failed");
        rejected([&] { write_file(root / "artifact", "different"); }, "exclusive creation overwrote file");
        write_file(root / "stage", "new");
        rejected([&] { sd::rename_no_replace(root / "stage", root / "artifact"); }, "publication overwrote foreign artifact");
        sd::rename_no_replace(root / "stage", root / "published");
        expect(fs::exists(root / "published") && !fs::exists(root / "stage"), "no-replace publish failed");
        sd::write_atomic(root / "journal", "before");
        sd::write_atomic(root / "journal", "after");
        const auto body = sd::read_bounded(root / "journal", 16);
        expect(std::string(reinterpret_cast<const char*>(body.data()), body.size()) == "after", "atomic journal update failed");
        rejected([&] { (void)sd::read_bounded(root / "artifact", 2); }, "read limit ignored");
        auto wrong = owned; ++wrong.index;
        rejected([&] { sd::remove_owned_tree(root, wrong); }, "foreign identity removal accepted");

        auto budget = std::make_shared<transport::SharedBudget>(transport::BudgetLimits{1, 0, 65536});
        auto permit = budget->acquire_connection({});
        std::atomic<bool> cancel{};
        const auto start = std::chrono::steady_clock::now();
        auto pending = std::async(std::launch::async, [&] {
            rejected([&] { auto waiting = budget->acquire_connection([&] { return cancel.load(); }); }, "queued cancellation ignored");
        });
        std::this_thread::sleep_for(40ms); cancel = true; budget->wake(); pending.get();
        expect(std::chrono::steady_clock::now() - start < 1s, "queued cancellation too slow");
        expect(budget->stats().queued_connections == 0, "cancelled queue ticket leaked");
        budget->configure({2, 0, 65536});
        auto second = budget->acquire_connection({});
        expect(budget->stats().active_connections == 2, "live concurrency increase not applied");
        budget->configure({1, 32768, 65536});
        expect(budget->stats().active_connections == 2, "decrease destroyed active permits");
        permit = {}; second = {};
        auto buffer = budget->reserve_buffer(65536, {});
        rejected([&] { auto too_big = budget->reserve_buffer(65537, {}); }, "oversized buffer admitted");
        buffer = {};
        const auto rate_start = std::chrono::steady_clock::now();
        budget->throttle(16384, {});
        expect(std::chrono::steady_clock::now() - rate_start >= 400ms, "aggregate rate budget ignored");
        expect(budget->stats().reserved_buffer_bytes == 0 && budget->stats().active_connections == 0,
               "budget reservation leaked");
        sd::remove_owned_tree(root, owned);
        expect(!fs::exists(root), "owned cleanup incomplete");
        std::cout << "PASS path/device/case/owned identity/no-overwrite/atomic journal/hash vectors/bounded IO/FIFO cancellation/dynamic concurrency/rate/buffer budgets\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << " root=" << sd::path_utf8(root) << '\n';
        return 1;
    }
}
