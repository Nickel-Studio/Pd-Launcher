#include "app/application_state.hpp"
#include "core/config/atomic_file.hpp"
#include "core/instances/icons.hpp"
#include "core/instances/settings.hpp"
#include "core/tasks/secure_fs.hpp"

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QImage>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace io = nickel::tasks::detail;
namespace instances = nickel::instances;
using nickel::Cancellation;
using nickel::Json;
using nickel::app::ApplicationState;
using nickel::app::OperationResult;
using nickel::ui::UiAction;
using nickel::ui::UiQuery;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Function>
void rejected(Function&& operation, std::string_view message) {
    bool failed = false;
    try { std::forward<Function>(operation)(); }
    catch (const std::exception& failure) {
        failed = true;
        std::cout << "REJECTED " << message << ": " << failure.what() << '\n';
    }
    check(failed, message);
}

template<class Function>
void rejected_code(Function&& operation, std::string_view expected, std::string_view message) {
    try { std::forward<Function>(operation)(); }
    catch (const nickel::ErrorException& failure) {
        check(failure.error().code == expected, message);
        std::cout << "REJECTED " << message << ": " << failure.error().code << '\n';
        return;
    }
    throw std::runtime_error(std::string(message));
}

void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    auto file = io::SecureFile::create_new(path);
    file.write(bytes);
    file.flush();
}

std::string read_file(const fs::path& path, std::size_t maximum = 64 * 1024) {
    const auto bytes = io::read_bounded(path, maximum);
    return bytes.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Json scope(const fs::path& root) {
    return {{"root", instances::path_to_utf8(root)}, {"versionId", "directory-id"}};
}

Json selected(const fs::path& root, const fs::path& path) {
    auto request = scope(root);
    request["path"] = instances::path_to_utf8(path);
    return request;
}

void seed_instance(const fs::path& root) {
    const auto directory = root / "versions" / "directory-id";
    fs::create_directories(directory);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const Json descriptor{{"id", "directory-id"}, {"type", "release"},
        {"mainClass", "net.minecraft.client.main.Main"}, {"minecraftVersion", "1.20.1"},
        {"future", maximum}, {"nickelCustom", {{"name", "Display label is not identity"},
        {"description", "Keep metadata"}, {"favorite", true}, {"favoriteAt", 123}, {"future", maximum}}}};
    nickel::atomic_write_text(directory / "directory-id.json", descriptor.dump());
    instances::set_version_settings(root, "directory-id", Json{{"windowWidth", 1024}, {"icon", "anvil"},
        {"future", {{"counter", maximum}, {"keep", true}}}});
}

QByteArray source_png() {
    QImage image(64, 48, QImage::Format_RGBA8888);
    check(!image.isNull(), "Cannot create source image");
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = y < 24 ? (x < 32 ? QColor(255, 0, 0) : QColor(0, 255, 0))
                                        : (x < 32 ? QColor(0, 0, 255) : QColor(255, 255, 255));
            image.setPixelColor(x, y, color);
        }
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "Cannot encode real selected PNG");
    return bytes;
}

void check_preserved(const fs::path& root, const std::string& descriptor) {
    const auto directory = root / "versions" / "directory-id";
    check(read_file(directory / "directory-id.json") == descriptor, "Icon action changes descriptor/display identity");
    const auto settings = instances::read_local_settings(root, "directory-id").value;
    check(settings.at("windowWidth") == 1024 && settings.at("future").at("keep") == true,
          "Icon action replaces unrelated version settings");
    check(settings.at("future").at("counter") == std::numeric_limits<std::uint64_t>::max(),
          "Icon action loses persisted UINT64");
}

std::string managed_media_key(const OperationResult& result, const Json& data,
                              const fs::path& root, const QImage& expected) {
    check(data.at("root") == instances::path_to_utf8(root) && data.at("versionId") == "directory-id",
          "Managed media changes canonical root/directory scope");
    const auto key = data.at("iconKey").get<std::string>();
    check(key.starts_with("instance:") && data.at("icon") == key && data.at("iconSource") == "managed",
          "Managed query does not return the actual opaque icon contract");
    check(data.at("mediaScope") == scope(root), "Managed image scope does not match its queried instance");
    check(result.media.size() == 1 && result.media.front().first == key,
          "Managed query omits its image or includes another instance's media");
    const auto& image = result.media.front().second;
    check(!image.isNull() && image.width() == 32 && image.height() == 32
          && image.convertToFormat(QImage::Format_RGBA8888) == expected.convertToFormat(QImage::Format_RGBA8888),
          "Typed managed media differs from the verified normalized PNG");
    return key;
}

void unmanaged_queries(ApplicationState& state, const fs::path& root) {
    const auto detail = state.query(UiQuery::VersionDetail, scope(root), Cancellation{});
    check(detail.media.empty() && !detail.data.contains("iconKey") && !detail.data.contains("mediaScope"),
          "Built-in, inherited or legacy icon is exposed as managed detail media");
    const auto list = state.query(UiQuery::Instances, Json{{"root", instances::path_to_utf8(root)}}, Cancellation{});
    check(list.media.empty() && list.data.at("items").size() == 1
          && !list.data.at("items").at(0).contains("iconKey") && !list.data.at("items").at(0).contains("mediaScope"),
          "Built-in, inherited or legacy icon is exposed as managed list media");
}

void real_icon_actions(const fs::path& fixture) {
    const auto executable = fixture / "bin" / "fixture.exe";
    fs::create_directories(executable.parent_path());
    fs::create_directories(fixture / "system");
    fs::create_directories(fixture / "data");
    nickel::app::AppServicesOptions options;
    options.paths = {executable, fixture / "system", fixture / "data", false};
    options.executable = executable;
    auto state = std::make_shared<ApplicationState>(std::move(options));
    const auto root = state->paths.default_game_directory();
    const auto other_root = fixture / "other-game";
    seed_instance(root);
    seed_instance(other_root);
    const auto directory = root / "versions" / "directory-id";
    const auto descriptor = read_file(directory / "directory-id.json");
    const auto other_settings_file = other_root / "versions" / "directory-id" / "nickel.settings.json";
    const auto other_settings = read_file(other_settings_file);
    const auto input = fixture / "selected.png";
    const auto png = source_png();
    write_file(input, std::as_bytes(std::span(png.constData(), static_cast<std::size_t>(png.size()))));
    const auto input_bytes = read_file(input);
    check(state->tasks->snapshots().empty(), "Fixture begins with unrelated registry work");

    std::cout << "CASE original-path-payload\n";
    const auto reply = state->action_instances(UiAction::SetInstanceIcon, selected(root, input), Cancellation{});
    check(reply.error_code.empty(), "Selected icon action reports failure");
    check(reply.data.at("versionId") == "directory-id" && reply.data.at("root") == instances::path_to_utf8(root),
          "Selected icon reply changes canonical root/directory identity");
    check(std::any_of(reply.changes.begin(), reply.changes.end(), [](const auto& change) {
        return change.first == nickel::ui::UiTopic::Instances;
    }), "Selected icon action does not refresh instances");
    auto managed = instances::read_instance_icon(root, "directory-id");
    check(managed.has_value(), "Selected path does not publish a managed icon");
    check(managed->reference.starts_with(".nickel-icons/") && managed->reference.ends_with(".png")
          && managed->reference != instances::path_to_utf8(input), "Selected path is persisted instead of a managed reference");
    check(managed->png.size() <= instances::managed_icon_max_bytes, "Managed icon exceeds 8192 bytes");
    QImage normalized;
    check(normalized.loadFromData(managed->png.data(), static_cast<int>(managed->png.size()), "PNG")
          && normalized.width() == 32 && normalized.height() == 32, "Managed icon is not a real 32x32 PNG");
    check(normalized.pixelColor(1, 1) == QColor(255, 0, 0)
          && normalized.pixelColor(30, 30) == QColor(255, 255, 255), "Icon normalization crops instead of resizing the full image");
    check_preserved(root, descriptor);
    check(read_file(input) == input_bytes && state->tasks->snapshots().empty(), "Icon action mutates its source or creates a task");
    check(read_file(other_settings_file) == other_settings, "Icon action edits the same directory ID in another root");

    std::cout << "CASE typed-managed-query\n";
    const auto original_key = managed_media_key(reply, reply.data, root, normalized);
    const auto detail = state->query(UiQuery::VersionDetail, scope(root), Cancellation{});
    check(managed_media_key(detail, detail.data, root, normalized) == original_key,
          "Detail query changes the managed action key");
    const auto list = state->query(UiQuery::Instances, Json{{"root", instances::path_to_utf8(root)}}, Cancellation{});
    check(list.data.at("items").size() == 1, "Scoped instance list changes its directory population");
    check(managed_media_key(list, list.data.at("items").at(0), root, normalized) == original_key,
          "Instance list and detail use different managed media keys");

    std::cout << "CASE content-addressed-repeat\n";
    const auto repeat_reply = state->action_instances(UiAction::SetInstanceIcon, selected(root, input), Cancellation{});
    const auto repeated = instances::read_instance_icon(root, "directory-id");
    check(repeated && repeated->reference == managed->reference,
          "Repeated selected input changes the managed reference");
    check(managed_media_key(repeat_reply, repeat_reply.data, root, normalized) == original_key,
          "Repeated selected input changes its scoped media key");
    unsigned icons = 0;
    for (const auto& file : fs::directory_iterator(directory / ".nickel-icons"))
        if (file.path().extension() == ".png") ++icons;
    check(icons == 1, "Repeated selected input republishes duplicate PNG files");

    const auto settings_file = directory / "nickel.settings.json";
    const auto before_rejection = read_file(settings_file);
    const auto reject_unchanged = [&](const Json& request, const Cancellation& cancellation, std::string_view message) {
        rejected([&] { (void)state->action_instances(UiAction::SetInstanceIcon, request, cancellation); }, message);
        check(read_file(settings_file) == before_rejection, "Rejected icon action changes durable settings");
        check_preserved(root, descriptor);
    };
    std::cout << "CASE invalid-selected-inputs\n";
    reject_unchanged(selected(root, fs::path("relative.png")), Cancellation{}, "Relative selected image is accepted");
    auto mixed = selected(root, input);
    mixed["icon"] = "fabric";
    reject_unchanged(mixed, Cancellation{}, "Mixed path and built-in icon payload is accepted");
    mixed = selected(root, input);
    mixed["patch"] = Json{{"icon", "fabric"}};
    reject_unchanged(mixed, Cancellation{}, "Mixed path and metadata patch payload is accepted");
    auto wrong_scope = selected(root, input);
    wrong_scope["versionId"] = "Display label is not identity";
    reject_unchanged(wrong_scope, Cancellation{}, "Display label is accepted as directory identity");
    wrong_scope["versionId"] = "../directory-id";
    reject_unchanged(wrong_scope, Cancellation{}, "Traversal is accepted as directory identity");
    wrong_scope = selected(root, input);
    wrong_scope["dir"] = instances::path_to_utf8(other_root);
    reject_unchanged(wrong_scope, Cancellation{}, "Conflicting root aliases retarget an icon action");
    reject_unchanged(selected(root, fixture / "missing.png"), Cancellation{}, "Missing selected image is accepted");
    reject_unchanged(selected(root, fixture / "data"), Cancellation{}, "Selected image directory is accepted");
    const auto malformed = fixture / "malformed.png";
    const std::string junk = "not a raster image";
    write_file(malformed, std::as_bytes(std::span(junk.data(), junk.size())));
    reject_unchanged(selected(root, malformed), Cancellation{}, "Malformed selected image is accepted");
    const auto oversized = fixture / "oversized.png";
    const std::vector<std::byte> excessive(instances::selected_icon_max_bytes + 1);
    write_file(oversized, excessive);
    reject_unchanged(selected(root, oversized), Cancellation{}, "Selected image exceeds the complete 4 MiB bound");
    const auto hardlink_source = fixture / "hardlink-source.png";
    const auto hardlink = fixture / "hardlink.png";
    write_file(hardlink_source, std::as_bytes(std::span(png.constData(), static_cast<std::size_t>(png.size()))));
    fs::create_hard_link(hardlink_source, hardlink);
    reject_unchanged(selected(root, hardlink), Cancellation{}, "Hardlinked selected image is accepted");
    Cancellation cancelled;
    cancelled.cancel();
    reject_unchanged(selected(root, input), cancelled, "Cancelled selected image reaches publication");
    rejected_code([&] { (void)state->query(UiQuery::VersionDetail, scope(root), cancelled); },
                  "cancelled", "Cancelled detail query reads managed media");
    rejected_code([&] { (void)state->query(UiQuery::Instances, Json{{"root", instances::path_to_utf8(root)}}, cancelled); },
                  "cancelled", "Cancelled instance query reads managed media");
    rejected([&] { (void)state->action_instances(UiAction::SetInstanceIcon, selected(other_root, input), Cancellation{}); },
             "Unregistered root is accepted for icon publication");
    rejected([&] { (void)state->query(UiQuery::VersionDetail, scope(other_root), Cancellation{}); },
             "Unregistered root is accepted for detail media");
    rejected([&] { (void)state->query(UiQuery::Instances, Json{{"root", instances::path_to_utf8(other_root)}}, Cancellation{}); },
             "Unregistered root is accepted for list media");
    check(read_file(other_settings_file) == other_settings, "Unauthorized root rejection changes its settings");
    check(read_file(settings_file) == before_rejection, "Rejected query changes durable icon settings");

    std::cout << "CASE registered-root-media-isolation\n";
    (void)state->action_instances(UiAction::AddRoot, Json{{"root", instances::path_to_utf8(other_root)}}, Cancellation{});
    const auto other_reply = state->action_instances(UiAction::SetInstanceIcon, selected(other_root, input), Cancellation{});
    const auto other_managed = instances::read_instance_icon(other_root, "directory-id");
    check(other_managed && other_managed->reference == managed->reference,
          "Equal normalized PNG bytes do not have equal content-addressed references");
    const auto other_key = managed_media_key(other_reply, other_reply.data, other_root, normalized);
    check(other_key != original_key, "Same directory ID and PNG in different roots share a media key");
    check(read_file(settings_file) == before_rejection, "Registering another root changes the original instance");
    const auto other_after_icon = read_file(other_settings_file);
    (void)state->action_instances(UiAction::RenameRoot,
        Json{{"root", instances::path_to_utf8(other_root)}, {"label", "Other registered display label"}}, Cancellation{});
    const auto relabeled = state->query(UiQuery::VersionDetail, scope(other_root), Cancellation{});
    check(managed_media_key(relabeled, relabeled.data, other_root, normalized) == other_key,
          "Root display label replaces canonical media identity");
    const auto explicit_original = state->action_instances(UiAction::SetInstanceIcon, selected(root, input), Cancellation{});
    check(managed_media_key(explicit_original, explicit_original.data, root, normalized) == original_key,
          "Active root retargets an explicitly scoped icon action");
    check(read_file(other_settings_file) == other_after_icon, "Explicit original-root action edits the active sibling root");
    check_preserved(root, descriptor);
    (void)state->action_instances(UiAction::RemoveRoot, Json{{"root", instances::path_to_utf8(other_root)}}, Cancellation{});
    rejected([&] { (void)state->query(UiQuery::VersionDetail, scope(other_root), Cancellation{}); },
             "Removed registered root remains authorized for detail media");
    rejected([&] { (void)state->action_instances(UiAction::SetInstanceIcon, selected(other_root, input), Cancellation{}); },
             "Removed registered root remains authorized for icon publication");
    check(read_file(other_settings_file) == other_after_icon && read_file(settings_file) == before_rejection,
          "Registration revocation changes durable instance icon settings");
    check(state->tasks->snapshots().empty(), "Scoped icon/root controls create registry work");

    std::cout << "CASE accepted-operation-task-contract\n";
    const auto operation = state->begin_operation(Json{{"operationId", "icon-conflict-control"}});
    state->start_operation(operation);
    Cancellation unrelated_request;
    check(operation->cancellation.stop_token() != unrelated_request.stop_token(),
          "Accepted operation does not own an independent cancellation token");
    check(!state->operation_task(operation->cancellation) && !state->operation_task(unrelated_request),
          "Nullable operation lookup invents a task before registration");
    nickel::tasks::TaskSpec specification;
    specification.target = directory;
    specification.kind = nickel::tasks::TaskKind::Version;
    specification.network = state->http->snapshot();
    specification.cancellation = operation->cancellation;
    auto task = state->tasks->begin(specification);
    state->register_operation_task(operation->cancellation, task);
    state->register_operation_task(operation->cancellation, task);
    check(operation->task_id == task->id() && task->cancellation().stop_token() == operation->cancellation.stop_token()
          && state->operation_task(operation->cancellation) == task && !state->operation_task(unrelated_request),
          "Task registration loses the accepted token or reuses another operation's task");
    rejected_code([&] { state->register_operation_task(unrelated_request, task); },
                  "integrity", "Task registration accepts a different cancellation identity");
    bool incorrectly_scheduled = false;
    rejected_code([&] {
        state->schedule_operation([&](const Cancellation&) { incorrectly_scheduled = true; }, unrelated_request, task);
    }, "integrity", "Task scheduler accepts a substituted cancellation identity");
    check(!incorrectly_scheduled && state->tasks->snapshots().size() == 1,
          "Rejected task handoff schedules work or duplicates registry tasks");
    const auto pending = state->pending_operation_snapshot();
    check(pending.size() == 1 && pending.at(0).at("operationId") == operation->operation_id
          && pending.at(0).at("taskId") == task->id() && task->id() != "directory-id",
          "Pending operation receipt substitutes the directory ID for the real registry task ID");
    check(state->pending_operation_snapshot({task->id()}).empty(), "Known task is duplicated in pending operation rows");
    state->finish_operation(operation, Json{{"taskId", task->id()}});
    check(operation->handler_complete && operation->task_id == task->id(), "Handler completion changes the registered task");

    std::cout << "CASE task-conflict-rejection\n";
    reject_unchanged(selected(root, input), Cancellation{}, "Active instance task is ignored by icon action");
    check(state->tasks->snapshots().size() == 1, "Busy icon action creates another registry task");
    check(state->cancel_operation(operation->operation_id) && operation->cancellation.cancelled(),
          "Operation cancellation does not reach its original task token");
    bool published_after_cancel = false;
    rejected_code([&] { task->commit_external([&] { published_after_cancel = true; }); },
                  "cancelled", "Cancelled accepted task enters publication");
    check(!published_after_cancel && task->snapshot().status == nickel::tasks::TaskStatus::Cancelled,
          "Cancelled task control admits publication or loses its terminal state");
    const auto events = state->tasks->since(0);
    check(std::count_if(events.events.begin(), events.events.end(), [&](const auto& event) {
        return event.kind == nickel::tasks::EventKind::Terminal && event.snapshot.id == task->id();
    }) == 1, "Registered task emits duplicate terminal events");
    check(!state->tasks->cancellation_for(task->id()) && !state->cancel_operation(operation->operation_id),
          "Terminal registered task exposes cancellation or accepts it again");

    std::cout << "CASE builtin-and-null-compatibility\n";
    auto builtin = scope(root);
    builtin["icon"] = "fabric";
    (void)state->action_instances(UiAction::SetInstanceIcon, builtin, Cancellation{});
    check(instances::read_local_settings(root, "directory-id").value.at("icon") == "fabric"
          && !instances::read_instance_icon(root, "directory-id"), "Built-in icon compatibility is lost");
    unmanaged_queries(*state, root);
    builtin["icon"] = nullptr;
    (void)state->action_instances(UiAction::SetInstanceIcon, builtin, Cancellation{});
    check(instances::read_local_settings(root, "directory-id").value.at("icon").is_null()
          && !instances::read_instance_icon(root, "directory-id"), "Null icon does not inherit");
    unmanaged_queries(*state, root);
    check_preserved(root, descriptor);

    std::cout << "CASE legacy-external-reference-is-not-media\n";
    auto legacy = instances::read_local_settings(root, "directory-id").value;
    legacy["icon"] = instances::path_to_utf8(input);
    nickel::atomic_write_text(settings_file, legacy.dump());
    check(!instances::read_instance_icon(root, "directory-id"), "Legacy arbitrary path is exposed as managed media");
    unmanaged_queries(*state, root);
    check(read_file(input) == input_bytes, "Legacy media read changes the selected file");
    const auto replacement = state->action_instances(UiAction::SetInstanceIcon, selected(root, input), Cancellation{});
    managed = instances::read_instance_icon(root, "directory-id");
    check(managed.has_value(), "Legacy external reference prevents an approved selected-file replacement");
    check(managed_media_key(replacement, replacement.data, root, normalized) == original_key,
          "Approved replacement of a legacy reference changes the stable scoped key");
    const auto published = directory / instances::path_from_utf8(managed->reference);
    auto tampered = managed->png;
    tampered.at(tampered.size() / 2) ^= 1;
    nickel::atomic_write_text(published, {reinterpret_cast<const char*>(tampered.data()), tampered.size()});
    rejected([&] { (void)instances::read_instance_icon(root, "directory-id"); },
             "Changed managed PNG is returned without digest verification");
    rejected([&] { (void)state->query(UiQuery::VersionDetail, scope(root), Cancellation{}); },
             "Changed managed PNG is returned as detail media");
    rejected([&] { (void)state->query(UiQuery::Instances, Json{{"root", instances::path_to_utf8(root)}}, Cancellation{}); },
             "Changed managed PNG is returned as list media");
    check(state->tasks->snapshots().size() == 1, "Icon controls manufacture registry work");
    state->shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        check(argc == 2, "An exact owned fixture path is required");
        const auto fixture = io::checked_absolute(io::utf8_path(argv[1]));
        check(!fs::exists(io::io_path(fixture)), "Icon fixture path already exists");
        io::create_directory_new(fixture);
        real_icon_actions(fixture);
        std::cout << "PASS real ApplicationState selected-icon actions and canonical managed PNG controls\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL production icon actions: " << failure.what() << '\n';
        return 1;
    }
}
