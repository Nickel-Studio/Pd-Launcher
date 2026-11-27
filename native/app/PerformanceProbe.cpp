#include "PerformanceProbe.hpp"

#include "BuildInfo.hpp"
#include "core/config/atomic_file.hpp"
#include "core/network/hash.hpp"
#include "ui/MainWindow.hpp"
#include "ui/StartupGate.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFontInfo>
#include <QTimer>
#include <QEvent>
#include <QImageWriter>
#include <QIODevice>
#include <QScreen>
#include <QThread>
#include <QWindow>

#include <cmath>
#include <cstdlib>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nickel::app {
namespace {
QEvent::Type input_probe_event() {
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
}

template<class T>
Json optional_number(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}

class SecurePngDevice final : public QIODevice {
public:
    explicit SecurePngDevice(tasks::detail::SecureFile& file, std::uint64_t budget) : file_(file), budget_(budget) {
        if (!open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot open guarded rendered PNG output");
    }
    bool isSequential() const override { return true; }
    [[nodiscard]] std::uint64_t written() const { return written_; }
protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 length) override {
        if (length < 0 || static_cast<std::uint64_t>(length) > budget_ - written_) return -1;
        try {
            file_.write(std::span(reinterpret_cast<const std::byte*>(data), static_cast<std::size_t>(length)));
            written_ += static_cast<std::uint64_t>(length);
            return length;
        } catch (...) { return -1; }
    }
private:
    tasks::detail::SecureFile& file_;
    std::uint64_t budget_;
    std::uint64_t written_ = 0;
};

Json window_observation(const QWidget& window, const QImage& image) {
    if (!window.isVisible() || window.isMinimized() || !window.windowHandle()
        || !window.windowHandle()->isExposed() || !std::isfinite(window.devicePixelRatioF())
        || std::abs(image.devicePixelRatio() - window.devicePixelRatioF()) > 0.0001)
        throw std::runtime_error("Rendered capture has no matching exposed production window");
    Json result{{"qtWindowWidth", window.width()}, {"qtWindowHeight", window.height()},
        {"qtWindowDevicePixelRatio", window.devicePixelRatioF()}, {"windowIdentityObserved", false}};
    if (const auto* screen = window.screen()) {
        const auto geometry = screen->geometry();
        result["qtScreenGeometry"] = {{"x", geometry.x()}, {"y", geometry.y()}, {"width", geometry.width()}, {"height", geometry.height()}};
        result["qtScreenDevicePixelRatio"] = screen->devicePixelRatio();
        result["qtScreenLogicalDotsPerInch"] = screen->logicalDotsPerInch();
        result["qtScreenPhysicalDotsPerInch"] = screen->physicalDotsPerInch();
    }
#ifdef Q_OS_WIN
    if (QApplication::platformName() != QStringLiteral("windows")
        || !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        throw std::runtime_error("Virtualized or non-Windows pixels are not actual window DPI evidence");
    const auto handle = reinterpret_cast<HWND>(window.winId());
    DWORD owner = 0;
    RECT client{}, outer{};
    if (!IsWindow(handle) || !GetWindowThreadProcessId(handle, &owner) || owner != GetCurrentProcessId()
        || !GetClientRect(handle, &client) || !GetWindowRect(handle, &outer))
        throw std::runtime_error("The captured HWND is not owned by the production process");
    POINT origin{client.left, client.top};
    const auto dpi = GetDpiForWindow(handle);
    if (!dpi || !ClientToScreen(handle, &origin) || client.right - client.left != image.width()
        || client.bottom - client.top != image.height())
        throw std::runtime_error("Rendered pixels do not match the actual physical client dimensions");
    result["windowIdentityObserved"] = true;
    result["platform"] = "windows";
    result["hwnd"] = static_cast<quint64>(window.winId());
    result["nativeWindowDpi"] = dpi;
    result["dpiAwareness"] = "per-monitor-v2";
    result["physicalClientRect"] = {{"x", origin.x}, {"y", origin.y},
        {"width", client.right - client.left}, {"height", client.bottom - client.top}};
    result["physicalWindowRect"] = {{"x", outer.left}, {"y", outer.top},
        {"width", outer.right - outer.left}, {"height", outer.bottom - outer.top}};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor))
        throw std::runtime_error("The actual capture monitor is unavailable");
    result["physicalMonitorRect"] = {{"x", monitor.rcMonitor.left}, {"y", monitor.rcMonitor.top},
        {"width", monitor.rcMonitor.right - monitor.rcMonitor.left}, {"height", monitor.rcMonitor.bottom - monitor.rcMonitor.top}};
#else
    result["platform"] = QApplication::platformName().toStdString();
    result["hwnd"] = nullptr;
    result["nativeWindowDpi"] = nullptr;
#endif
    return result;
}

Json current_visual_state(ui::MainWindow& window, const Json& labels) {
    auto state = labels;
    auto route = ui::routeKey(window.navigation().route()).toStdString();
    if (window.navigation().route() == ui::Route::More) {
        const auto* page = window.routePage(ui::Route::More);
        const auto* pager = page ? page->findChild<ui::Pager*>() : nullptr;
        if (!pager || pager->selected() != QStringLiteral("about"))
            throw std::runtime_error("The actual More pager is not on About");
        route = "more/about";
    }
    state["route"] = route;
    state["windowLogicalSize"] = {window.width(), window.height()};
    state["themeId"] = ui::accentKey(window.theme().appearance().accent).toStdString();
    state["mode"] = window.theme().tokens().dark ? "dark" : "light";
    state["blur"] = labels.at("blur").is_number_float()
        ? Json(static_cast<double>(window.theme().appearance().backgroundBlur)) : Json(window.theme().appearance().backgroundBlur);
    state["reducedMotion"] = window.theme().reducedMotion();
#ifdef Q_OS_WIN
    const auto dpi = GetDpiForWindow(reinterpret_cast<HWND>(window.winId()));
    if (!dpi || static_cast<std::uint64_t>(dpi) * 100 % 96)
        throw std::runtime_error("Actual Windows DPI cannot be represented by the frozen scenario");
    state["dpiPercent"] = static_cast<std::uint64_t>(dpi) * 100 / 96;
#else
    throw std::runtime_error("This presentation acceptance requires actual Windows DPI");
#endif
    return state;
}

} // namespace

struct PerformanceRecord::Impl {
    const AcceptancePaths& paths;
    std::optional<tasks::detail::SecureFile> output;
    Json identity = Json::object();
    const QThread* thread = QThread::currentThread();
    std::optional<AcceptancePhaseRequest> active_phase;
    std::set<std::string> phase_nonces;
    std::optional<qint64> phase_reply_ns;
    Json phase_state = Json::object();
    std::string phase_state_sha256;
    bool phase_frame_recorded = false;
    std::uint64_t bytes = 0;
    std::uint64_t png_bytes = 0;
    std::uint64_t sequence = 0;
    bool finished = false;

    explicit Impl(const AcceptancePaths& accepted) : paths(accepted) {
        paths.verify();
        const auto& request = paths.request();
        const auto found = request.find("productionEvents");
        if (found == request.end() || !found->is_string()) throw std::runtime_error("A guarded production event output is required");
        const auto file = path_from_utf8(found->get<std::string>());
        if (!file.is_absolute() || foundation_detail::checked_absolute_path(file).parent_path() != paths.context_file().parent_path()
            || file.filename() == paths.context_file().filename()) throw std::runtime_error("The production event output is not owned by this context");
        reject_path_links(file);
        output.emplace(tasks::detail::SecureFile::create_new(file));
        for (const auto name : {"runId", "scenarioId", "requestNonce", "phaseRequestNonce", "phase", "sourceCommit", "sourceSnapshotSha256",
                                "buildSourceSha256", "driverSourceSha256", "inputCorpusSha256", "executableSha256"})
            identity[name] = request.at(name);
        identity["schemaVersion"] = 1;
        identity["protocol"] = "nickel-evidence-driver-v1";
        identity["origin"] = "actual-launcher-production-event";
        identity["role"] = "native";
        identity["observationAdapter"] = "native-production-markers-v1";
        identity["product"] = std::string(build::display_name);
        identity["executableName"] = std::string(build::executable_name);
        identity["clock"] = PerformanceApplication::clock_method().toStdString();
#ifdef Q_OS_WIN
        identity["platform"] = "windows";
        identity["clock"] = "perf_counter_ns";
        identity["clockImplementation"] = "QueryPerformanceCounter";
#else
        identity["platform"] = QApplication::platformName().toStdString();
#endif
        identity["rootPid"] = QCoreApplication::applicationPid();
        identity["rootCreationTime100ns"] = nullptr;
        identity["phaseBound"] = false;
        identity["sourceBindingsOrigin"] = "runner-context-claims-not-build-attestation";
#ifdef Q_OS_WIN
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            throw std::runtime_error("The production process identity is unavailable");
        identity["rootCreationTime100ns"] = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
#endif
    }

    void append(const char* name, const Json& fields) {
        if (finished || QThread::currentThread() != thread || !fields.is_object() || sequence >= 128)
            throw std::runtime_error("Invalid production event lifecycle");
        paths.verify();
        auto event = identity;
        event["event"] = name;
        event["sequence"] = ++sequence;
        event["monotonicNs"] = PerformanceApplication::monotonic_nanoseconds();
        event["observation"] = fields;
        const auto text = event.dump(-1, ' ', true) + '\n';
        if (text.size() > 64 * 1024 || bytes + text.size() > 1024 * 1024)
            throw std::runtime_error("The production event budget is exhausted");
        output->write(std::as_bytes(std::span(text)));
        output->flush();
        bytes += text.size();
    }
};

PerformanceRecord::PerformanceRecord(const AcceptancePaths& paths, qint64 entry_ns)
    : impl_(std::make_unique<Impl>(paths)) {
    marker("entry", {{"entryMonotonicNs", entry_ns}, {"timestampBasis", "main-before-QApplication"}});
    const auto& options = paths.options();
    marker("pathContextAccepted", {{"protocol", "explicit-accepted-path-context-v1"},
        {"explicitAcceptedPathContext", true}, {"executableGuardPassed", true}, {"fixturePathsGuardPassed", true},
        {"allDataRootsWithinRunFixture", true}, {"systemKnownFolderAccess", "disabled"},
        {"dataRoots", {{"config", path_to_utf8(options.config_root)}, {"game", path_to_utf8(options.game_root)},
                       {"cache", path_to_utf8(options.cache_root)}, {"temp", path_to_utf8(options.temporary_root)}}}});
}

PerformanceRecord::~PerformanceRecord() = default;

std::optional<AcceptancePhaseRequest> PerformanceRecord::poll_phase_request() {
    if (impl_->finished || QThread::currentThread() != impl_->thread)
        throw std::runtime_error("Invalid phase observation lifecycle");
    const auto birth = impl_->identity.at("rootCreationTime100ns");
    if (!birth.is_number_unsigned()) throw std::runtime_error("Actual kernel process birth is required for phase requests");
    auto phase = impl_->paths.phase_request(impl_->identity.at("rootPid").get<std::uint64_t>(),
        birth.get<std::uint64_t>(), PerformanceApplication::monotonic_nanoseconds());
    if (!phase) return std::nullopt;
    const auto nonce = phase->request.at("phaseRequestNonce").get<std::string>();
    const auto name = phase->request.at("phase").get<std::string>();
    if (impl_->phase_nonces.contains(nonce)) {
        if (!impl_->active_phase || nonce != impl_->active_phase->request.at("phaseRequestNonce").get<std::string>()
            || phase->context_sha256 != impl_->active_phase->context_sha256)
            throw std::runtime_error("Stale or changed acceptance phase nonce");
        return std::nullopt;
    }
    if (impl_->phase_nonces.size() >= 32 || (!impl_->active_phase && name != "ready"))
        throw std::runtime_error("Invalid first phase or phase request budget");
    if (impl_->active_phase) {
        const auto& previous = impl_->active_phase->request;
        if (!impl_->phase_reply_ns || !impl_->phase_frame_recorded || previous.at("phase") == "shutdown" || name == "ready"
            || phase->request.at("phaseBeginNs").get<std::uint64_t>() <= previous.at("phaseBeginNs").get<std::uint64_t>())
            throw std::runtime_error("Acceptance phase skipped its capture/reply or moved backwards");
    }
    impl_->paths.verify();
    impl_->active_phase = *phase;
    impl_->phase_nonces.insert(nonce);
    impl_->phase_reply_ns.reset();
    impl_->phase_state = Json::object();
    impl_->phase_state_sha256.clear();
    impl_->phase_frame_recorded = false;
    for (const auto* key : {"phase", "phaseRequestNonce", "phaseBeginNs", "launchBeginNs"}) impl_->identity[key] = phase->request.at(key);
    impl_->identity["phaseContextSha256"] = phase->context_sha256;
    impl_->identity["phaseBound"] = true;
    marker("phaseRequestAccepted", {{"requestObservedFrom", "guarded-live-launchContext"},
        {"contextSha256", phase->context_sha256}, {"sourceBindingsOrigin", "runner-context-claims-not-build-attestation"}});
    return phase;
}


void PerformanceRecord::marker(const char* name, const Json& fields) { impl_->append(name, fields); }

void PerformanceRecord::frame(const FrameMeasurement& measurement) {
    marker("paintComplete", {{"paintCompleteMonotonicNs", optional_number(measurement.paint_complete_ns)},
        {"flushCompleteMonotonicNs", optional_number(measurement.backing_store_flush_complete_ns)},
        {"presentDwmConfirmedMonotonicNs", optional_number(measurement.present_dwm_confirmed_ns)},
        {"dwmFlushHresult", optional_number(measurement.dwm_flush_hresult)},
        {"hwnd", optional_number(measurement.native_window_id)}, {"nativeWindowDpi", optional_number(measurement.native_window_dpi)},
        {"presentationStatus", measurement.presentation_status.toStdString()},
        {"flushMethod", measurement.flush_method.toStdString()}, {"ready", false}});
}

void PerformanceRecord::rendered_frame(const QImage& image, const QWidget* window, std::optional<qint64> capture_begin_ns) {
    const auto complete_ns = PerformanceApplication::monotonic_nanoseconds();
    if (impl_->active_phase && (!impl_->phase_reply_ns || !capture_begin_ns || impl_->phase_state.empty()
        || *capture_begin_ns < *impl_->phase_reply_ns || complete_ns < *capture_begin_ns))
        throw std::runtime_error("A phase frame must follow its fresh queued RuntimeInformation state reply");
    if (image.isNull()) {
        marker("renderedFrameUnavailable", {{"rendered", false}, {"presented", false}, {"ready", false},
            {"captureBeginMonotonicNs", optional_number(capture_begin_ns)}, {"captureCompleteMonotonicNs", complete_ns}});
        return;
    }
    if (!window || image.sizeInBytes() <= 0 || image.sizeInBytes() > 32 * 1024 * 1024 || image.depth() > 32)
        throw std::runtime_error("Rendered frame exceeds the bounded capture plane or lacks a production window");
    auto fields = window_observation(*window, image);
    const auto filename = std::string("rendered-") + impl_->identity.at("phaseRequestNonce").get<std::string>()
        + "-" + std::to_string(impl_->sequence + 1) + ".png";
    const auto file = impl_->paths.context_file().parent_path() / path_from_utf8(filename);
    constexpr std::uint64_t one_png_budget = 4 * 1024 * 1024;
    constexpr std::uint64_t total_png_budget = 64 * 1024 * 1024;
    if (impl_->png_bytes > total_png_budget - one_png_budget) throw std::runtime_error("Rendered PNG output budget is exhausted");
    auto output = tasks::detail::SecureFile::create_new(file);
    SecurePngDevice device(output, one_png_budget);
    QImageWriter writer(&device, "PNG");
    if (!writer.write(image)) throw std::runtime_error("Cannot encode the guarded rendered PNG");
    output.flush();
    output.close();
    const auto token = tasks::detail::capture_regular_file(file.parent_path(), file, one_png_budget);
    if (token.size != device.written()) throw std::runtime_error("Rendered PNG identity or size changed during publication");
    impl_->png_bytes += token.size;
    // This exact reference hashes encoded file bytes, never an in-memory pixel plane.
    const auto& request = impl_->paths.request();
    const auto evidence_root = request.contains("evidenceRoot")
        ? foundation_detail::checked_absolute_path(path_from_utf8(request.at("evidenceRoot").get<std::string>()))
        : impl_->paths.options().fixture_root;
    const auto relative = file.lexically_relative(evidence_root);
    if (relative.empty() || relative.is_absolute()) throw std::runtime_error("Rendered PNG has no owned relative path");
    for (const auto& component : relative)
        if (component == "..") throw std::runtime_error("Rendered PNG escaped the fixed evidence root");
    const Json rendered_png{{"path", relative.generic_string()}, {"sha256", token.sha256}, {"bytes", token.size}};
    fields.update(Json{{"method", "production-client-rendered-png"},
        {"rendered", true}, {"presented", false}, {"ready", false}, {"clientOnly", true},
        {"windowIdentity", {{"hwnd", fields.at("hwnd")}, {"rootPid", impl_->identity.at("rootPid")},
            {"rootCreationTime100ns", impl_->identity.at("rootCreationTime100ns")}}},
        {"renderedForPhaseRequestNonce", impl_->identity.at("phaseRequestNonce")},
        {"currentState", impl_->phase_state}, {"stateSha256", impl_->phase_state_sha256},
        {"currentStateSha256", impl_->phase_state_sha256}, {"pixelSize", {image.width(), image.height()}},
        {"widthPixels", image.width()}, {"heightPixels", image.height()}, {"qtDevicePixelRatio", image.devicePixelRatio()},
        {"presentationStatus", "not-presented-evidence"}, {"renderedPNG", rendered_png},
        {"captureBeginMonotonicNs", optional_number(capture_begin_ns)}, {"captureCompleteMonotonicNs", complete_ns}});
    marker("renderedFrame", fields);
    impl_->phase_frame_recorded = true;
}


void PerformanceRecord::input(const InputMeasurement& measurement) {
    if (impl_->active_phase) {
        if (measurement.request_nonce.toStdString() != impl_->active_phase->request.at("phaseRequestNonce").get<std::string>()
            || !measurement.posted_ns || !measurement.dispatched_ns || !measurement.reply_ns
            || static_cast<std::uint64_t>(*measurement.posted_ns) < impl_->active_phase->request.at("phaseBeginNs").get<std::uint64_t>()
            || *measurement.posted_ns > *measurement.dispatched_ns || *measurement.dispatched_ns > *measurement.reply_ns
            || measurement.current_state != impl_->active_phase->request.at("expectedState"))
            throw std::runtime_error("Input reply is not bound to the active fresh acceptance state/phase");
        if (measurement.successful) {
            impl_->phase_reply_ns = measurement.reply_ns;
            impl_->phase_state = measurement.current_state;
            impl_->phase_state_sha256 = measurement.current_state_sha256;
        }
    }
    marker("inputReady", {{"queuedEventMonotonicNs", optional_number(measurement.posted_ns)},
        {"queryMonotonicNs", optional_number(measurement.dispatched_ns)}, {"replyMonotonicNs", optional_number(measurement.reply_ns)},
        {"requestNonce", measurement.request_nonce.toStdString()},
        {"replyRequestNonce", measurement.successful ? Json(measurement.request_nonce.toStdString()) : Json(nullptr)},
        {"replyId", measurement.reply_id}, {"successful", measurement.successful}, {"errorCode", measurement.error_code.toStdString()},
        {"currentState", measurement.current_state}, {"currentStateSha256", measurement.current_state_sha256},
        {"query", "canonicalRuntimeInformation"}, {"method", "ui-queued-query-matching-reply"}});
}

void PerformanceRecord::finish(int exit_code) {
    marker("exit", {{"exitCode", exit_code}, {"eventLoopReturned", true}, {"productionServicesDestroyed", true}});
    impl_->finished = true;
    impl_->output->close();
}

PerformanceProbe::PerformanceProbe(PerformanceApplication& application, ui::UiServices& services, QObject* parent)
    : QObject(parent), application_(application), bindings_(services) {}

void PerformanceProbe::request_input_round_trip(const QString& request_nonce, InputSink sink) {
    if (in_flight_ || !sink || QThread::currentThread() != application_.thread())
        throw std::runtime_error("An input measurement is already active or invalid");
    if (request_nonce.isEmpty() || request_nonce.size() > 128)
        throw std::runtime_error("Invalid input measurement nonce");
    for (const auto character : request_nonce) {
        const auto value = character.unicode();
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9') || value == '-' || value == '_'))
            throw std::runtime_error("Invalid input measurement nonce");
    }
    input_ = {};
    input_.request_nonce = request_nonce;
    input_sink_ = std::move(sink);
    input_.posted_ns = PerformanceApplication::monotonic_nanoseconds();
    in_flight_ = true;
    QCoreApplication::postEvent(this, new QEvent(input_probe_event()));
}

void PerformanceProbe::request_phase_round_trip(const AcceptancePhaseRequest& phase, ui::MainWindow& window, InputSink sink) {
    if (in_flight_ || !sink || QThread::currentThread() != application_.thread())
        throw std::runtime_error("An acceptance phase is already active or invalid");
    const auto& expected = phase.request.at("expectedState");
    const std::set<std::string> required{"route", "windowLogicalSize", "dpiPercent", "fontProfile", "themeId", "mode",
        "backgroundSha256", "blur", "reducedMotion", "fixtureId", "pendingImageCount", "activeTaskCount"};
    if (!expected.is_object() || expected.size() != required.size())
        throw std::runtime_error("This finite phase requires the complete frozen visual state");
    for (const auto& key : required)
        if (!expected.contains(key)) throw std::runtime_error("Frozen visual state has an unknown or missing field");
    const auto route = expected.at("route").get<std::string>();
    if ((route != "home" && route != "more/about") || !expected.at("backgroundSha256").is_null()
        || expected.at("pendingImageCount") != 0 || expected.at("activeTaskCount") != 0)
        throw std::runtime_error("This finite Home/About observer does not attest background/task/image scenarios");
    phase_window_ = &window;
    phase_state_ = expected;
    const auto nonce = QString::fromStdString(phase.request.at("phaseRequestNonce").get<std::string>());
    in_flight_ = true;
    bindings_.query(ui::UiQuery::Onboarding, {}, this, [this, nonce, sink = std::move(sink)](const ui::UiReply& gate) {
        try {
            if (!phase_window_ || gate.error.isError() || !gate.data.toObject().value("startupReady").toBool())
                throw std::runtime_error("Normal onboarding/CLI/portable dispatch has not completed");
            bindings_.query(ui::UiQuery::Settings, {{"section", "app"}}, this,
                [this, nonce, sink](const ui::UiReply& reply) {
                    try {
                        if (!phase_window_ || reply.error.isError() || !reply.data.isObject())
                            throw std::runtime_error("Authoritative appearance settings are unavailable");
                        const auto snapshot = reply.data.toObject();
                        const auto settings = snapshot.value("values").isObject() ? snapshot.value("values").toObject() : snapshot;
                        if (!settings.value("backgroundImage").isNull() && !settings.value("backgroundImage").toString().isEmpty())
                            throw std::runtime_error("A selected background is outside this no-background capture contract");
                        phase_window_->applySettings(settings);
                        const auto size = phase_state_.at("windowLogicalSize");
                        if (!size.is_array() || size.size() != 2 || !size.at(0).is_number_integer() || !size.at(1).is_number_integer()
                            || size.at(0).get<int>() < 900 || size.at(1).get<int>() < 600
                            || size.at(0).get<int>() > 4096 || size.at(1).get<int>() > 4096)
                            throw std::runtime_error("Frozen logical window size exceeds the real production window bounds");
                        phase_window_->resize(size.at(0).get<int>(), size.at(1).get<int>());
                        if (phase_state_.at("route") == "home") phase_window_->navigation().navigate(ui::Route::Home);
                        else {
                            phase_window_->navigation().navigate(ui::Route::More);
                            auto* page = phase_window_->routePage(ui::Route::More);
                            auto* pager = page ? page->findChild<ui::Pager*>() : nullptr;
                            if (!pager) throw std::runtime_error("The actual More page has no production pager");
                            pager->select(QStringLiteral("about"));
                        }
                        bindings_.query(ui::UiQuery::Accounts, {}, this, [this, nonce, sink](const ui::UiReply& accounts) {
                            try {
                                const auto snapshot = accounts.data.toObject();
                                if (accounts.error.isError() || !snapshot.value("items").isArray() || !snapshot.value("items").toArray().isEmpty())
                                    throw std::runtime_error("This finite local-asset frame requires a real empty account snapshot");
                                bindings_.query(ui::UiQuery::Downloads, {}, this, [this, nonce, sink](const ui::UiReply& downloads) {
                                    try {
                                        if (downloads.error.isError() || !downloads.data.toObject().value("items").isArray())
                                            throw std::runtime_error("Current production task state is unavailable");
                                        for (const auto& item : downloads.data.toObject().value("items").toArray()) {
                                            const auto status = item.toObject().value("status").toString();
                                            if (status == "queued" || status == "running" || status == "cancelling" || status == "committing")
                                                throw std::runtime_error("A real active production task excludes the frozen idle capture");
                                        }
                                        // Let the existing 420 ms theme transition finish; never disable its visual effects.
                                        QTimer::singleShot(500, this, [this, nonce, sink] {
                                            try { in_flight_ = false; request_input_round_trip(nonce, sink); }
                                            catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
                                        });
                                    } catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
                                });
                            } catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
                        });
                    } catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
                });
        } catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
    });
}

const InputMeasurement& PerformanceProbe::input_measurement() const { return input_; }

bool PerformanceProbe::event(QEvent* event) {
    if (event->type() != input_probe_event() || !in_flight_) return QObject::event(event);
    try {
        input_.dispatched_ns = PerformanceApplication::monotonic_nanoseconds();
        bindings_.query(ui::UiQuery::RuntimeInformation, {{"requestNonce", input_.request_nonce}}, this,
            [this](const ui::UiReply& reply) {
                try {
                    input_.reply_ns = PerformanceApplication::monotonic_nanoseconds();
                    input_.reply_id = reply.id;
                    input_.successful = !reply.error.isError() && reply.data.isObject()
                        && reply.data.toObject().value("requestNonce").toString() == input_.request_nonce;
                    input_.error_code = reply.error.code;
                    if (input_.successful && !phase_state_.empty()) {
                        if (!phase_window_) throw std::runtime_error("The observed production window was destroyed");
                        input_.current_state = current_visual_state(*phase_window_, phase_state_);
                        input_.successful = input_.current_state == phase_state_;
                        transport::detail::IncrementalHash hash(transport::HashAlgorithm::Sha256);
                        const auto canonical = input_.current_state.dump(-1, ' ', true);
                        hash.update(std::as_bytes(std::span(canonical)));
                        input_.current_state_sha256 = hash.finish();
                    }
                    if (!input_.successful && input_.error_code.isEmpty())
                        input_.error_code = QStringLiteral("invalid-or-unmatched-public-reply-state");
                    in_flight_ = false;
                    input_sink_(input_);
                } catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
            });
    } catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
    return true;
}

} // namespace nickel::app
