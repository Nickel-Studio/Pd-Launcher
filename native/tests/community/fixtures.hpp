#pragma once

#include "core/resources/community.hpp"
#include "core/resources/mods.hpp"
#include "core/tasks/secure_fs.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace nickel::tests {
using namespace resources;

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Function>
void expectError(std::string_view code, Function&& function) {
    try { function(); }
    catch (const ErrorException& failure) {
        require(failure.error().code == code, "Unexpected common error: " + failure.error().code + " (expected " + std::string(code) + ")");
        return;
    }
    throw std::runtime_error("Expected common error was not thrown: " + std::string(code));
}

inline void requireSpace(const Path& directory, std::uintmax_t bytesToWrite = 0) {
    const auto available = std::filesystem::space(directory).available;
    constexpr std::uintmax_t reserve = 16 * 1024 * 1024;
    require(available >= reserve && bytesToWrite <= available - reserve,
        "Insufficient available space before fixture writes");
}

class TemporaryRoot {
public:
    TemporaryRoot() : path(std::filesystem::temp_directory_path() / ("nickel-community-test-" + tasks::detail::unique_id())) {
        requireSpace(path.parent_path());
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("Cannot create isolated test root");
    }
    ~TemporaryRoot() { std::error_code error; std::filesystem::remove_all(path, error); }
    TemporaryRoot(const TemporaryRoot&) = delete;
    Path path;
};

inline void write(const Path& path, std::string_view text) {
    requireSpace(path.parent_path(), text.size());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) throw std::runtime_error("Cannot write test fixture");
}

inline Bytes bytes(std::string_view value) { return {value.begin(), value.end()}; }

inline HttpResponse response(const HttpRequest& request, const Json& json, int status = 200,
                             std::map<std::string, std::string> headers = {}) {
    const auto text = json.dump();
    return {bytes(text), status, std::move(headers), request.url};
}

inline Json mrProject(std::string id = "p1", std::string slug = "fixture") {
    return {{"id", id}, {"project_id", id}, {"slug", slug}, {"project_type", "mod"},
        {"title", "Fixture"}, {"description", "Summary"}, {"author", "Fixture author"},
        {"versions", Json::array({"1.20.1"})}, {"loaders", Json::array({"fabric"})},
        {"downloads", 100}, {"date_modified", "2026-01-01T00:00:00Z"}};
}

inline Json cfProject() {
    return {{"id", 42}, {"slug", "fixture"}, {"classId", 6}, {"gameId", 432},
        {"name", "Fixture"}, {"summary", "Summary"}, {"downloadCount", 100},
        {"authors", Json::array({Json{{"name", "Fixture author"}}})},
        {"dateModified", "2026-01-01T00:00:00Z"}, {"latestFilesIndexes", Json::array()}};
}

inline Json mrFile(std::string name = "fixture.jar", bool primary = true,
                   std::string sha1 = std::string(40, '1'), std::string sha512 = std::string(128, '2'),
                   std::uint64_t size = 3) {
    return {{"url", "https://cdn.modrinth.com/data/p1/versions/v1/" + name}, {"filename", name},
        {"primary", primary}, {"size", size}, {"hashes", {{"sha1", sha1}, {"sha512", sha512}}}};
}

inline Json mrVersion(std::string id = "v1", std::string project = "p1") {
    return {{"id", id}, {"project_id", project}, {"name", "Fixture version"}, {"version_number", "1.0"},
        {"loaders", Json::array({"fabric"})}, {"game_versions", Json::array({"1.20.1"})},
        {"date_published", "2026-01-01T00:00:00Z"}, {"version_type", "release"},
        {"files", Json::array({mrFile()})}, {"dependencies", Json::array()}};
}

inline Json cfVersion(std::uint64_t id = 101, std::string sha1 = std::string(40, '1')) {
    return {{"id", id}, {"modId", 42}, {"displayName", "Fixture version"}, {"fileName", "fixture.jar"},
        {"downloadUrl", "https://edge.forgecdn.net/files/1/1/fixture.jar"}, {"fileLength", 3},
        {"fileDate", "2026-01-01T00:00:00Z"}, {"releaseType", 1}, {"fileFingerprint", 0},
        {"gameVersions", Json::array({"1.20.1", "Fabric"})},
        {"hashes", Json::array({Json{{"algo", 1}, {"value", sha1}}})}, {"dependencies", Json::array()}};
}

inline InstanceContext context(const Path& root) {
    InstanceContext result;
    result.identity = {"test-root", "test-instance"};
    result.label = "Disposable instance";
    result.minecraftVersion = "1.20.1";
    result.loader = "fabric";
    result.resourceRoot = root;
    result.modsRoot = root / "mods";
    result.resourcepacksRoot = root / "resourcepacks";
    result.shaderpacksRoot = root / "shaderpacks";
    result.datapacksRoot = root / "datapacks";
    result.revision = "test-revision";
    return result;
}

inline ResourceServices authority(const InstanceContext& selected) {
    ResourceServices result;
    result.context = [selected](const InstanceIdentity& identity) {
        require(identity == selected.identity, "Fixture identity mismatch");
        return selected;
    };
    result.acquireMutation = [](const InstanceIdentity&, const std::string&) { return std::make_shared<int>(1); };
    return result;
}

} // namespace nickel::tests
