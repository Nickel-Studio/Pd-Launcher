// External, finite WGC observer. It never creates a window or calls DwmFlush.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <tlhelp32.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <QBuffer>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSet>
#include <QStorageInfo>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
constexpr qint64 MaximumJsonBytes = 64 * 1024;
constexpr qint64 MaximumPngBytes = 4 * 1024 * 1024;
constexpr qint64 MaximumWorkingBytes = 64 * 1024 * 1024;
constexpr qint64 MaximumPixels = 8 * 1024 * 1024;

[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
void require(bool condition, const char* message) { if (!condition) fail(message); }
const wchar_t* wide(const QString& value) { return reinterpret_cast<const wchar_t*>(value.utf16()); }
QString sha(const QByteArray& raw) { return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex()); }
bool digest(const QString& value) {
    return value.size() == 64 && std::all_of(value.cbegin(), value.cend(), [](QChar c) {
        return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f');
    });
}
class UniqueJsonKeys final {
public:
    explicit UniqueJsonKeys(const QByteArray& bytes) : bytes_(bytes) {}
    void verify() { value(0); space(); require(position_ == bytes_.size(), "Trailing context JSON bytes"); }
private:
    void space() { while (position_ < bytes_.size() && (bytes_[position_] == ' ' || bytes_[position_] == '\n' || bytes_[position_] == '\r' || bytes_[position_] == '\t')) ++position_; }
    bool take(char token) { space(); if (position_ < bytes_.size() && bytes_[position_] == token) { ++position_; return true; } return false; }
    QString string() {
        space(); const auto begin = position_;
        require(take('"'), "JSON object key must be a string");
        while (position_ < bytes_.size()) {
            const auto token = bytes_[position_++];
            if (token == '\\') { require(position_ < bytes_.size(), "Truncated JSON escape"); ++position_; }
            else if (token == '"') {
                QJsonParseError error{};
                const auto parsed = QJsonDocument::fromJson('[' + bytes_.mid(begin, position_ - begin) + ']', &error);
                require(error.error == QJsonParseError::NoError && parsed.isArray() && parsed.array().size() == 1
                    && parsed.array().at(0).isString(), "Invalid UTF-8 JSON string");
                return parsed.array().at(0).toString();
            }
        }
        fail("Truncated JSON string");
    }
    void value(int depth) {
        require(depth <= 32, "Context JSON nesting exceeds its finite bound"); space();
        if (take('{')) {
            QSet<QString> keys;
            if (take('}')) return;
            do { const auto key = string(); require(!keys.contains(key), "Duplicate JSON context key"); keys.insert(key);
                require(take(':'), "Missing JSON object colon"); value(depth + 1);
            } while (take(','));
            require(take('}'), "Incomplete JSON object");
        } else if (take('[')) {
            if (take(']')) return;
            do { value(depth + 1); } while (take(',')); require(take(']'), "Incomplete JSON array");
        } else if (position_ < bytes_.size() && bytes_[position_] == '"') { (void)string(); }
        else { const auto begin = position_; while (position_ < bytes_.size() && bytes_[position_] != ','
            && bytes_[position_] != '}' && bytes_[position_] != ']' && bytes_[position_] != ' '
            && bytes_[position_] != '\n' && bytes_[position_] != '\r' && bytes_[position_] != '\t') ++position_;
            require(position_ > begin, "Missing JSON value"); }
    }
    const QByteArray& bytes_;
    qsizetype position_ = 0;
};
qint64 qpcNs() {
    LARGE_INTEGER ticks{}, frequency{};
    require(QueryPerformanceCounter(&ticks) && QueryPerformanceFrequency(&frequency)
        && ticks.QuadPart > 0 && frequency.QuadPart > 0 && frequency.QuadPart <= 1000000000,
        "QPC unavailable or unsupported frequency");
    const qint64 seconds = ticks.QuadPart / frequency.QuadPart;
    require(seconds < std::numeric_limits<qint64>::max() / 1000000000, "QPC epoch exceeds nanosecond range");
    return seconds * 1000000000 + (ticks.QuadPart % frequency.QuadPart) * 1000000000 / frequency.QuadPart;
}
qint64 positive(const QJsonObject& object, const char* name) {
    const auto value = object.value(QLatin1StringView(name));
    const qint64 result = value.toInteger(-1);
    require(value.isDouble() && result > 0 && value.toDouble() == double(result), "Missing positive integer identity/time");
    return result;
}
qint64 argumentInteger(const QCommandLineParser& parser, const char* name) {
    bool okay = false;
    const auto result = parser.value(QLatin1StringView(name)).toLongLong(&okay, 0);
    require(okay && result > 0, "Invalid positive integer argument");
    return result;
}
void noRedirect(const QString& target, bool directory = false) {
    const auto attributes = GetFileAttributesW(wide(target));
    require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Missing or redirected filesystem material");
    if (directory) require((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)),
                           "Capture fixture must be an uncompressed, nonsparse directory");
    else {
        const auto handle = CreateFileW(wide(target), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "Cannot pin capture input identity");
        const auto close = qScopeGuard([handle] { CloseHandle(handle); });
        BY_HANDLE_FILE_INFORMATION info{};
        require(GetFileInformationByHandle(handle, &info) && info.nNumberOfLinks == 1
            && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "Capture input cannot be a hardlink or directory");
    }
}
QString guarded(const QString& root, const QString& relative, bool output = false) {
    require(!relative.isEmpty() && !QDir::isAbsolutePath(relative) && !relative.contains(u':') && !relative.contains(u'\\'),
            "Exact forward-slash relative evidence path is required");
    const auto parts = relative.split(u'/', Qt::KeepEmptyParts);
    QString cursor = root;
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const auto& part = parts[index];
        require(!part.isEmpty() && part != u"." && part != u".." && !part.endsWith(u'.') && !part.endsWith(u' '),
                "Ambiguous or traversing evidence path");
        cursor = QDir(cursor).filePath(part);
        if (output && index == parts.size() - 1) require(!QFileInfo::exists(cursor), "Refusing to overwrite capture evidence");
        else noRedirect(cursor, index < parts.size() - 1);
    }
    return cursor;
}
QByteArray boundedRead(const QString& path, qint64 maximum) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= maximum, "Bounded input file unavailable");
    const auto expected = file.size();
    const auto raw = file.read(maximum + 1);
    require(raw.size() == expected && file.size() == expected, "Input bytes changed or exceeded input bound");
    return raw;
}
QString fileSha(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() > 0, "Executable material is unavailable");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Executable hash could not be read");
    return QString::fromLatin1(hash.result().toHex());
}
QJsonObject reference(const QString& relative, const QByteArray& raw) {
    return {{"path", relative}, {"sha256", sha(raw)}, {"bytes", qint64(raw.size())}};
}
void writeNew(const QString& path, const QByteArray& raw) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Capture output must not overwrite existing evidence");
    require(file.write(raw) == raw.size() && file.flush(), "Capture output write failed");
    file.close();
    require(boundedRead(path, std::max(MaximumJsonBytes, qint64(raw.size()))) == raw, "Capture output bytes changed");
}
struct KernelIdentity {
    DWORD pid{};
    quint64 birth{};
    QString executable;
};
KernelIdentity identity(HANDLE process, DWORD pid) {
    require(WaitForSingleObject(process, 0) == WAIT_TIMEOUT, "Observed process is no longer alive");
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(GetProcessTimes(process, &creation, &exit, &kernel, &user), "Kernel process birth is unavailable");
    wchar_t path[32768]{}; DWORD size = DWORD(std::size(path));
    require(QueryFullProcessImageNameW(process, 0, path, &size) && size, "Kernel executable identity is unavailable");
    return {pid, (quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime, QString::fromWCharArray(path, int(size))};
}
DWORD parentPid(DWORD pid) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    require(snapshot != INVALID_HANDLE_VALUE, "Observer ancestry snapshot failed");
    const auto close = qScopeGuard([snapshot] { CloseHandle(snapshot); });
    PROCESSENTRY32W row{}; row.dwSize = sizeof(row);
    if (Process32FirstW(snapshot, &row)) do {
        if (row.th32ProcessID == pid) return row.th32ParentProcessID;
    } while (Process32NextW(snapshot, &row));
    fail("Observer ancestry was not found");
}
QString objectName(HANDLE handle) {
    wchar_t name[256]{}; DWORD needed = 0;
    require(handle && GetUserObjectInformationW(handle, UOI_NAME, name, sizeof(name), &needed), "Desktop object name unavailable");
    return QString::fromWCharArray(name);
}
QString interactiveDesktop() {
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    require(input != nullptr, "No accessible interactive input desktop");
    const auto close = qScopeGuard([input] { CloseDesktop(input); });
    const auto current = GetThreadDesktop(GetCurrentThreadId());
    const auto name = objectName(current);
    require(name == objectName(input), "Observer is not on the actual interactive input desktop");
    USEROBJECTFLAGS flags{}; DWORD needed = 0;
    require(GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), &needed)
            && (flags.dwFlags & WSF_VISIBLE), "Window station is not interactive/visible");
    return name;
}
struct Geometry {
    RECT client{};
    RECT frame{};
    POINT clientOrigin{};
    UINT dpi{};
    UINT desktopDpi{};
    int monitorScale{};
    int awareness{};
    QString frameBasis;
};
bool equalRect(const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
Geometry geometry(HWND window, DWORD pid, const winrt::Windows::Graphics::SizeInt32& size) {
    DWORD owner = 0; DWORD cloaked = 0;
    require(IsWindow(window) && GetWindowThreadProcessId(window, &owner) && owner == pid
            && IsWindowVisible(window) && !IsIconic(window), "HWND is not a live visible nonminimized launcher-owned window");
    require(SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && !cloaked,
            "Launcher HWND is cloaked or its cloak state is unavailable");
    Geometry result;
    const auto awareness = GetWindowDpiAwarenessContext(window);
    result.awareness = int(GetAwarenessFromDpiAwarenessContext(awareness));
    require(result.awareness == int(DPI_AWARENESS_PER_MONITOR_AWARE)
            && AreDpiAwarenessContextsEqual(awareness, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
            "DPI-unaware/system virtualized windows are not actual per-monitor-v2 OS-DPI evidence");
    result.dpi = GetDpiForWindow(window);
    result.desktopDpi = GetDpiForSystem();
    require(result.dpi && result.desktopDpi && GetClientRect(window, &result.client) && ClientToScreen(window, &result.clientOrigin),
            "Physical client geometry or actual desktop/window DPI unavailable");
    const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    DEVICE_SCALE_FACTOR scale{};
    require(monitor && SUCCEEDED(GetScaleFactorForMonitor(monitor, &scale)), "Actual monitor display scale unavailable");
    result.monitorScale = int(scale);
    RECT extended{};
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &extended, sizeof(extended)))
            && extended.right - extended.left == size.Width && extended.bottom - extended.top == size.Height) {
        result.frame = extended; result.frameBasis = QStringLiteral("DwmGetWindowAttribute-extended-frame-bounds");
    } else {
        require(GetWindowRect(window, &result.frame) && result.frame.right - result.frame.left == size.Width
                && result.frame.bottom - result.frame.top == size.Height, "WGC content/frame geometry is ambiguous; no inferred crop");
        result.frameBasis = QStringLiteral("GetWindowRect-per-monitor-v2");
    }
    return result;
}
QJsonArray rectangle(const RECT& value) { return {value.left, value.top, value.right, value.bottom}; }
QJsonArray pixelSize(const QImage& value) { return {value.width(), value.height()}; }
QString rgbaSha(const QImage& image) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y) hash.addData(QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(y)), image.width() * 4));
    return QString::fromLatin1(hash.result().toHex());
}
bool variedVisible(const QImage& image) {
    bool seen = false; quint32 first = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const auto* p = row + x * 4;
            if (!p[3] || !(p[0] || p[1] || p[2])) continue;
            const quint32 color = quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16);
            if (seen && color != first) return true;
            first = color; seen = true;
        }
    }
    return false;
}
QImage readRendered(const QByteArray& raw, int width, int height) {
    require(raw.size() >= 33 && raw.first(8) == QByteArray::fromHex("89504e470d0a1a0a"), "Rendered input is not a PNG");
    QBuffer input; input.setData(raw); require(input.open(QIODevice::ReadOnly), "Rendered PNG input unavailable");
    QImageReader reader(&input, "png");
    require(reader.size() == QSize(width, height) && reader.imageCount() == 1, "Rendered PNG dimensions/animation differ from scenario");
    const auto image = reader.read().convertToFormat(QImage::Format_RGBA8888);
    require(!image.isNull() && image.size() == QSize(width, height) && variedVisible(image), "Rendered PNG is empty, black, transparent or uniform");
    return image;
}
QImage copyClient(const winrt::com_ptr<ID3D11Device>& device, const winrt::com_ptr<ID3D11DeviceContext>& context,
                  const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& frame, const Geometry& geometry,
                  int width, int height, qint64 deadline) {
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
    const auto content = frame.ContentSize();
    require(description.Format == DXGI_FORMAT_B8G8R8A8_UNORM && description.SampleDesc.Count == 1
            && content.Width > 0 && content.Height > 0 && UINT(content.Width) <= description.Width && UINT(content.Height) <= description.Height,
            "WGC content/texture format or bounds are invalid");
    const int left = geometry.clientOrigin.x - geometry.frame.left;
    const int top = geometry.clientOrigin.y - geometry.frame.top;
    require(left >= 0 && top >= 0 && left <= content.Width - width && top <= content.Height - height,
            "Physical client crop exceeds WGC ContentSize; undefined texture bytes are forbidden");
    D3D11_TEXTURE2D_DESC staging{};
    staging.Width = UINT(width); staging.Height = UINT(height); staging.MipLevels = 1; staging.ArraySize = 1;
    staging.Format = DXGI_FORMAT_B8G8R8A8_UNORM; staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> readable;
    winrt::check_hresult(device->CreateTexture2D(&staging, nullptr, readable.put()));
    const D3D11_BOX crop{UINT(left), UINT(top), 0, UINT(left + width), UINT(top + height), 1};
    context->CopySubresourceRegion(readable.get(), 0, 0, 0, 0, texture.get(), 0, &crop);
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE map{};
    for (;;) {
        require(qpcNs() <= deadline, "WGC staging readback exceeded the finite capture deadline");
        const auto status = context->Map(readable.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
        if (status != DXGI_ERROR_WAS_STILL_DRAWING) { winrt::check_hresult(status); break; }
        Sleep(1);
    }
    const auto unmap = qScopeGuard([&] { context->Unmap(readable.get(), 0); });
    require(map.pData && map.RowPitch >= UINT(width * 4), "WGC staging texture has no complete client rows");
    QImage premultiplied(width, height, QImage::Format_ARGB32_Premultiplied);
    require(!premultiplied.isNull(), "WGC image allocation failed");
    for (int y = 0; y < height; ++y) std::memcpy(premultiplied.scanLine(y), static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch, size_t(width) * 4);
    return premultiplied.convertToFormat(QImage::Format_RGBA8888);
}

int capture(const QCommandLineParser& parser) {
    const QString root = QFileInfo(parser.value("root")).canonicalFilePath();
    require(!root.isEmpty() && QDir(root).isAbsolute(), "Evidence root must be an existing absolute directory");
    noRedirect(root, true);
    const auto contextName = parser.value("context");
    const auto contextPath = guarded(root, contextName);
    const auto contextBytes = boundedRead(contextPath, MaximumJsonBytes);
    require(digest(parser.value("context-sha256")) && sha(contextBytes) == parser.value("context-sha256"), "Context bytes differ from driver binding");
    UniqueJsonKeys(contextBytes).verify();
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(contextBytes, &parse);
    require(parse.error == QJsonParseError::NoError && document.isObject(), "Context must be a received JSON object");
    const auto input = document.object();
    require(input.value("protocol").toString() == QStringLiteral("nickel-evidence-driver-v1")
            && input.value("role").toString() == QStringLiteral("native")
            && input.value("observationAdapter").toString() == QStringLiteral("native-production-markers-v1")
            && (input.value("phase").toString() == QStringLiteral("ready") || input.value("phase").toString() == QStringLiteral("state")),
            "Only existing native ready/state observation contexts are accepted");
    for (const char* name : {"sourceSnapshotSha256", "buildSourceSha256", "executableSha256", "driverSourceSha256", "inputCorpusSha256"})
        require(digest(input.value(QLatin1StringView(name)).toString()), "Missing complete source/executable/driver/corpus hash");
    for (const char* name : {"runId", "scenarioId", "requestNonce", "phaseRequestNonce", "sourceCommit"})
        require(!input.value(QLatin1StringView(name)).toString().isEmpty(), "Missing finite launch/phase identity");
    require(digest(parser.value("state-sha256")), "Canonical scenario SHA-256 is required");
    const auto state = input.value("expectedState").toObject();
    const auto logical = state.value("windowLogicalSize").toArray();
    require(logical.size() == 2 && logical[0].toInteger() > 0 && logical[1].toInteger() > 0
            && logical[0].toInteger() <= 16384 && logical[1].toInteger() <= 16384,
            "Frozen logical geometry is missing or exceeds finite dimensions");
    const qint64 percent = positive(state, "dpiPercent");
    require(percent == 100 || percent == 125, "This finite capture entry supports actual Windows 100/125 percent only");
    const qint64 width64 = (logical[0].toInteger() * percent + 50) / 100;
    const qint64 height64 = (logical[1].toInteger() * percent + 50) / 100;
    require(width64 > 0 && height64 > 0 && width64 <= MaximumPixels / height64
            && width64 <= std::numeric_limits<int>::max() && height64 <= std::numeric_limits<int>::max(), "Scenario exceeds capture plane budget");
    const int width = int(width64), height = int(height64);
    const auto renderedName = parser.value("rendered-frame");
    const auto renderedPath = guarded(root, renderedName);
    const auto renderedBytes = boundedRead(renderedPath, MaximumPngBytes);
    require(digest(parser.value("rendered-sha256")) && sha(renderedBytes) == parser.value("rendered-sha256"), "Rendered PNG bytes differ from the phase binding");
    QImageReader::setAllocationLimit(64);
    const auto rendered = readRendered(renderedBytes, width, height);
    const auto renderedPixelsSha = rgbaSha(rendered);
    const auto frameName = parser.value("frame"), outputName = parser.value("output");
    const auto framePath = guarded(root, frameName, true), outputPath = guarded(root, outputName, true);
    const QString fixture = QFileInfo(input.value("fixtureRoot").toString()).canonicalFilePath();
    require(!fixture.isEmpty() && fixture != root && fixture.startsWith(root + u'/', Qt::CaseInsensitive), "Actual fixture root is not a run-owned child of evidence root");
    require(!input.contains("evidenceRoot") || QFileInfo(input.value("evidenceRoot").toString()).canonicalFilePath() == root,
        "The capture root differs from the immutable producer evidence root");
    noRedirect(fixture, true);
    require(framePath.startsWith(fixture + u'/', Qt::CaseInsensitive) && outputPath.startsWith(fixture + u'/', Qt::CaseInsensitive),
            "WGC PNG/raw output must stay within the accepted run fixture");
    QStorageInfo storage(fixture);
    require(storage.isValid() && storage.isReady() && storage.fileSystemType().toUpper() == "NTFS"
            && storage.bytesAvailable() >= 256 * 1024 * 1024, "Actual capture fixture must have uncompressed NTFS capacity");
    const qint64 pid64 = positive(input, "rootPid"), birth = positive(input, "rootCreationTime100ns");
    require(pid64 <= std::numeric_limits<DWORD>::max(), "Launcher PID exceeds DWORD range");
    const DWORD pid = DWORD(pid64);
    const auto hwnd64 = argumentInteger(parser, "hwnd");
    require(quint64(hwnd64) <= std::numeric_limits<quintptr>::max(), "HWND exceeds pointer width");
    const auto window = reinterpret_cast<HWND>(quintptr(hwnd64));
    const auto driver64 = argumentInteger(parser, "capture-root-pid");
    require(driver64 <= std::numeric_limits<DWORD>::max() && DWORD(driver64) != pid
            && parentPid(GetCurrentProcessId()) == DWORD(driver64), "Capture root must be this helper's external driver parent, never launcher/child subtraction");
    DWORD ancestor = DWORD(driver64);
    for (int depth = 0; ancestor && depth < 64; ++depth) {
        require(ancestor != pid && ancestor != GetCurrentProcessId(), "Capture observer must not be a launcher descendant");
        ancestor = parentPid(ancestor);
        require(depth < 63 || !ancestor, "Capture observer ancestry exceeds finite depth");
    }
    const HANDLE driver = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(driver64));
    require(driver != nullptr, "External capture driver kernel identity is unavailable");
    const auto closeDriver = qScopeGuard([driver] { CloseHandle(driver); });
    const auto driverIdentity = identity(driver, DWORD(driver64));
    const auto captureIdentity = identity(GetCurrentProcess(), GetCurrentProcessId());
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    require(process != nullptr, "Launcher kernel process is unavailable");
    const auto close = qScopeGuard([process] { CloseHandle(process); });
    const auto actual = identity(process, pid);
    const auto executablePath = guarded(root, parser.value("executable"));
    require(actual.birth == quint64(birth) && QFileInfo(actual.executable).canonicalFilePath().compare(QFileInfo(executablePath).canonicalFilePath(), Qt::CaseInsensitive) == 0
            && fileSha(executablePath) == input.value("executableSha256").toString(), "Live executable/kernel birth differs from this generation");
    const qint64 phaseBegin = positive(input, "phaseBeginNs");
    const qint64 notBefore = argumentInteger(parser, "not-before-ns");
    const qint64 start = qpcNs();
    require(positive(input, "launchBeginNs") <= phaseBegin && phaseBegin <= notBefore && notBefore <= start,
            "Render/phase timestamps are stale, future or not shared QPC nanoseconds");
    const qint64 timeout = argumentInteger(parser, "timeout-ms");
    require(timeout <= 10000 && start <= std::numeric_limits<qint64>::max() - timeout * 1000000,
            "WGC observer timeout exceeds finite ten-second bound");
    const qint64 deadline = start + timeout * 1000000;
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    require(previousDpi != nullptr, "Physical window observations require per-monitor-v2 thread context");
    const auto restoreDpi = qScopeGuard([previousDpi] { SetThreadDpiAwarenessContext(previousDpi); });
    const auto desktop = interactiveDesktop();
    require(winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported(), "Actual Windows Graphics Capture is unavailable");
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> deviceContext;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, deviceContext.put()));
    const auto dxgiDevice = device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    const auto runtimeDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    const auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
    const auto initialSize = item.Size();
    require(initialSize.Width > 0 && initialSize.Height > 0 && initialSize.Width <= MaximumPixels / initialSize.Height,
            "WGC item exceeds bounded physical content dimensions");
    require(qint64(initialSize.Width) * initialSize.Height * 8 + width64 * height64 * 20
            + renderedBytes.size() + contextBytes.size() + MaximumPngBytes <= MaximumWorkingBytes,
            "WGC pool/staging/image planes exceed declared 64 MiB working-plane budget");
    const auto before = geometry(window, pid, initialSize);
    require(before.dpi * 100 == percent * 96 && before.desktopDpi * 100 == percent * 96 && before.monitorScale == percent
            && before.client.right - before.client.left == width && before.client.bottom - before.client.top == height,
            "Actual desktop/monitor/window DPI or physical client does not match requested scenario");
    auto pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(runtimeDevice,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, initialSize);
    auto session = pool.CreateCaptureSession(item);
    bool captureClosed = false;
    const auto closeCapture = qScopeGuard([&] {
        if (!captureClosed) { try { session.Close(); } catch (...) {} try { pool.Close(); } catch (...) {} }
    });
    const bool cursorProperty = winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled");
    if (cursorProperty) session.IsCursorCaptureEnabled(false);
    session.StartCapture();
    QImage captured;
    qint64 ticks100ns = 0, frameNs = 0, completion = 0;
    int frames = 0; bool equal = false;
    while (qpcNs() <= deadline && frames < 128) {
        auto frame = pool.TryGetNextFrame();
        if (!frame) { Sleep(5); continue; }
        const auto returnFrame = qScopeGuard([&] { try { frame.Close(); } catch (...) {} });
        ++frames;
        const qint64 time100ns = frame.SystemRelativeTime().count();
        require(time100ns > 0 && time100ns <= std::numeric_limits<qint64>::max() / 100, "WGC compositor QPC timestamp is invalid");
        if (time100ns * 100 < start) continue;
        const auto size = frame.ContentSize();
        require(size.Width == initialSize.Width && size.Height == initialSize.Height, "Live WGC content resized during phase capture");
        const auto current = geometry(window, pid, size);
        require(equalRect(current.client, before.client) && equalRect(current.frame, before.frame)
                && current.clientOrigin.x == before.clientOrigin.x && current.clientOrigin.y == before.clientOrigin.y
                && current.dpi == before.dpi && current.desktopDpi == before.desktopDpi && current.monitorScale == before.monitorScale,
                "Live window/desktop/monitor geometry changed during phase capture");
        captured = copyClient(device, deviceContext, frame, current, width, height, deadline);
        ticks100ns = time100ns; frameNs = time100ns * 100; completion = qpcNs();
        require(notBefore <= start && start <= frameNs && frameNs <= completion && completion <= deadline,
                "Compositor frame is future, late or on a different QPC epoch");
        equal = captured.size() == rendered.size();
        for (int y = 0; equal && y < height; ++y) equal = std::memcmp(captured.constScanLine(y), rendered.constScanLine(y), size_t(width) * 4) == 0;
        if (equal && variedVisible(captured)) break;
    }
    session.Close(); pool.Close(); captureClosed = true;
    require(!captured.isNull() && frames > 0 && variedVisible(captured), "No fresh nonblack/nontransparent compositor client frame arrived");
    const auto after = geometry(window, pid, initialSize);
    require(equalRect(after.client, before.client) && equalRect(after.frame, before.frame)
            && after.clientOrigin.x == before.clientOrigin.x && after.clientOrigin.y == before.clientOrigin.y
            && after.dpi == before.dpi && after.desktopDpi == before.desktopDpi && after.monitorScale == before.monitorScale,
            "Live window/DPI changed after WGC capture");
    require(identity(process, pid).birth == actual.birth && identity(driver, DWORD(driver64)).birth == driverIdentity.birth
            && fileSha(executablePath) == input.value("executableSha256").toString(), "Launcher/driver/executable identity changed during WGC capture");
    require(boundedRead(contextPath, MaximumJsonBytes) == contextBytes && boundedRead(renderedPath, MaximumPngBytes) == renderedBytes,
            "Context/rendered PNG bytes changed during WGC capture");
    QByteArray png;
    QBuffer output(&png); require(output.open(QIODevice::WriteOnly), "PNG encoding buffer unavailable");
    QImageWriter writer(&output, "png"); require(writer.write(captured) && !png.isEmpty() && png.size() <= MaximumPngBytes, "Compositor PNG exceeds encoded-byte budget");
    writeNew(framePath, png);
    QJsonObject raw;
    for (const char* name : {"protocol", "role", "observationAdapter", "runId", "scenarioId", "requestNonce", "phaseRequestNonce", "phase", "sourceCommit",
                            "sourceSnapshotSha256", "buildSourceSha256", "executableSha256", "driverSourceSha256", "inputCorpusSha256", "rootPid", "rootCreationTime100ns", "phaseBeginNs", "launchBeginNs"})
        raw.insert(QLatin1StringView(name), input.value(QLatin1StringView(name)));
    raw.insert("schemaVersion", 1); raw.insert("origin", "actual-windows-graphics-capture-client-frame");
    raw.insert("method", "external-WGC-presented"); raw.insert("clock", "perf_counter_ns"); raw.insert("platform", "windows");
    raw.insert("sourceBindingsOrigin", "runner-context-claims-not-build-attestation");
    raw.insert("nonce", input.value("phaseRequestNonce"));
    raw.insert("windowIdentity", QJsonObject{{"hwnd", hwnd64}, {"rootPid", pid64}, {"rootCreationTime100ns", birth}});
    raw.insert("captureRootPid", driver64); raw.insert("capturePid", qint64(GetCurrentProcessId()));
    raw.insert("observerRootPid", driver64); raw.insert("observerRootCreationTime100ns", qint64(driverIdentity.birth));
    raw.insert("captureRootIdentity", QJsonObject{{"pid", driver64}, {"creationTime100ns", qint64(driverIdentity.birth)}, {"executable", driverIdentity.executable}});
    raw.insert("captureProcessIdentity", QJsonObject{{"pid", qint64(GetCurrentProcessId())}, {"creationTime100ns", qint64(captureIdentity.birth)}, {"executable", captureIdentity.executable}});
    raw.insert("captureParentPid", qint64(parentPid(GetCurrentProcessId()))); raw.insert("launcherDescendantExcluded", true);
    raw.insert("systemRelativeTime100ns", ticks100ns); raw.insert("frameMonotonicNs", frameNs); raw.insert("capturedMonotonicNs", completion);
    raw.insert("captureBeginNs", start); raw.insert("renderedMonotonicNs", notBefore);
    raw.insert("state", state); raw.insert("stateSha256", parser.value("state-sha256"));
    raw.insert("pixelSize", pixelSize(captured)); raw.insert("clientOnly", true);
    raw.insert("frameEvidence", reference(frameName, png)); raw.insert("renderedFrameEvidence", reference(renderedName, renderedBytes));
    raw.insert("contextEvidence", reference(contextName, contextBytes));
    raw.insert("windowsDpi", int(before.dpi)); raw.insert("desktopDpi", int(before.desktopDpi));
    raw.insert("monitorScalePercent", before.monitorScale); raw.insert("windowDpiAwareness", before.awareness); raw.insert("windowPerMonitorV2", true);
    raw.insert("clientPhysicalSize", pixelSize(captured)); raw.insert("windowFramePhysicalBounds", rectangle(before.frame));
    raw.insert("clientScreenPhysicalOrigin", QJsonArray{before.clientOrigin.x, before.clientOrigin.y}); raw.insert("frameBoundsMethod", before.frameBasis);
    raw.insert("desktop", desktop); raw.insert("cursorCaptureEnabled", !cursorProperty); raw.insert("captureBorderSuppressed", false);
    raw.insert("pixelFormat", "DXGI_FORMAT_B8G8R8A8_UNORM-to-unpremultiplied-RGBA8");
    raw.insert("rgbaSha256", rgbaSha(captured)); raw.insert("renderedRgbaSha256", renderedPixelsSha); raw.insert("pixelsEqual", equal);
    raw.insert("nonblank", true); raw.insert("framesInspected", frames); raw.insert("maximumWorkingPlaneBytes", MaximumWorkingBytes);
    raw.insert("limitations", QJsonArray{"Compositor-rendered client evidence, not physical monitor photons.",
        "Declared working-plane accounting is not total driver/GPU/plugin/process peak memory.", "No system display scale or launcher state is changed by this observer."});
    writeNew(outputPath, QJsonDocument(raw).toJson(QJsonDocument::Indented));
    require(SetThreadDpiAwarenessContext(previousDpi) != nullptr, "Observer thread DPI awareness could not be restored");
    std::printf("%s\n", QJsonDocument(QJsonObject{{"scope", "actual-compositor-client-frame-only"}, {"status", equal ? "CAPTURED_MATCHED_RENDERED_PIXELS" : "CAPTURED_PIXELS_DIFFER"},
        {"frameEvidence", reference(frameName, png)}, {"rawEvidence", reference(outputName, boundedRead(outputPath, MaximumJsonBytes))}, {"productionVisualAcceptance", "NOT_ASSESSED"}}).toJson(QJsonDocument::Compact).constData());
    return equal ? 0 : 2;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("pd-production-capture-observer"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Finite external WGC capture of one explicitly owned production HWND; not a second UI."));
    parser.addHelpOption();
    for (const auto* name : {"root", "context", "context-sha256", "executable", "hwnd", "capture-root-pid", "rendered-frame", "rendered-sha256", "state-sha256", "not-before-ns", "frame", "output"})
        parser.addOption(QCommandLineOption(QString::fromLatin1(name), QString::fromLatin1(name), QStringLiteral("value")));
    parser.addOption(QCommandLineOption(QStringLiteral("timeout-ms"), QStringLiteral("Finite timeout, at most 10000 milliseconds"), QStringLiteral("value"), QStringLiteral("5000")));
    parser.process(app);
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto uninitialize = qScopeGuard([] { winrt::uninit_apartment(); });
        return capture(parser);
    } catch (const winrt::hresult_error& error) {
        const auto message = QString::fromWCharArray(error.message().c_str());
        std::fprintf(stderr, "%s\n", QJsonDocument(QJsonObject{{"status", "BLOCK"}, {"origin", "external-WGC-capture-attempt"},
            {"error", message}, {"hresult", qint64(error.code().value)}, {"actualCompositorFrameObserved", false}}).toJson(QJsonDocument::Compact).constData());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", QJsonDocument(QJsonObject{{"status", "BLOCK"}, {"origin", "external-WGC-capture-attempt"},
            {"error", QString::fromUtf8(error.what())}, {"actualCompositorFrameObserved", false}}).toJson(QJsonDocument::Compact).constData());
    }
    return 1;
}
