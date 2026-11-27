#include "core/archive/archive.hpp"
#include "core/common/cancellation.hpp"
#include "core/tasks/tasks.hpp"
#include "core/tasks/secure_fs.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
namespace fs = std::filesystem;
namespace sd = nickel::tasks::detail;
using nickel::archive::Archive;
using nickel::archive::Limits;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool path_exists(const fs::path& path) { return fs::exists(sd::io_path(path)); }
bool path_directory(const fs::path& path) { return fs::is_directory(sd::io_path(path)); }
template<class Function> void rejected(Function&& function, const char* message) {
    bool caught{};
    try { function(); } catch (const nickel::ErrorException&) { caught = true; }
    require(caught, message);
}
std::string text(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void read_only_tests(const fs::path& fixtures) {
    nickel::Cancellation cancel;
    Limits limits; limits.max_total_bytes = 32 * 1024 * 1024; limits.max_entry_bytes = 16 * 1024 * 1024;
    const auto members = Archive::inspect_members(fixtures / "readonly.jar", limits, cancel);
    require(members.size() == 2, "read-only JAR with DOS-reserved unrelated member rejected");
    require(text(Archive::read_member_exact(fixtures / "readonly.jar", "skin.png", 1024, cancel, limits)) == "archive payload\n",
            "exact read failed with unrelated CON.class");
    require(text(Archive::read_member_exact(fixtures / "readonly.jar", "CON.class", 1024, cancel, limits)) == "class bytes",
            "read-only DOS-reserved member read failed");
    rejected([&] { Archive::read_member_exact(fixtures / "readonly.jar", "skin.png/", 1024, cancel, limits); }, "nonexact member name accepted");
    rejected([&] { Archive::read_member_exact(fixtures / "readonly.jar", "SKIN.png", 1024, cancel, limits); }, "case-insensitive exact read accepted");
    rejected([&] { Archive::read_member_exact(fixtures / "readonly.jar", "skin.png", 1, cancel, limits); }, "read-only member quota ignored");
    rejected([&] { Archive::list(fixtures / "readonly.jar", limits, cancel); }, "extraction naming policy weakened");
    rejected([&] { Archive::validate_relative_path("CON.class"); }, "disk output accepted DOS-reserved component");
    for (const auto* name : {"crc.zip", "deflate-corrupt.zip", "symlink.zip", "hardlink.tar", "symlink.tar", "fifo.tar", "traversal.tar", "encrypted.zip", "traversal.zip", "nul-name.zip", "payload-truncated.zip", "duplicate.zip"})
        rejected([&] { Archive::inspect_members(fixtures / name, limits, cancel); }, "read-only naming weakened structural safety");
    // case.tar holds Case.txt and case.txt: rejected on volumes whose name matching
    // folds case (Windows NTFS, macOS APFS default) and accepted as two distinct
    // members on byte-exact volumes (Linux ext4, case-sensitive APFS). The decision
    // follows the fixtures volume instead of assuming platform semantics.
    if (!sd::volume_case_sensitive(fixtures))
        rejected([&] { Archive::inspect_members(fixtures / "case.tar", limits, cancel); }, "case conflict accepted");
    else
        require(Archive::inspect_members(fixtures / "case.tar", limits, cancel).size() == 2,
                "case-distinct members rejected");
    rejected([&] { Archive::read_member_exact(fixtures / "unrelated-crc.zip", "skin.png", 1024, cancel, limits); }, "exact read skipped unrelated CRC validation");
    auto small = limits; small.max_entries = 1;
    rejected([&] { Archive::inspect_members(fixtures / "readonly.jar", small, cancel); }, "read-only entry quota ignored");
    small = limits; small.max_total_bytes = 4;
    rejected([&] { Archive::read_member_exact(fixtures / "readonly.jar", "CON.class", 1024, cancel, small); }, "unrelated member total quota ignored");
    nickel::Cancellation stopped; stopped.cancel();
    rejected([&] { Archive::inspect_members(fixtures / "readonly.jar", limits, stopped); }, "cancelled inspection accepted");
    rejected([&] { Archive::read_member_exact(fixtures / "readonly.jar", "skin.png", 1024, stopped, limits); }, "cancelled exact read accepted");
    std::cout << "PASS read-only JAR reserved member/exact read/full traversal/CRC/path/link/case/duplicate/quota/cancellation and strict disk-output policy\n";
}
void run(const fs::path& fixtures) {
    nickel::Cancellation cancel;
    Limits limits; limits.max_total_bytes = 32 * 1024 * 1024; limits.max_entry_bytes = 16 * 1024 * 1024;
    std::cerr << "archive phase: ZIP list\n";
    const auto entries = Archive::list(fixtures / "valid.zip", limits, cancel);
    require(entries.size() == 4, "ZIP listing lost entries");
    std::cerr << "archive phase: ZIP read\n";
    require(text(Archive::read_entry(fixtures / "valid.zip", "dir/data.txt", 1024, cancel, limits)) == "archive payload\n", "ZIP read corrupted payload");
    require(text(Archive::read_entry(fixtures / "valid.zip", "dir/data.txt", 64ULL * 1024 * 1024, cancel, limits)) == "archive payload\n", "business read quota incorrectly reserved 64 MiB");
    std::cerr << "archive phase: empty ZIP\n";
    require(Archive::list(fixtures / "empty.zip", limits, cancel).empty(), "empty ZIP not empty");
    rejected([&] { Archive::read_entry(fixtures / "valid.zip", "dir/data.txt", 1, cancel, limits); }, "memory cap not enforced");
    rejected([&] { Archive::read_entry(fixtures / "valid.zip", "missing", 1024, cancel, limits); }, "missing ZIP entry accepted");
    read_only_tests(fixtures);
    rejected([&] { Archive::extract(fixtures / "readonly.jar", fixtures / "readonly-output", {}, cancel); }, "DOS-reserved member extracted");
    require(!path_exists(fixtures / "readonly-output"), "read-only naming policy leaked into disk output");
    for (const auto* name : {"crc.zip", "deflate-corrupt.zip", "symlink.zip", "hardlink.tar", "symlink.tar", "fifo.tar", "traversal.tar", "encrypted.zip", "traversal.zip", "nul-name.zip", "payload-truncated.zip"}) {
        rejected([&] { Archive::validate(fixtures / name, limits, cancel); }, "hostile archive accepted");
        const auto target = fixtures / (std::string("reject-") + name);
        rejected([&] { Archive::extract(fixtures / name, target, {}, cancel); }, "hostile archive extracted");
        require(!path_exists(target), "hostile archive published target");
    }
    if (!sd::volume_case_sensitive(fixtures))
        rejected([&] { Archive::validate(fixtures / "case.tar", limits, cancel); }, "case conflict validated");
    else {
        Archive::validate(fixtures / "case.tar", limits, cancel);
        Archive::extract(fixtures / "case.tar", fixtures / "case-extract", {}, cancel);
        require(path_exists(fixtures / "case-extract/Case.txt") && path_exists(fixtures / "case-extract/case.txt"),
                "case-distinct extraction failed");
    }
    for (const auto* name : {"valid.tar", "valid.tar.gz", "valid.tar.bz2", "valid.tar.xz", "valid.tar.zst"}) {
        require(Archive::list(fixtures / name, limits, cancel).size() == 2, "TAR/filter listing failed");
        require(text(Archive::read_entry(fixtures / name, "dir/data.txt", 1024, cancel, limits)) == "archive payload\n", "TAR/filter payload corrupted");
    }
    const auto legacy = Archive::list(fixtures / "legacy-maven.zip", limits, cancel);
    require(legacy.size() == 2 && legacy.front().directory, "legacy Maven directory attributes rejected");
    Archive::extract(fixtures / "legacy-maven.zip", fixtures / "legacy-maven", {}, cancel);
    require(text(sd::read_bounded(fixtures / "legacy-maven/META-INF/maven/pom.xml", 1024)) == "archive payload\n", "legacy Maven directory extraction lost data");
    nickel::archive::ExtractOptions runtime; runtime.subtrees = {"controlled-jre"};
    Archive::extract(fixtures / "runtime.zip", fixtures / "java", runtime, cancel);
    runtime.allow_owned_overlay = true; runtime.existing_owned_files = {"bin/java.exe", "release"};
    Archive::extract(fixtures / "runtime.zip", fixtures / "java", runtime, cancel);
    require(sd::read_bounded(fixtures / "java/bin/java.exe", 2048) == sd::read_bounded(fixtures / "source.bin", 2048), "repeated runtime extraction corrupted executable");
    auto small = limits; small.max_entries = 1;
    rejected([&] { Archive::validate(fixtures / "valid.zip", small, cancel); }, "entry quota accepted");
    small = limits; small.max_total_bytes = 4;
    rejected([&] { Archive::validate(fixtures / "valid.zip", small, cancel); }, "total quota accepted");
    small = limits; small.max_expansion_ratio = 2;
    rejected([&] { Archive::validate(fixtures / "bomb.zip", small, cancel); }, "ZIP bomb accepted");
    const auto receipt = Archive::extract(fixtures / "valid.zip", fixtures / "extracted", {}, cancel);
    require(receipt.files.size() == 2 && text(sd::read_bounded(fixtures / "extracted/dir/data.txt", 1024)) == "archive payload\n", "ZIP extraction lost data");
    require(path_directory(fixtures / "extracted/empty"), "empty directory lost");
    sd::write_atomic(fixtures / "extracted/foreign.txt", "preserve");
    nickel::archive::ExtractOptions overlay; overlay.allow_owned_overlay = true;
    overlay.existing_owned_files = {"dir/data.txt", "other.bin"};
    Archive::extract(fixtures / "valid.zip", fixtures / "extracted", overlay, cancel);
    require(text(sd::read_bounded(fixtures / "extracted/foreign.txt", 1024)) == "preserve", "overlay changed unselected file");
    rejected([&] { Archive::extract(fixtures / "valid.zip", fixtures / "extracted", {}, cancel); }, "unleased overlay accepted");
    nickel::archive::ExtractOptions subtree; subtree.subtrees = {"dir"};
    Archive::extract(fixtures / "valid.zip", fixtures / "subtree", subtree, cancel);
    require(path_exists(fixtures / "subtree/data.txt") && !path_exists(fixtures / "subtree/other.bin"), "subtree selection failed");
    rejected([&] { Archive::extract(fixtures / "protected.zip", fixtures / "protected", {}, cancel); }, "launcher control path extracted");
    nickel::Cancellation stopped;
    rejected([&] { Archive::extract(fixtures / "valid.zip", fixtures / "cancelled", {}, stopped, [&](const auto&) { stopped.cancel(); }); }, "cancel during extraction published");
    require(!path_exists(fixtures / "cancelled"), "cancelled extraction target exists");
    std::vector<nickel::archive::ZipSource> sources = {{"dir/data.txt", fixtures / "source.txt"}, {"other.bin", fixtures / "source.bin"}};
    Archive::write_zip(fixtures / "roundtrip.zip", sources, limits, cancel);
    require(text(Archive::read_entry(fixtures / "roundtrip.zip", "dir/data.txt", 1024, cancel, limits)) == "archive payload\n", "ZIP export roundtrip failed");
    rejected([&] { Archive::write_zip(fixtures / "roundtrip.zip", sources, limits, cancel); }, "ZIP export overwrote target");
    nickel::Cancellation export_cancel;
    rejected([&] { Archive::write_zip(fixtures / "cancel-export.zip", sources, limits, export_cancel, [&](const auto&) { export_cancel.cancel(); }); }, "cancelled ZIP export published");
    require(!path_exists(fixtures / "cancel-export.zip"), "cancelled ZIP export target exists");
    Archive::validate_executable_jar(fixtures / "valid.jar", {}, cancel, limits);
    rejected([&] { Archive::validate_executable_jar(fixtures / "invalid.jar", {}, cancel, limits); }, "invalid executable JAR accepted");
    std::cout << "PASS libarchive ZIP/TAR/gzip/bzip2/xz/zstd/CRC/payload/path/link/quota/roundtrip/subtree/overlay/cancellation/JAR\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--readonly") {
            read_only_tests(sd::checked_absolute(sd::utf8_path(argv[2]))); return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--inspect") {
            nickel::Cancellation cancel;
            const auto entries = Archive::list(sd::utf8_path(argv[2]), {}, cancel);
            std::cout << "PASS actual archive inspection entries=" << entries.size() << '\n'; return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--crash") {
            nickel::Cancellation cancel;
            const auto root = sd::checked_absolute(sd::utf8_path(argv[2]));
            Archive::extract(root / "valid.zip", root / "crash-extract", {}, cancel, [](const auto&) { std::_Exit(0); });
            throw std::runtime_error("crash callback did not run");
        }
        if (argc == 3 && std::string_view(argv[1]) == "--recover") {
            nickel::Cancellation cancel;
            const auto root = sd::checked_absolute(sd::utf8_path(argv[2]));
            auto entries = nickel::tasks::Registry::process()->recover(root, cancel);
            require(entries.size() == 1 && entries.front().recovered && !entries.front().committed, "archive shared task recovery failed");
            require(!path_exists(root / "crash-extract"), "crashed archive published");
            std::cout << "PASS archive shared-task process recovery\n"; return 0;
        }
        require(argc == 2, "archive fixture root required"); run(sd::checked_absolute(sd::utf8_path(argv[1]))); return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL archive runtime: " << error.what() << '\n'; return 1; }
}
