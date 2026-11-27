#pragma once

#include "suite_cases.hpp"

#include <string_view>

namespace nickel::production_smoke {

inline void graph_pin(std::string_view value, std::size_t length = 64) {
    check(value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "same-graph identity is not a full lowercase hash");
}

inline Json graph_records(const Json& document) {
    const auto& files = document.at("files");
    if (files.is_array()) return files;
    check(files.is_object(), "same-graph source inventory is not an array or object");
    Json rows = Json::array();
    for (const auto& [path, info] : files.items()) {
        rows.push_back({{"path", path}, {"size", info.at("bytes")}, {"sha256", info.at("sha256")}});
    }
    return rows;
}

template<class OwnedPath>
Json verified_same_graph_inputs(const fs::path& input, const std::string& sourcePin,
    const std::string& sourceCommit, const std::string& harnessPin,
    const fs::path& compiledHarnessRoot, const fs::path& self, OwnedPath owned) {
    graph_pin(sourcePin);
    graph_pin(sourceCommit, 40);
    graph_pin(harnessPin);
    const auto freeze = read_disk(input);
    check(freeze.at("kind") == "verified-same-graph-production-inputs", "new Pd requires same-graph evidence, not an old87 portal");
    check(freeze.at("sourceGenerationManifestSha256") == sourcePin && freeze.at("sourceCommit") == sourceCommit,
        "same-graph source differs from the compiled identities");
    check(freeze.at("harnessManifestSha256") == harnessPin, "same-graph harness differs from its compiled freeze");
    const auto bound = [&](const char* pathKey, const char* hashKey) {
        const auto path = owned(freeze.at(pathKey).template get<std::string>());
        const auto pin = freeze.at(hashKey).template get<std::string>();
        graph_pin(pin);
        check(file_hash(path) == pin, "same-graph bound input changed");
        return path;
    };
    const auto sourcePath = bound("sourceManifest", "sourceGenerationManifestSha256");
    const auto sources = read_disk(sourcePath);
    check(sources.at("sourceCommit") == sourceCommit, "raw production source manifest binds another commit");
    const auto harnessPath = bound("harnessManifest", "harnessManifestSha256");
    const auto harness = read_disk(harnessPath);
    check(harness.at("kind") == "owned-production-smoke-source-delta" && harness.at("owner") == "production-smoke",
        "raw harness publication is not this owner's immutable source delta");
    std::set<std::string> harnessNames;
    for (const auto& row : graph_records(harness)) {
        const auto name = row.at("path").template get<std::string>();
        const auto relative = path_of(name);
        check(!relative.empty() && !relative.is_absolute(), "harness source path escaped its freeze");
        for (const auto& part : relative) check(part != ".." && part != ".", "harness source path contains dot components");
        if (!name.starts_with("tests/native-production-smoke/")) continue;
        check(harnessNames.insert(name).second, "duplicate harness source inventory");
        const auto file = compiledHarnessRoot / relative;
        check(fs::is_regular_file(fs::symlink_status(file))
            && fs::file_size(file) == row.at("size").template get<std::uintmax_t>()
            && file_hash(file) == row.at("sha256").template get<std::string>(), "compiled harness source differs from its frozen bytes");
    }
    for (const auto* required : {"production_main.cpp", "suite_cases.hpp", "qt_probe.hpp", "same_graph_inputs.hpp", "CMakeLists.txt", "matrix.json"}) {
        check(harnessNames.contains(std::string("tests/native-production-smoke/") + required), "harness manifest omits a compiled matrix input");
    }
    const auto ledgerPath = bound("artifactManifest", "artifactManifestSha256");
    const auto completePath = bound("completeMarker", "completeMarkerSha256");
    const auto compilePath = bound("compileCommands", "compileCommandsSha256");
    const auto linkPath = bound("linkRecipe", "linkRecipeSha256");
    const auto inventoryPath = bound("actualCTestInventory", "actualCTestInventorySha256");
    const auto ledger = read_disk(ledgerPath);
    const auto complete = read_disk(completePath);
    check(ledger.at("kind") == "matched-same-graph-production-artifacts" && ledger.at("sourceCommit") == sourceCommit
        && ledger.at("sourceGenerationManifestSha256") == sourcePin && ledger.at("harnessManifestSha256") == harnessPin,
        "artifact ledger does not bind both production and acceptance source scopes");
    check(complete.at("publicationComplete") == true && complete.at("receiver") == "production-smoke"
        && complete.at("sourceCommit") == sourceCommit && complete.at("sourceGenerationManifestSha256") == sourcePin
        && complete.at("harnessManifestSha256") == harnessPin
        && complete.at("artifactManifestSha256") == freeze.at("artifactManifestSha256")
        && complete.at("compileCommandsSha256") == freeze.at("compileCommandsSha256")
        && complete.at("linkRecipeSha256") == freeze.at("linkRecipeSha256"), "build completion does not close the same graph");
    const auto commands = read_disk(compilePath);
    check(commands.is_array(), "actual compile commands are missing");
    bool compiled = false;
    for (const auto& command : commands) {
        const auto file = path_of(command.at("file").template get<std::string>());
        if (!same_path(qstr(path_text(file)), compiledHarnessRoot / "tests/native-production-smoke/production_main.cpp")) continue;
        const auto text = command.contains("command") ? command.at("command").template get<std::string>() : command.at("arguments").dump();
        check(text.find(sourcePin) != std::string::npos && text.find(sourceCommit) != std::string::npos
            && text.find(harnessPin) != std::string::npos, "actual compile command lacks the compiled generation pins");
        compiled = true;
    }
    check(compiled, "actual compile graph omits this acceptance consumer");
    const auto linkBytes = read_fixture(linkPath, 16 * 1024 * 1024);
    check(linkBytes.contains("nickel_production_smoke_existing_matrix"), "actual link recipe omits the existing matrix target");
    std::size_t libraries = 0;
    std::size_t productionBinaries = 0;
    std::size_t smokeBinaries = 0;
    std::set<std::string> artifacts;
    for (const auto& row : ledger.at("artifacts")) {
        const auto path = path_of(row.at("physicalPath").template get<std::string>());
        check(path.is_absolute() && artifacts.insert(QDir::fromNativeSeparators(qstr(path_text(path))).toCaseFolded().toStdString()).second,
            "duplicate or relative same-graph artifact reference");
        const auto digest = row.at("sha256").template get<std::string>();
        graph_pin(digest);
        check(fs::is_regular_file(path) && fs::file_size(path) == row.at("bytes").template get<std::uintmax_t>()
            && file_hash(path) == digest, "same-graph published artifact changed");
        const auto role = row.at("role").template get<std::string>();
        if (role == "production-library") {
            ++libraries;
            check(linkBytes.contains(QByteArray::fromStdString(path.filename().string())), "published production library is absent from the actual link recipe");
        } else if (role == "production-executable") {
            ++productionBinaries;
        } else if (role == "smoke-executable") {
            ++smokeBinaries;
            check(same_path(qstr(path_text(path)), self) && file_hash(self) == digest, "artifact ledger does not identify this running consumer");
        }
    }
    check(libraries != 0 && productionBinaries == 1 && smokeBinaries == 1, "same-graph production/consumer artifact closure is incomplete");
    const auto inventory = read_disk(inventoryPath);
    check(inventory.at("kind") == "ctestInfo", "actual same-graph CTest inventory is missing");
    bool present = false;
    const auto argv = QCoreApplication::arguments();
    for (const auto& test : inventory.at("tests")) {
        if (test.at("name") != "nickel_production_smoke_existing_matrix") continue;
        check(!present, "duplicate actual existing-matrix CTest entry");
        const auto& command = test.at("command");
        check(command.is_array() && command.size() == static_cast<std::size_t>(argv.size()), "actual CTest command arguments differ");
        for (qsizetype index = 0; index < argv.size(); ++index) {
            const auto value = qstr(command.at(static_cast<std::size_t>(index)).template get<std::string>());
            check(value == argv.at(index) || same_path(value, path_of(argv.at(index).toUtf8().toStdString())), "actual CTest command does not bind this run");
        }
        present = true;
    }
    check(present, "actual same-graph CTest inventory omits the 22-case consumer");
    check(file_hash(sourcePath) == sourcePin && file_hash(harnessPath) == harnessPin
        && file_hash(ledgerPath) == freeze.at("artifactManifestSha256").template get<std::string>(), "same-graph manifests changed during verification");
    return freeze;
}

} // namespace nickel::production_smoke
