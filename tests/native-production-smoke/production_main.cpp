#include "same_graph_inputs.hpp"
#include "fixture_inputs.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QStorageInfo>
#include <QVersionNumber>

#include <algorithm>
#include <iostream>
#include <map>
#include <string_view>

#ifndef NICKEL_SMOKE_SOURCE_SHA256
#error "The production source manifest must be pinned by the matched build graph"
#endif
#ifndef NICKEL_SMOKE_OWNER_ROOT
#error "The exclusive production-smoke worktree must be declared by CMake"
#endif

namespace {
using namespace nickel::production_smoke;
using nickel::Json;

fs::path owned_path(const std::string& text) {
    const auto owner = path_of(NICKEL_SMOKE_OWNER_ROOT).lexically_normal();
    const auto value = fs::absolute(path_of(text)).lexically_normal();
    const auto relative = value.lexically_relative(owner);
    check(!relative.empty() && !relative.is_absolute(), "input/output is outside the owner worktree");
    for (const auto& part : relative) {
        check(part != "..", "input/output escaped the owner worktree");
    }
    auto current = owner;
    for (const auto& part : relative) {
        current /= part;
        const QFileInfo info(qstr(path_text(current)));
        check(!info.isSymLink() && !info.isJunction(), "input/output traverses a link or junction");
    }
    return value;
}

void require_pin(std::string_view value) {
    check(value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "source pin is not a full lowercase SHA256");
}

Json verified_inputs(const fs::path& evidence, const std::string& pin) {
#ifdef NICKEL_SMOKE_SAME_GRAPH
    return verified_same_graph_inputs(evidence, pin, NICKEL_SMOKE_SOURCE_COMMIT,
        NICKEL_SMOKE_HARNESS_SHA256, path_of(NICKEL_SMOKE_HARNESS_ROOT),
        path_of(QCoreApplication::applicationFilePath().toUtf8().toStdString()), owned_path);
#else
    const auto freeze = read_disk(evidence);
    check(freeze.at("kind") == "verified-built-generation-inputs", "verified built-generation evidence is required");
    check(freeze.at("sourceGenerationManifestSha256") == pin, "runtime source generation differs from the compiled pin");
    const auto root = owned_path(freeze.at("root").get<std::string>());
    const auto portal_path = owned_path(freeze.at("portalManifest").get<std::string>());
    const auto portal_pin = freeze.at("portalManifestSha256").get<std::string>();
    require_pin(portal_pin);
    check(portal_pin == NICKEL_SMOKE_PORTAL_SHA256, "runtime portal differs from the compiled link portal");
    check(file_hash(portal_path) == portal_pin, "raw historical portal changed after linking");
    const auto portal = read_disk(portal_path);
    check(portal.at("sourceGenerationManifestSha256") == pin && portal.at("member") == "production-smoke",
          "portal is not the matched source generation for this receiver");
    const auto source_manifest = owned_path(freeze.at("sourceManifest").get<std::string>());
    check(file_hash(source_manifest) == pin, "raw source-generation manifest changed");
    const auto sources = read_disk(source_manifest);
    const auto builder_manifest = root / "bound-inputs/.snow/build-resume-owned-sources-20261121T1429Z.json";
    check(file_hash(builder_manifest) == portal.at("boundInputs").at(".snow/build-resume-owned-sources-20261121T1429Z.json").at("sha256").get<std::string>(),
          "bound builder-source checkpoint changed");
    const auto builder_sources = read_disk(builder_manifest);
    const auto marker = owned_path(freeze.at("completeMarker").get<std::string>());
    check(file_hash(marker) == freeze.at("completeMarkerSha256").get<std::string>(), "completion marker changed");
    const auto completed = read_disk(marker);
    check(completed.at("publicationComplete") == true && completed.at("receiver") == "production-smoke"
          && completed.at("receiverManifestSha256") == portal_pin
          && completed.at("sourceGenerationManifestSha256") == pin, "completion does not bind both distinct manifest scopes");
    check(freeze.at("files").is_array() && !freeze.at("files").empty(), "verified source list is empty");
    std::set<std::string> headers;
    for (const auto& row : freeze.at("files")) {
        const auto text = row.at("path").get<std::string>();
        check(!text.empty() && text.find_first_of("\\:") == std::string::npos, "noncanonical manifest path");
        const auto relative = path_of(text);
        check(!relative.is_absolute(), "absolute path in manifest");
        for (const auto& part : relative) check(part != ".." && part != ".", "dot path in manifest");
        const auto source = owned_path(path_text(root / relative));
        check(fs::is_regular_file(source) && fs::file_size(source) == row.at("size").get<std::uintmax_t>(),
              "frozen input file/size changed");
        check(file_hash(source) == row.at("sha256").get<std::string>(), "frozen input hash changed");
        if (!portal.at("files").contains(text)) continue;
        check(headers.insert(text).second, "duplicate frozen header record");
        check(portal.at("files").at(text).at("sha256") == row.at("sha256"), "header differs from the bound link portal");
        if (sources.at("files").contains(text)) {
            check(sources.at("files").at(text).at("sha256") == row.at("sha256"), "header differs from the source-generation manifest");
        } else {
            bool present = false;
            for (const auto& captured : builder_sources.at("files")) {
                if (captured.at("path") != text) continue;
                check(captured.at("sha256") == row.at("sha256"), "builder header differs from its bound checkpoint");
                present = true;
            }
            check(present, "header is absent from both bound source scopes");
        }
    }
    check(headers.size() == portal.at("files").size() && headers.size() == completed.at("headerFiles").get<std::size_t>(),
          "actual header inventory differs from portal completion");
    const auto minimum_path = owned_path(freeze.at("minimumInputs").get<std::string>());
    check(file_hash(minimum_path) == freeze.at("minimumInputsSha256").get<std::string>(), "received link/common inputs changed");
    const auto minimum = read_disk(minimum_path);
    check(minimum.at("sourceGenerationManifestSha256") == pin && minimum.at("portalManifestSha256") == portal_pin,
          "received inputs are not from the linked generation");
    for (const auto& row : minimum.at("files")) {
        const auto source = owned_path(path_text(minimum_path.parent_path() / path_of(row.at("path").get<std::string>())));
        check(fs::file_size(source) == row.at("size").get<std::uintmax_t>() && file_hash(source) == row.at("sha256").get<std::string>(),
              "received old-generation link/common input changed");
    }
    std::size_t libraries = 0;
    bool production_binary = false;
    for (const auto& row : portal.at("artifacts")) {
        const auto logical = path_of(row.at("physicalPath").get<std::string>());
        const auto extension = logical.extension().string();
        if (extension != ".lib" && extension != ".exe") continue;
        check(fs::is_regular_file(logical) && fs::file_size(logical) == row.at("bytes").get<std::uintmax_t>(),
              "relocated production artifact size changed");
        check(file_hash(logical) == row.at("sha256").get<std::string>(), "relocated logical production artifact hash changed");
        if (extension == ".lib") ++libraries;
        if (extension == ".exe") {
            production_binary = true;
            check(row.at("sha256") == completed.at("artifactSha256"), "completion binds a different production executable");
        }
    }
    check(libraries != 0 && production_binary, "no matched production binary/library closure");
    const auto inventory_path = owned_path(freeze.at("actualCTestInventory").get<std::string>());
    const auto inventory = read_disk(inventory_path);
    check(inventory.at("kind") == "ctestInfo" && inventory.at("tests").is_array(), "actual CTest JSON inventory is required");
    bool matrix_present = false;
    for (const auto& test : inventory.at("tests")) {
        if (test.at("name") != "nickel_production_smoke_existing_matrix") continue;
        const auto& command = test.at("command");
        check(command.is_array() && !command.empty(), "CTest matrix has no real command");
        const auto executable = owned_path(command.at(0).get<std::string>());
        check(file_hash(executable) == file_hash(owned_path(QCoreApplication::applicationFilePath().toUtf8().toStdString())),
              "CTest command does not identify this compiled matrix executable");
        matrix_present = true;
    }
    check(matrix_present, "the actual matched CTest graph omits the existing matrix");
    check(file_hash(portal_path) == portal_pin && file_hash(source_manifest) == pin, "raw manifests changed during verification");
    return freeze;
#endif
}

void write_new(const fs::path& output, const Json& report) {
    check(!fs::exists(output), "case evidence output already exists");
    fs::create_directories(output.parent_path());
    const auto payload = report.dump(2) + "\n";
    check(payload.size() <= 8 * 1024 * 1024, "case report exceeded the evidence quota");
    QFile file(qstr(path_text(output)));
    check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "cannot exclusively create case evidence");
    check(file.write(payload.data(), static_cast<qint64>(payload.size())) == static_cast<qint64>(payload.size()),
          "case evidence write was incomplete");
    check(file.flush(), "case evidence flush failed");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication application(argc, argv);
        application.setQuitOnLastWindowClosed(false);
        check(QLibraryInfo::version() == QVersionNumber(6, 8, 3), "runtime Qt is not exactly 6.8.3");
        std::map<std::string, std::string> arguments;
        const auto values = QCoreApplication::arguments();
        check(values.size() == 11, "required arguments: --freeze --fixtures --matrix --manifest-sha256 --output");
        for (qsizetype index = 1; index < values.size(); index += 2) {
            const auto key = values.at(index).toUtf8().toStdString();
            check(key == "--freeze" || key == "--fixtures" || key == "--matrix"
                  || key == "--manifest-sha256" || key == "--output", "unknown production smoke argument");
            check(arguments.emplace(key, values.at(index + 1).toUtf8().toStdString()).second,
                  "duplicate production smoke argument");
        }
        const auto pin = arguments.at("--manifest-sha256");
        require_pin(pin);
        check(pin == NICKEL_SMOKE_SOURCE_SHA256, "caller source label differs from the compiled source generation");
        const auto freeze_path = owned_path(arguments.at("--freeze"));
        const auto output = owned_path(arguments.at("--output"));
        check(!fs::exists(output), "case evidence output already exists");
        const QStorageInfo worktree(qstr(NICKEL_SMOKE_OWNER_ROOT));
        check(worktree.isValid() && worktree.bytesAvailable() >= 288LL * 1024 * 1024,
              "owner volume is below the 256 MiB reserve plus fixture-copy budget");
#ifdef _WIN32
        const QStorageInfo system_volume(QStringLiteral("C:/"));
        check(system_volume.isValid() && system_volume.bytesAvailable() >= 1024LL * 1024 * 1024,
              "C volume is below the 1 GiB execution reserve");
#endif
        const auto self = path_of(QCoreApplication::applicationFilePath().toUtf8().toStdString());
        const auto binary_hash = file_hash(self);
        const auto freeze = verified_inputs(freeze_path, pin);
        const auto fixture_path = owned_path(arguments.at("--fixtures"));
        const auto matrix_path = path_of(arguments.at("--matrix"));
#ifdef NICKEL_SMOKE_SAME_GRAPH
        check(same_path(qstr(path_text(matrix_path)), path_of(NICKEL_SMOKE_HARNESS_ROOT) / "tests/native-production-smoke/matrix.json"),
            "same-graph caller chose an unbound matrix");
        RuntimeFixtures runtime_fixtures(fixture_path, output, freeze, owned_path);
        const auto fixtures = runtime_fixtures.document();
#else
        owned_path(path_text(matrix_path));
        const auto fixtures = read_disk(fixture_path);
#endif
        const auto matrix = read_disk(matrix_path);
        std::size_t constructed = 0;
        std::map<std::string, std::size_t> scenario_counts;
        for (const auto& fixture : fixtures.at("scenarios")) {
            check(scenario_counts.emplace(fixture.at("name").get<std::string>(), 0).second, "duplicate fixture scenario name");
        }
        Factory factory = [&constructed, &scenario_counts](const Json& fixture) {
            const auto name = fixture.at("name").get<std::string>();
            check(scenario_counts.contains(name), "AppServices received an undeclared fixture scenario");
            nickel::app::AppServicesOptions options;
            options.executable = owned_path(fixture.at("executable").get<std::string>());
            options.paths = {options.executable, owned_path(fixture.at("systemConfigBase").get<std::string>()),
                             owned_path(fixture.at("appDataBase").get<std::string>()), fixture.at("macos").get<bool>()};
            options.startup_arguments = fixture.value("startupArguments", std::vector<std::string>{});
            auto services = std::make_unique<nickel::app::AppServices>(std::move(options));
            ++constructed;
            ++scenario_counts.at(name);
            return services;
        };
        Suite suite(std::move(factory), fixtures, matrix);
        suite.run();
        auto report = suite.report(pin, binary_hash);
        report["compiledSourceManifestSha256"] = NICKEL_SMOKE_SOURCE_SHA256;
#ifdef NICKEL_SMOKE_SAME_GRAPH
        runtime_fixtures.verify_template();
        report["fixtureBinding"] = runtime_fixtures.binding();
        report["fixtureTemplateUnchangedAfterRun"] = true;
        report["sourceCommit"] = NICKEL_SMOKE_SOURCE_COMMIT;
        report["harnessManifestSha256"] = NICKEL_SMOKE_HARNESS_SHA256;
        report["artifactManifestSha256"] = freeze.at("artifactManifestSha256");
        report["generationBinding"] = "same-graph-pd-production-and-owned-consumer";
#else
        report["compiledPortalManifestSha256"] = NICKEL_SMOKE_PORTAL_SHA256;
        report["portalManifestSha256"] = freeze.at("portalManifestSha256");
        report["generationBinding"] = "historical-generation87-portal";
#endif
        report["binaryPath"] = path_text(self);
        report["verifiedFreezeSha256"] = file_hash(freeze_path);
        report["fixtureManifestSha256"] = file_hash(fixture_path);
        report["matrixSha256"] = file_hash(matrix_path);
        report["sourceFiles"] = freeze.at("files");
        report["artifactReferences"] = freeze.at("artifactReferences");
        report["actualCTestInventorySha256"] = file_hash(owned_path(freeze.at("actualCTestInventory").get<std::string>()));
        report["qtVersion"] = QLibraryInfo::version().toString().toStdString();
        report["appServicesConstructed"] = constructed;
        report["scenarioAppServicesConstructed"] = scenario_counts;
        const bool all_scenarios = !scenario_counts.empty() && std::all_of(scenario_counts.begin(), scenario_counts.end(),
            [](const auto& row) { return row.second != 0; });
        report["allDeclaredScenariosObserved"] = all_scenarios;
        const int result = suite.has_failure() ? 1 : suite.full_pass() && all_scenarios ? 0 : 2;
        report["exitCode"] = result;
        report["verdict"] = result == 0 ? "PASS" : result == 1 ? "FAIL" : "BLOCKED";
        check(file_hash(self) == binary_hash, "running smoke binary changed during execution");
        write_new(output, report);
        for (const auto& row : report.at("cases")) {
            std::cout << row.at("id").get<std::string>() << ": " << row.at("verdict").get<std::string>()
                      << " " << row.at("reason").get<std::string>() << '\n';
        }
        return result;
    } catch (const std::exception& error) {
        std::cerr << "BLOCKED: production smoke input or harness error: " << error.what() << '\n';
        return 2;
    }
}
