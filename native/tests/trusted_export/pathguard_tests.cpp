#include "core/common/cancellation.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using nickel::Json;
using nickel::ErrorException;
using nickel::foundation_detail::DirectoryGuard;
using nickel::foundation_detail::checked_absolute_path;
using nickel::foundation_detail::native_filesystem_path;

void check(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

template<class Work>
std::string failure(Work work) {
    try { work(); }
    catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Negative PathGuard operation unexpectedly succeeded");
}

void write_new(const fs::path& path, std::string_view data) {
    nickel::create_file_exclusive(path, {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
}

Json case_result(std::string_view id, const std::function<Json()>& work) {
    Json value{{"id", id}, {"status", "FAIL"}, {"scope", "foundation-primitives-only"}, {"executed", true}};
    try { value["evidence"] = work(); value["status"] = "PASS"; }
    catch (const ErrorException& error) { value["code"] = error.error().code; value["failure"] = error.what(); }
    catch (const std::exception& error) { value["failure"] = error.what(); }
    std::cout << value.dump() << '\n';
    return value;
}
}

int main(int argc, char* argv[]) {
    if (argc != 2) { std::cerr << "Owned absolute fixture directory required\n"; return 1; }
    try {
        const auto base = checked_absolute_path(nickel::path_from_utf8(argv[1]));
        check(!fs::exists(native_filesystem_path(base)), "Fixture must be a new isolated directory");
        fs::create_directory(native_filesystem_path(base));
        const auto source = base / "source.exe";
        write_new(source, "inert unexecuted file bytes");
        Json cases = Json::array();
        const auto add = [&](std::string_view id, const std::function<Json()>& work) { cases.push_back(case_result(id, work)); };
        add("path-traversal-denied", [&] {
            const auto code = failure([&] { checked_absolute_path(base / ".." / "outside.exe"); });
            check(code == "UnsafePath", "Wrong traversal denial");
            return Json{{"code", code}};
        });
#ifdef _WIN32
        add("path-device-namespace-denied", [&] {
            const auto code = failure([&] { checked_absolute_path(fs::path(L"\\\\.\\NUL")); });
            check(code == "UnsafePath", "Wrong device namespace denial");
            return Json{{"code", code}};
        });
        add("path-ads-denied", [&] {
            const auto code = failure([&] { checked_absolute_path(base / "source.exe:stream"); });
            check(code == "UnsafePath", "Wrong ADS denial");
            return Json{{"code", code}};
        });
        add("path-reserved-leaf-denied", [&] {
            const auto code = failure([&] { checked_absolute_path(base / "COM1.txt"); });
            check(code == "UnsafePath", "Wrong device leaf denial");
            return Json{{"code", code}};
        });
        add("directory-pin-prevents-rename", [&] {
            const auto directory = base / "pinned";
            fs::create_directory(native_filesystem_path(directory));
            DirectoryGuard guard(directory);
            const auto moved = base / "pinned-moved";
            const auto renamed = MoveFileExW(native_filesystem_path(directory).c_str(), native_filesystem_path(moved).c_str(), 0);
            const auto error = GetLastError();
            check(!renamed && (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED), "Pinned directory rename was not denied");
            guard.verify();
            return Json{{"nativeError", error}, {"directoryPreserved", true}};
        });
        add("hardlink-source-denied", [&] {
            const auto hard_source = base / "hard-source.exe";
            const auto alias = base / "hardlink.exe";
            write_new(hard_source, "inert hardlink bytes");
            check(CreateHardLinkW(native_filesystem_path(alias).c_str(), native_filesystem_path(hard_source).c_str(), nullptr) != 0,
                  "Could not inject a real owned hardlink");
            const auto target = base / "hardlink.zip";
            const auto code = failure([&] { nickel::atomic_copy_no_replace(hard_source, target, 1024); });
            check(code == "UnsafePath" && !fs::exists(native_filesystem_path(target)), "Hardlinked source was published");
            return Json{{"code", code}, {"destinationExists", false}};
        });
#endif
        add("wrong-source-sha-no-publication", [&] {
            const auto target = base / "hash-mismatch.zip";
            const auto code = failure([&] { nickel::atomic_copy_no_replace(source, target, 1024, nullptr, std::string(64, '0')); });
            check(code == "CorruptData" && !fs::exists(native_filesystem_path(target)), "SHA mismatch published output");
            return Json{{"code", code}, {"destinationExists", false}};
        });
        add("cancel-no-publication", [&] {
            const auto target = base / "cancelled.zip";
            nickel::Cancellation stop;
            stop.cancel();
            const auto code = failure([&] { nickel::atomic_copy_no_replace(source, target, 1024, &stop); });
            check(code == "Cancelled" && !fs::exists(native_filesystem_path(target)), "Cancellation published output");
            return Json{{"code", code}, {"destinationExists", false}};
        });
        add("existing-destination-preserved", [&] {
            const auto target = base / "collision.zip";
            write_new(target, "external owner bytes");
            check(!nickel::atomic_copy_no_replace(source, target, 1024), "Collision unexpectedly published");
            check(nickel::read_text_bounded(target, 1024) == "external owner bytes", "Collision overwrote existing bytes");
            return Json{{"existingPreserved", true}};
        });
        add("actual-source-sha-copy-control", [&] {
            const auto target = base / "small-control.zip";
            const auto original = nickel::read_text_bounded(source, 1024);
            const auto sha = nickel::foundation_detail::sha256_text(original);
            check(nickel::atomic_copy_no_replace(source, target, 1024, nullptr, sha), "Valid small copy did not publish");
            check(nickel::read_text_bounded(target, 1024) == original, "Valid small copy differs");
            return Json{{"bytes", original.size()}, {"sha256", sha}, {"scope", "inert-small-primitive-control-not-launcher"}};
        });
        Json result{{"schema", "pd.trusted-export.pathguard.v1"}, {"scope", "foundation-primitives-only"},
                    {"productionAppServicesTested", false}, {"actualPdRoundTripPass", false}, {"cases", cases}};
        write_new(base / "result.json", result.dump(2) + "\n");
        std::cout << "Foundation-only: no AppServices/launcher capability assertion\n";
        return std::any_of(cases.begin(), cases.end(), [](const auto& value) { return value.at("status") == "FAIL"; }) ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Owned primitive harness failed closed: " << error.what() << '\n';
        return 1;
    }
}
