// Reuse the frozen independent fixture and production DTO/pack oracles.
// The original translation unit is not separately compiled into this target.
#define main pd_trust_negatives_previous_main
#include "../../native/tests/trusted_export/trusted_export_tests.cpp"
#undef main

#include "app/service/trusted_launcher.hpp"

#include <type_traits>

namespace {

static_assert(!std::is_default_constructible_v<nickel::app::TrustedLauncherReceipt>);
static_assert(!std::is_copy_constructible_v<nickel::app::TrustedLauncherReceipt>);
static_assert(!std::is_constructible_v<nickel::app::TrustedLauncherReceipt, fs::path>);

Json public_denial(Fixture& fixture) {
    const auto destination = fixture.save("must-not-publish.zip");
    const auto before = fixture.state->tasks->snapshots().size();
    const auto reply = fixture.action(UiAction::ExportPack, fixture.export_args(destination, true));
    check(reply.error.isError(), "Public ExportPack minted trust from an executable option");
    const auto code = reply.error.code.toStdString();
    check(code == nickel::app::public_error("missing_service").code.toStdString()
          || code == nickel::app::public_error("integrity").code.toStdString()
          || code == nickel::app::public_error("untrusted_launcher").code.toStdString()
          || code == nickel::app::public_error("unsupported").code.toStdString(),
          "Public denial did not reach the trusted launcher capability boundary");
    check(fixture.state->tasks->snapshots().size() == before, "Untrusted export started a canonical Task");
    check(!fs::exists(safe::io_path(destination)), "Untrusted export published a destination");
    return {{"publicDenial", code}, {"includeLauncher", true}, {"actualAppServices", true},
            {"productionReceiptUsed", false}, {"destinationExists", false},
            {"deeperImageOrNoticeGateExercised", false}, {"fixture", nickel::path_to_utf8(fixture.root)}};
}

Json test_image_cannot_mint() {
    const auto image = safe::checked_absolute(nickel::current_executable());
    const auto before = safe::capture_regular_file(image.parent_path(), image, 256ULL * 1024 * 1024);
    const auto receipt = nickel::app::capture_trusted_launcher();
    check(!receipt, "An unstamped test executable minted a genuine Palladium receipt");
    const auto after = safe::capture_regular_file(image.parent_path(), image, 256ULL * 1024 * 1024);
    check(before.file_identity == after.file_identity && before.revision == after.revision
          && before.size == after.size && before.sha256 == after.sha256,
          "Actual OS process image changed during capture");
    return {{"actualOsImage", nickel::path_to_utf8(image)}, {"sha256", after.sha256},
            {"bytes", after.size}, {"fileIdentity", {{"volume", after.file_identity.volume},
                                                       {"index", after.file_identity.index}}},
            {"productionReceiptMinted", false}, {"scope", "actual-factory-test-image-denial"}};
}

Json factory_cancelled() {
    Cancellation cancellation;
    cancellation.cancel();
    const auto code = denied([&] { (void)nickel::app::capture_trusted_launcher(cancellation); });
    check(code == "cancelled", "Factory ignored a pre-cancelled capability capture");
    return {{"rawDenial", code}, {"productionReceiptMinted", false}, {"cancelledBeforeCapture", true}};
}

Json identity_replacement(const fs::path& base) {
    Fixture fixture(base, fake_pe());
    const auto before = safe::capture_regular_file(fixture.root, fixture.executable);
    safe::rename_no_replace(fixture.executable, fixture.executable.parent_path() / "original.exe");
    write_new(fixture.executable, fake_pe());
    const auto after = safe::capture_regular_file(fixture.root, fixture.executable);
    check(before.file_identity != after.file_identity && before.sha256 == after.sha256
          && before.size == after.size, "Same-byte file identity replacement was not real");
    auto evidence = public_denial(fixture);
    evidence["beforeSha256"] = before.sha256;
    evidence["afterSha256"] = after.sha256;
    evidence["fileIdentityChanged"] = true;
    return evidence;
}

Json same_size_mutation(const fs::path& base) {
    Fixture fixture(base, fake_pe());
    const auto before = safe::capture_regular_file(fixture.root, fixture.executable);
    auto file = safe::SecureFile::open_owned(fixture.executable, before.file_identity);
    file.seek(700);
    const std::array<std::byte, 1> mutation{std::byte{0x5a}};
    file.write(mutation);
    file.flush();
    file.close();
    const auto after = safe::capture_regular_file(fixture.root, fixture.executable);
    check(before.file_identity == after.file_identity && before.size == after.size
          && before.sha256 != after.sha256, "Same-size actual file hash mutation was not real");
    auto evidence = public_denial(fixture);
    evidence["beforeSha256"] = before.sha256;
    evidence["afterSha256"] = after.sha256;
    evidence["sameFileIdentity"] = true;
    return evidence;
}

Json public_cancel_before_work(const fs::path& base) {
    Fixture fixture(base);
    const auto destination = fixture.save("cancel-before-work.zip");
    auto args = fixture.export_args(destination, true);
    const auto operation_id = std::string("trust-negative-") + safe::unique_id();
    args["operationId"] = operation_id;
    constexpr nickel::ui::RequestId export_id = 900001;
    QObject receiver;
    std::optional<UiReply> export_reply;
    QObject::connect(fixture.services.get(), &nickel::ui::UiServices::commandFinished, &receiver,
        [&](const UiReply& reply) { if (reply.id == export_id) export_reply = reply; });
    // submit registers the operation synchronously; no Qt event pump intervenes.
    fixture.services->submit(export_id, UiAction::ExportPack, nickel::app::to_public_json(args).toObject());
    const auto cancel_reply = fixture.action(UiAction::CancelDownload, {{"id", operation_id}});
    check(!cancel_reply.error.isError(), "Public CancelDownload did not accept the registered export");
    wait_for([&] { return export_reply.has_value(); });
    check(export_reply->error.code == nickel::app::public_error("cancelled").code,
          "Cancelled public export did not return its canonical cancellation denial");
    check(fixture.state->tasks->snapshots().empty(), "Pre-work cancellation started an export Task");
    check(!fs::exists(safe::io_path(destination)), "Cancelled launcher export published a destination");
    return {{"includeLauncher", true}, {"actualAppServices", true}, {"publicCancelAccepted", true},
            {"publicDenial", export_reply->error.code.toStdString()}, {"destinationExists", false},
            {"productionReceiptUsed", false}, {"scope", "public-pre-work-cancellation"}};
}

Json not_run(std::string_view id, std::string_view reason) {
    return {{"id", id}, {"status", "NOT_RUN"}, {"executed", false}, {"assertions", 0}, {"reason", reason}};
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        const auto arguments = QCoreApplication::arguments();
        const auto output = safe::checked_absolute(safe::utf8_path(option(arguments, "--output")));
        const auto base = safe::checked_absolute(safe::utf8_path(option(arguments, "--fixture-root")));
        const auto graph_sha = option(arguments, "--graph-sha256");
        child_of(output.parent_path(), base);
        check(!fs::exists(safe::io_path(output)) && !fs::exists(safe::io_path(base)), "Owned output/fixtures already exist");
        check(graph_sha.size() == 64 && std::all_of(graph_sha.begin(), graph_sha.end(), [](char c) {
            return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'); }), "Graph SHA256 is not a full digest");
        for (const auto* name : {"APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "TEMP", "TMP"}) {
            const auto value = qgetenv(name);
            check(!value.isEmpty(), "Runner did not isolate the process environment");
            child_of(output.parent_path(), safe::checked_absolute(safe::utf8_path(value.toStdString())));
        }
        capacity();
        safe::create_directory_new(base);
        Json cases = Json::array();
        const auto add = [&](std::string_view id, const std::function<Json()>& work) { cases.push_back(run_case(id, work)); };
        add("factory-test-exe-cannot-mint", test_image_cannot_mint);
        add("factory-pre-cancelled", factory_cancelled);
        add("public-options-text-no-trust", [&] { Fixture fixture(base); return public_denial(fixture); });
        add("public-options-missing-no-trust", [&] { Fixture fixture(base, {}, "missing"); return public_denial(fixture); });
        add("public-options-directory-no-trust", [&] { Fixture fixture(base, {}, "directory"); return public_denial(fixture); });
        add("public-options-wrong-os-no-trust", [&] { Fixture fixture(base, std::string("\x7f" "ELF") + std::string(1020, '\0')); return public_denial(fixture); });
        add("public-options-arm64-no-trust", [&] { Fixture fixture(base, fake_pe(0xaa64)); return public_denial(fixture); });
        add("public-options-pe32-no-trust", [&] { Fixture fixture(base, fake_pe(0x14c)); return public_denial(fixture); });
        add("public-options-dll-no-trust", [&] { Fixture fixture(base, fake_pe(0x8664, true)); return public_denial(fixture); });
        add("public-options-no-versioninfo-no-trust", [&] { Fixture fixture(base, fake_pe()); return public_denial(fixture); });
        add("public-options-identity-replacement-no-trust", [&] { return identity_replacement(base); });
        add("public-options-same-size-hash-mutation-no-trust", [&] { return same_size_mutation(base); });
        add("public-cancel-before-work", [&] { return public_cancel_before_work(base); });
        for (const auto* id : {"factory-no-header-branch", "factory-material-ready-false-branch",
                "actual-pd-path-identity-replacement", "actual-pd-arch-type-version-denial",
                "actual-pd-notice-missing", "actual-pd-notice-hash-mismatch", "actual-pd-notice-set-mismatch",
                "actual-pd-copy-pin-lifecycle", "actual-pd-archive-write-pin-lifecycle",
                "actual-pd-include-launcher-schema1-roundtrip"}) {
            cases.push_back(not_run(id, "Requires builder-observed branch or legally ready actual Pd process; a test EXE cannot mint trust"));
        }
        bool failed = false;
        bool missing = false;
        for (const auto& item : cases) {
            failed = failed || item.at("status") == "FAIL";
            missing = missing || item.at("status") == "NOT_RUN";
        }
        Json result{{"schema", "pd.trust-negatives.final.v1"}, {"status", failed ? "FAIL" : missing ? "NOT_RUN" : "PASS"},
            {"scope", "production-appservices-negative-only"}, {"productAccepted", false},
            {"sourceCommit", PD_TRUSTED_EXPORT_SOURCE_COMMIT}, {"sourceSha256", PD_TRUSTED_EXPORT_SOURCE_SHA256},
            {"graphSha256", graph_sha}, {"cases", cases}};
        write_new(output, result.dump(2) + "\n");
        return failed ? 1 : missing ? 77 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
