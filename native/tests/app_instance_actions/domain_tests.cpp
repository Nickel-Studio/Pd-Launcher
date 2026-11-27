#include "app/WorkerQueue.hpp"
#include "app/service/mutation_gate.hpp"
#include "app/service/operation_queue.hpp"
#include "core/common/cancellation.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/repository.hpp"
#include "core/instances/domain_io.hpp"
#include "core/instances/instances.hpp"
#include "core/instances/settings.hpp"
#include "core/instances/startup.hpp"

#include <condition_variable>
#include <future>
#include <mutex>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using nickel::Json;
namespace fs = std::filesystem;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Fn> void rejected(Fn&& operation, const char* message) {
    bool threw = false;
    try { operation(); }
    catch (const nickel::ErrorException&) { threw = true; }
    check(threw, message);
}

Json descriptor(std::string id) {
    return {{"id", std::move(id)}, {"type", "release"}, {"mainClass", "net.minecraft.client.main.Main"},
            {"minecraftVersion", "1.20.1"}, {"future", std::numeric_limits<std::uint64_t>::max()},
            {"nickelCustom", {{"name", "Original"}, {"description", "Keep description"},
                              {"favorite", false}, {"future", std::numeric_limits<std::uint64_t>::max()}}}};
}

void root_and_settings(const fs::path& work) {
    const auto config = work / "config";
    const auto root = work / "default-game";
    fs::create_directories(config);
    nickel::instances::InstanceRegistry registry(config, root);
    check(registry.ensure_default() == root, "default registration differs");
    const auto parent = work / "created";
    fs::create_directories(parent);
    const auto created = registry.create_root(parent);
    registry.rename_root(created, "Display root");
    auto roots = registry.get();
    check(roots.active == nickel::instances::path_to_utf8(created), "created root is not active");
    check(roots.labels[nickel::instances::path_to_utf8(created)] == "Display root", "root label not durable");
    roots.unknown["future"] = std::numeric_limits<std::uint64_t>::max();
    registry.set(roots);
    check(registry.get().unknown["future"] == std::numeric_limits<std::uint64_t>::max(), "root UINT64 lost");
    const auto version = root / "versions" / "directory-id";
    fs::create_directories(version);
    nickel::atomic_write_text(version / "directory-id.json", descriptor("directory-id").dump());
    nickel::instances::set_version_settings(root, "directory-id", Json{{"windowWidth", 1024},
        {"future", {{"counter", std::numeric_limits<std::uint64_t>::max()}, {"retained", true}}},
        {"icon", "anvil"}});
    auto local = nickel::instances::read_local_settings(root, "directory-id").value;
    local.update(Json{{"windowHeight", 600}, {"future", {{"added", true}}}}, true);
    nickel::instances::set_version_settings(root, "directory-id", local);
    const auto persisted = nickel::instances::read_local_settings(root, "directory-id").value;
    check(persisted["windowWidth"] == 1024 && persisted["windowHeight"] == 600, "patch lost overrides");
    check(persisted["future"]["counter"] == std::numeric_limits<std::uint64_t>::max(), "patch UINT64 lost");
    check(persisted["future"]["retained"] == true && persisted["future"]["added"] == true, "nested patch lost future values");
    const auto defaults = nickel::instances::default_version_settings();
    for (const auto& [key, value] : defaults.items()) local[key] = value;
    nickel::instances::set_version_settings(root, "directory-id", local);
    const auto reset = nickel::instances::read_local_settings(root, "directory-id").value;
    check(reset["windowWidth"].is_null() && reset["memoryMode"] == "global", "reset does not inherit");
    check(reset["future"]["counter"] == std::numeric_limits<std::uint64_t>::max(), "reset loses unknown metadata");
    const auto file_before = nickel::read_text_bounded(version / "directory-id.json", 8192);
    check(Json::parse(file_before)["id"] == "directory-id", "settings modified descriptor identity");
    registry.remove_root(created, false, {});
    check(fs::is_directory(created), "unregister deletes files");
    const auto game_setting = Json::parse(nickel::read_text_bounded(version / "directory-id.json", 8192));
    check(game_setting["nickelCustom"]["name"] == "Original", "settings mixed display file");
    rejected([&] { registry.remove_root(config, true, {}); }, "protected config is removable");
}

void metadata_controls(const fs::path& work) {
    const auto root = work / "metadata-game";
    const auto other_root = work / "other-metadata-game";
    const auto directory = root / "versions" / "directory-id";
    const auto other_directory = other_root / "versions" / "directory-id";
    fs::create_directories(directory);
    fs::create_directories(other_directory);
    const auto file = directory / "directory-id.json";
    const auto settings_file = directory / "nickel.settings.json";
    nickel::atomic_write_text(file, descriptor("directory-id").dump());
    nickel::atomic_write_text(other_directory / "directory-id.json", descriptor("directory-id").dump());
    nickel::instances::set_version_settings(root, "directory-id", Json{{"icon", "anvil"},
        {"windowWidth", 1024}, {"future", {{"counter", std::numeric_limits<std::uint64_t>::max()}}}});
    const auto original_settings = nickel::read_text_bounded(settings_file, 8192);
    const auto other_original = nickel::read_text_bounded(other_directory / "directory-id.json", 8192);
    nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"name", "Display only"}});
    auto changed = Json::parse(nickel::read_text_bounded(file, 8192));
    check(changed["id"] == "directory-id" && changed["nickelCustom"]["name"] == "Display only",
          "display metadata changes directory identity");
    check(changed["nickelCustom"]["description"] == "Keep description", "display patch replaces omitted fields");
    check(changed["future"] == std::numeric_limits<std::uint64_t>::max()
          && changed["nickelCustom"]["future"] == std::numeric_limits<std::uint64_t>::max(), "metadata loses UINT64 fields");
    check(nickel::read_text_bounded(settings_file, 8192) == original_settings, "display metadata changes local settings");
    check(nickel::read_text_bounded(other_directory / "directory-id.json", 8192) == other_original,
          "same directory ID in a different root is modified");
    nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"favorite", true}});
    changed = Json::parse(nickel::read_text_bounded(file, 8192));
    const auto favorite_at = changed["nickelCustom"]["favoriteAt"];
    check(changed["nickelCustom"]["favorite"] == true && favorite_at.is_number_integer(), "favorite lacks a durable timestamp");
    nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"favorite", true}});
    changed = Json::parse(nickel::read_text_bounded(file, 8192));
    check(changed["nickelCustom"]["favoriteAt"] == favorite_at, "repeated favorite changes its ordering timestamp");
    nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"favorite", false}});
    changed = Json::parse(nickel::read_text_bounded(file, 8192));
    check(changed["nickelCustom"]["favorite"] == false && changed["nickelCustom"]["favoriteAt"].is_null(),
          "unfavorite leaves an ordering timestamp");
    const auto display_before_icon = nickel::read_text_bounded(file, 8192);
    nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"icon", "fabric"}});
    const auto icon_settings = nickel::instances::read_local_settings(root, "directory-id").value;
    check(icon_settings["icon"] == "fabric" && icon_settings["windowWidth"] == 1024,
          "icon-only patch replaces unrelated overrides");
    check(icon_settings["future"]["counter"] == std::numeric_limits<std::uint64_t>::max(), "icon patch loses UINT64 metadata");
    check(nickel::read_text_bounded(file, 8192) == display_before_icon, "icon-only patch writes display metadata");
    nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"icon", nullptr}});
    check(nickel::instances::read_local_settings(root, "directory-id").value["icon"].is_null(), "reset icon does not inherit");
    const auto settings_before_rejection = nickel::read_text_bounded(settings_file, 8192);
    rejected([&] { nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"name", "Mixed"}, {"icon", "grass"}}); },
             "mixed display/icon metadata is accepted");
    rejected([&] { nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"icon", "../external.png"}}); },
             "unmanaged icon path is accepted");
    rejected([&] { nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"favorite", "true"}}); },
             "non-boolean favorite is accepted");
    rejected([&] { nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"id", "new-id"}}); },
             "metadata can patch directory identity");
    nickel::Cancellation cancellation;
    cancellation.cancel();
    rejected([&] { nickel::instances::patch_instance_metadata(root, "directory-id", Json{{"name", "Cancelled"}},
                                                              cancellation.stop_token()); }, "cancelled metadata is committed");
    check(nickel::read_text_bounded(file, 8192) == display_before_icon, "rejected display patch changes durable bytes");
    check(nickel::read_text_bounded(settings_file, 8192) == settings_before_rejection, "rejected icon patch changes durable bytes");
}


void transaction_lifecycle(const fs::path& work) {
    const auto root = work / "transaction-game";
    fs::create_directories(root);
    {
        auto transaction = nickel::instances::InstanceTransaction::reserve(root, "Named", "1.20.1");
        const auto target = transaction.directory();
        transaction.check_owned();
        check(nickel::instances::is_pending(target), "reservation has no pending marker");
        check(nickel::instances::scan_instances(root).empty(), "pending reservation is visible");
        rejected([&] { nickel::instances::load_profile(root, "Named"); }, "public profile loads pending instance");
        transaction.complete({"1.20.1", std::nullopt, {}, descriptor("1.20.1"), nullptr});
        check(nickel::instances::scan_instances(root).empty(), "complete publishes before commit");
        nickel::Cancellation cancelled;
        cancelled.cancel();
        rejected([&] { transaction.commit("Display", Json::object(), cancelled.stop_token()); }, "cancelled commit publishes");
        check(nickel::instances::is_pending(target), "cancelled commit drops marker");
        transaction.cleanup();
        check(!fs::exists(target), "drained cancelled cleanup leaves target");
    }
    {
        auto transaction = nickel::instances::InstanceTransaction::reserve(root, "Published", "1.20.1");
        const auto inner = transaction.directory() / "instances" / "Published";
        const auto inner_descriptor = inner / "versions" / "1.20.1" / "1.20.1.json";
        nickel::atomic_write_text(inner_descriptor, descriptor("1.20.1").dump());
        transaction.check_owned();
        transaction.complete({"1.20.1", std::nullopt, {}, descriptor("1.20.1"), nullptr});
        transaction.commit("Not the directory ID", Json{{"origin", "test-fixture"}});
        const auto published = Json::parse(nickel::read_text_bounded(transaction.directory() / "Published.json", 8192));
        check(published["id"] == "Published" && published["nickelCustom"]["name"] == "Not the directory ID", "display changes identity");
        check(published["future"] == std::numeric_limits<std::uint64_t>::max(), "complete loses unknown UINT64");
        check(!nickel::instances::is_pending(transaction.directory()), "commit remains pending");
        check(nickel::instances::instance_detail(root, "Published").ready, "published profile not ready");
        const auto isolated = nickel::instances::resolve_launch_target(root, "Published", Json::object());
        check(isolated.install_root == inner && isolated.installed_version_id == "1.20.1",
              "named launch mixes its outer directory and Minecraft installation root");
        check(isolated.resource_root == transaction.directory(), "isolated resources do not use the outer instance");
        check(isolated.descriptor_path == inner_descriptor, "launch does not resolve the exact inner descriptor");
        const auto shared = nickel::instances::resolve_launch_target(root, "Published", Json{{"versionIsolation", false}});
        check(shared.install_root == inner && shared.resource_root == root, "shared resources change the inner installation root");
        transaction.cleanup();
        check(fs::is_directory(transaction.directory()), "cleanup removes a committed instance");
        const auto legacy = transaction.directory() / "versions" / "1.20.1" / "1.20.1.json";
        nickel::atomic_write_text(legacy, descriptor("1.20.1").dump());
        rejected([&] { nickel::instances::resolve_launch_target(root, "Published", Json::object()); },
                 "ambiguous legacy/native layout is accepted");
    }
    {
        auto transaction = nickel::instances::InstanceTransaction::reserve(root, "Recovery", "1.20.1");
        transaction.leave_for_recovery(false);
    }
    check(nickel::instances::is_pending(root / "versions" / "Recovery"), "undrained recovery cleans target");
    const auto recovered = nickel::instances::recover_interrupted_instances(root);
    check(std::find(recovered.skipped.begin(), recovered.skipped.end(), "Recovery") != recovered.skipped.end(),
          "undrained live writer is recovered");
}

void authorization_and_cancel(const fs::path& work) {
    static_assert(std::is_base_of_v<nickel::ErrorException, nickel::instances::Error>);
    rejected([&] { nickel::instances::checked_instance_directory(work, "../outside", false); }, "traversal ID accepted");
    rejected([&] { nickel::instances::checked_instance_directory(fs::path("relative"), "valid", false); }, "relative root accepted");
    nickel::app::MutationGate gate;
    nickel::Cancellation cancellation;
    const auto root = nickel::instances::root_identity(nickel::instances::path_to_utf8(work));
    auto lease = gate.instance(root, "directory-id", cancellation);
    rejected([&] { (void)gate.root(root, cancellation); }, "root conflicts do not honor instance lease");
    auto other = gate.instance(root, "other-directory-id", cancellation);
    auto shared_cancel = cancellation;
    shared_cancel.cancel();
    rejected([&] { cancellation.check(); }, "typed cancellation copy is detached");
    nickel::JsonRepository repository(work / "cancel-config");
    repository.write("settings.json", Json{{"future", std::numeric_limits<std::uint64_t>::max()}});
    rejected([&] { repository.update("settings.json", Json::object(), [](Json& value) {
        value["selectedInstance"] = Json{{"root", "unauthorized"}, {"versionId", "arbitrary"}};
    }, nickel::settings_max_bytes, &cancellation); }, "cancelled repository update succeeds");
    check(!repository.read("settings.json").contains("selectedInstance"), "cancelled update is durable");
}

void serial_admission_controls(const fs::path& work) {
    nickel::JsonRepository repository(work / "queue-config");
    repository.write("settings.json", Json{{"future", std::numeric_limits<std::uint64_t>::max()}});
    struct Progress {
        std::mutex mutex;
        std::condition_variable changed;
        bool release = false;
        std::size_t entered = 0;
        std::size_t finished = 0;
        std::size_t active = 0;
        std::size_t maximum = 0;
    };
    const auto progress = std::make_shared<Progress>();
    nickel::app::WorkerQueue workers(2, 16);
    const auto operations = std::make_shared<nickel::app::OperationQueue>();
    const auto dispatch = [&workers](nickel::app::OperationQueue::Work job) { return workers.submit(std::move(job)); };
    const auto long_work = [progress](const nickel::Cancellation& cancellation) {
        std::stop_callback stop(cancellation.stop_token(), [progress] { progress->changed.notify_all(); });
        std::unique_lock lock(progress->mutex);
        ++progress->entered;
        ++progress->active;
        progress->maximum = std::max(progress->maximum, progress->active);
        progress->changed.notify_all();
        progress->changed.wait_for(lock, std::chrono::seconds(4), [&] {
            return progress->release || cancellation.cancelled();
        });
        --progress->active;
        ++progress->finished;
        progress->changed.notify_all();
    };
    nickel::Cancellation first;
    nickel::Cancellation second;
    operations->enqueue(long_work, first, dispatch);
    bool first_started;
    {
        std::unique_lock lock(progress->mutex);
        first_started = progress->changed.wait_for(lock, std::chrono::seconds(2), [&] { return progress->entered == 1; });
    }
    operations->enqueue(long_work, second, dispatch);
    auto promise = std::make_shared<std::promise<Json>>();
    auto reply = promise->get_future();
    const bool query_submitted = workers.submit([promise, &repository](const nickel::Cancellation& cancellation) {
        try {
            cancellation.check();
            promise->set_value(repository.read("settings.json"));
        } catch (...) { promise->set_exception(std::current_exception()); }
    });
    const bool query_ready = reply.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    std::size_t entered_before_release;
    {
        std::lock_guard lock(progress->mutex);
        entered_before_release = progress->entered;
        progress->release = true;
    }
    progress->changed.notify_all();
    bool finished;
    std::size_t maximum;
    {
        std::unique_lock lock(progress->mutex);
        finished = progress->changed.wait_for(lock, std::chrono::seconds(4), [&] { return progress->finished == 2; });
        maximum = progress->maximum;
    }
    operations->close();
    workers.stop();
    check(first_started && query_submitted && query_ready, "two long operations starve the existing query worker");
    check(entered_before_release == 1 && maximum == 1 && finished, "long operations bypass serial admission");
    check(reply.get()["future"] == std::numeric_limits<std::uint64_t>::max(), "concurrent query loses stored UINT64");
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture root argument required");
        const auto work = fs::absolute(nickel::instances::path_from_utf8(argv[1]));
        check(!fs::exists(work), "refuse pre-existing fixture directory");
        fs::create_directories(work);
        root_and_settings(work);
        metadata_controls(work);
        serial_admission_controls(work);
        transaction_lifecycle(work);
        authorization_and_cancel(work);
        std::cout << "PASS real-domain roots/settings/metadata/transaction/layout/admission/auth/cancellation controls\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL real-domain fixture: " << error.what() << '\n';
        return 1;
    }
}
