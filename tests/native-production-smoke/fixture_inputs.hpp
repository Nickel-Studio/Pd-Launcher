#pragma once

#include "suite_cases.hpp"

#include <QFileInfo>
#include <QUuid>

#include <cstdint>
#include <map>
#include <set>
#include <string_view>

namespace nickel::production_smoke {

inline std::string fixture_relative_text(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline fs::path fixture_relative(const std::string& text) {
    check(!text.empty() && text.find_first_of("\\:") == std::string::npos,
        "fixture inventory path is not canonical relative text");
    check(std::none_of(text.begin(), text.end(), [](unsigned char value) { return value < 32; }),
        "fixture inventory path contains a control character");
    const auto relative = path_of(text);
    check(!relative.is_absolute() && fixture_relative_text(relative) == text, "fixture inventory path escaped");
    for (const auto& part : relative) check(part != ".." && part != "." && !part.empty(), "fixture inventory has a dot path");
    check(text.front() != '/' && text.back() != '/' && text.find("//") == std::string::npos,
        "fixture inventory path is not canonical");
    return relative;
}

inline bool fixture_inside(const fs::path& value, const fs::path& root) {
    const auto relative = value.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty() || relative.is_absolute()) return false;
    return std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; });
}

inline std::string fixture_digest(const QByteArray& payload) {
    return QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex().toStdString();
}

inline void fixture_hash_pin(const std::string& value, std::size_t length = 64) {
    check(value.size() == length && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }), "fixture identity is not a full lowercase hash");
}

inline std::string fixture_relocated_text(const std::string& text,
    const std::map<std::string, std::string>& paths, const std::string& sourceRoot) {
    if (const auto found = paths.find(text); found != paths.end()) return found->second;
    check(text != sourceRoot && !text.starts_with(sourceRoot + "/") && !text.starts_with(sourceRoot + "\\"),
        "fixture JSON contains an unregistered template path");
    return text;
}

inline Json fixture_relocated_json(const Json& value, const std::map<std::string, std::string>& paths,
    const std::string& sourceRoot, unsigned depth = 0) {
    check(depth <= 32, "fixture JSON relocation exceeded its depth bound");
    if (value.is_string()) return fixture_relocated_text(value.get<std::string>(), paths, sourceRoot);
    if (value.is_array()) {
        auto result = Json::array();
        for (const auto& item : value) result.push_back(fixture_relocated_json(item, paths, sourceRoot, depth + 1));
        return result;
    }
    if (value.is_object()) {
        auto result = Json::object();
        for (const auto& [key, item] : value.items()) {
            const auto target = fixture_relocated_text(key, paths, sourceRoot);
            check(!result.contains(target), "fixture JSON relocation collides with an existing key");
            result[target] = fixture_relocated_json(item, paths, sourceRoot, depth + 1);
        }
        return result;
    }
    // Preserve JSON integer types instead of passing durable data through QJson doubles.
    return value;
}

inline void fixture_write_new(const fs::path& path, const QByteArray& payload) {
    check(payload.size() <= 8 * 1024 * 1024, "runtime fixture file exceeded its byte bound");
    fs::create_directories(path.parent_path());
    QFile file(qstr(path_text(path)));
    check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "runtime fixture file is not fresh");
    check(file.write(payload) == payload.size() && file.flush(), "runtime fixture write was incomplete");
}

inline QByteArray fixture_json_bytes(const Json& value) {
    return QByteArray::fromStdString(value.dump(2, ' ', true) + "\n");
}

template<class OwnedPath>
class RuntimeFixtures final {
public:
    RuntimeFixtures(const fs::path& manifest, const fs::path& caseOutput, const Json& freeze, OwnedPath owned)
        : manifest_(manifest), template_(read_disk(manifest)), owned_(std::move(owned)) {
        manifestPin_ = freeze.at("fixtureManifestSha256").template get<std::string>();
        fixture_hash_pin(manifestPin_);
        check(same_path(qstr(path_text(manifest_)), path_of(freeze.at("fixtureManifest").template get<std::string>())),
            "caller fixture manifest differs from the prepared template");
        check(template_.at("fixtureRole") == "immutable-template", "new smoke requires an immutable fixture template");
        for (const auto* key : {"sourceCommit", "sourceGenerationManifestSha256", "harnessManifestSha256"}) {
            check(template_.at(key) == freeze.at(key), "fixture template belongs to another production/harness generation");
        }
        sourceRoot_ = owned_(template_.at("root").template get<std::string>());
        check(path_text(sourceRoot_).size() >= 310 && manifest_ == sourceRoot_ / "fixture-manifest.json",
            "fixture template root or long path differs");
        verify_template();
        const auto envelope = owned_(path_text(caseOutput.parent_path() / (path_text(caseOutput.filename()) + ".fixtures")));
        check(!fs::exists(caseOutput) && !fs::exists(envelope), "case output or runtime fixture namespace was already consumed");
        runRoot_ = envelope;
        while (path_text(runRoot_).size() < 310) runRoot_ /= "deep-run-path-preserved-0123456789";
        runRoot_ = owned_(path_text(runRoot_));
        check(!fixture_inside(runRoot_, sourceRoot_) && !fixture_inside(sourceRoot_, runRoot_),
            "runtime fixtures collide with their immutable template");
        const auto expectedRoot = path_of(freeze.at("fixtureRunRoot").template get<std::string>());
        check(same_path(qstr(path_text(runRoot_)), expectedRoot), "runtime fixture root differs from the prepared output binding");
        std::map<std::string, std::string> paths{{path_text(sourceRoot_), path_text(runRoot_)}};
        for (const auto& row : template_.at("pathTable")) {
            const auto value = row.at("value").template get<std::string>();
            const auto relative = fixture_relative(row.at("relative").template get<std::string>());
            check(paths.emplace(value, path_text(runRoot_ / relative)).second, "duplicate fixture path relocation");
        }
        document_ = template_;
        document_["root"] = path_text(runRoot_);
        document_["rootCharacters"] = path_text(runRoot_).size();
        document_["fixtureRole"] = "runtime-instance";
        document_["templateManifest"] = path_text(manifest_);
        document_["templateManifestSha256"] = manifestPin_;
        document_["templateToken"] = template_.at("token");
        document_["token"] = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').toStdString();
        document_["scenarios"] = fixture_relocated_json(template_.at("scenarios"), paths, path_text(sourceRoot_));
        document_["files"] = Json::array();
        document_["pathTable"] = Json::array();
        for (const auto& row : template_.at("pathTable")) {
            const auto value = row.at("value").template get<std::string>();
            document_["pathTable"].push_back({{"value", paths.at(value)}, {"relative", row.at("relative")}});
        }
        check(fs::create_directory(envelope), "runtime fixture envelope creation was not exclusive");
        fs::create_directories(runRoot_);
        for (const auto& directory : template_.at("directories")) {
            fs::create_directories(owned_(path_text(runRoot_ / fixture_relative(directory.template get<std::string>()))));
        }
        for (const auto& row : template_.at("files")) {
            const auto name = row.at("path").template get<std::string>();
            const auto source = owned_(path_text(sourceRoot_ / fixture_relative(name)));
            const auto payload = read_fixture(source);
            check(payload.size() >= 0 && static_cast<std::uintmax_t>(payload.size()) == row.at("size").template get<std::uintmax_t>()
                && fixture_digest(payload) == row.at("sha256").template get<std::string>(), "fixture template changed while cloning");
            auto bytes = payload;
            if (row.at("format") == "json") {
                auto value = Json::parse(payload.constData(), payload.constData() + payload.size());
                value = fixture_relocated_json(value, paths, path_text(sourceRoot_));
                if (name == "fixture-owner.json") {
                    value["token"] = document_.at("token");
                    value["fixtureRole"] = "runtime-instance";
                    value["templateToken"] = template_.at("token");
                }
                bytes = fixture_json_bytes(value);
            }
            const auto target = owned_(path_text(runRoot_ / fixture_relative(name)));
            fixture_write_new(target, bytes);
            const auto pin = fixture_digest(bytes);
            check(file_hash(target) == pin, "runtime fixture clone hash differs");
            document_["files"].push_back({{"path", name}, {"size", bytes.size()}, {"sha256", pin},
                {"format", row.at("format")}, {"templateSha256", row.at("sha256")}});
        }
        // Relocation changes a path-bearing JSON file's hash, including foreign proof JSON.
        for (auto& scenario : document_["scenarios"]) {
            for (auto& proof : scenario.at("foreignRootProofs")) {
                proof["sha256"] = file_hash(owned_(proof.at("path").template get<std::string>()));
            }
        }
        runtimeManifest_ = owned_(path_text(runRoot_ / "fixture-manifest.json"));
        fixture_write_new(runtimeManifest_, fixture_json_bytes(document_));
        runtimePin_ = file_hash(runtimeManifest_);
        verify_template();
    }

    [[nodiscard]] const Json& document() const { return document_; }

    [[nodiscard]] Json binding() const {
        return {{"templateManifest", path_text(manifest_)}, {"templateManifestSha256", manifestPin_},
            {"templateRoot", path_text(sourceRoot_)}, {"runtimeManifest", path_text(runtimeManifest_)},
            {"runtimeManifestSha256", runtimePin_}, {"runtimeRoot", path_text(runRoot_)},
            {"templatePassedToAppServices", false}, {"fixtureCopyMechanism", "byte-copy-with-registered-JSON-path-relocation"}};
    }

    void verify_template() const {
        check(file_hash(manifest_) == manifestPin_, "fixture template manifest changed");
        check(template_.at("kind") == "synthetic-production-fixtures" && template_.at("owner") == "production-smoke",
            "fixture template kind or owner differs");
        const auto marker = read_disk(owned_(path_text(sourceRoot_ / "fixture-owner.json")));
        fixture_hash_pin(template_.at("token").template get<std::string>(), 32);
        fixture_hash_pin(template_.at("generatorSha256").template get<std::string>());
        check(marker.at("owner") == "production-smoke" && marker.at("token") == template_.at("token")
            && marker.at("fixtureRole") == "immutable-template", "fixture template owner marker differs");
        const auto& files = template_.at("files");
        const auto& directories = template_.at("directories");
        check(files.is_array() && !files.empty() && files.size() <= 4096 && directories.is_array() && directories.size() <= 8192,
            "fixture template closure is missing or exceeds its bound");
        std::set<std::string> expectedFiles{"fixture-manifest.json"}, expectedDirs, foldedFiles, foldedDirs;
        std::uintmax_t bytes = 0;
        for (const auto& row : files) {
            const auto name = row.at("path").template get<std::string>();
            const auto relative = fixture_relative(name);
            check(expectedFiles.insert(name).second && foldedFiles.insert(qstr(name).toCaseFolded().toStdString()).second,
                "fixture template file inventory collides");
            check(row.at("size").is_number_integer() && row.at("size") >= 0, "fixture template size is not a bounded integer");
            const auto size = row.at("size").template get<std::uintmax_t>();
            check(size <= 8 * 1024 * 1024 && bytes <= 32 * 1024 * 1024 - size, "fixture template exceeds its byte bound");
            bytes += size;
            const auto path = owned_(path_text(sourceRoot_ / relative));
            const auto pin = row.at("sha256").template get<std::string>();
            fixture_hash_pin(pin);
            check(fs::is_regular_file(fs::symlink_status(path)) && fs::file_size(path) == size && file_hash(path) == pin,
                "fixture template file size/hash changed");
            check(row.at("format") == "json" || row.at("format") == "bytes", "fixture template format is not explicit");
        }
        check(expectedFiles.contains("fixture-owner.json"), "fixture template omits its owner marker");
        for (const auto& directory : directories) {
            const auto name = directory.template get<std::string>();
            fixture_relative(name);
            check(expectedDirs.insert(name).second && foldedDirs.insert(qstr(name).toCaseFolded().toStdString()).second,
                "fixture template directory inventory collides");
        }
        std::set<std::string> actualFiles, actualDirs;
        for (const auto& entry : fs::recursive_directory_iterator(sourceRoot_)) {
            const auto path = owned_(path_text(entry.path()));
            const QFileInfo info(qstr(path_text(path)));
            check(!info.isSymLink() && !info.isJunction(), "fixture template contains a link or junction");
            const auto name = path_text(path.lexically_relative(sourceRoot_));
            if (entry.is_regular_file()) actualFiles.insert(name);
            else {
                check(entry.is_directory(), "fixture template contains a special file");
                actualDirs.insert(name);
            }
        }
        check(actualFiles == expectedFiles && actualDirs == expectedDirs, "fixture template has an unlisted file or directory");
        const auto& table = template_.at("pathTable");
        check(table.is_array() && !table.empty() && table.size() <= 4096, "fixture template path table is missing");
        std::map<std::string, std::string> paths{{path_text(sourceRoot_), path_text(sourceRoot_)}};
        for (const auto& row : table) {
            const auto value = row.at("value").template get<std::string>();
            const auto relative = fixture_relative(row.at("relative").template get<std::string>());
            check(value == path_text(sourceRoot_ / relative) && paths.emplace(value, value).second,
                "fixture template path table escaped or repeats a path");
        }
        fixture_relocated_json(template_.at("scenarios"), paths, path_text(sourceRoot_));
        for (const auto& row : files) {
            if (row.at("format") == "json") {
                fixture_relocated_json(read_disk(owned_(path_text(sourceRoot_ / fixture_relative(row.at("path").template get<std::string>())))),
                    paths, path_text(sourceRoot_));
            }
        }
        check(file_hash(manifest_) == manifestPin_, "fixture template changed during verification");
        if (!runtimeManifest_.empty()) check(file_hash(runtimeManifest_) == runtimePin_, "runtime fixture manifest changed during execution");
    }

private:
    fs::path manifest_, sourceRoot_, runRoot_, runtimeManifest_;
    Json template_, document_;
    OwnedPath owned_;
    std::string manifestPin_, runtimePin_;
};

} // namespace nickel::production_smoke
