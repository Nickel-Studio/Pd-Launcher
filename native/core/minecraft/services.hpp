#pragma once

#include "metadata.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <stop_token>

namespace nickel { class OwnedProcess; }

namespace nickel::minecraft {

struct Progress {
    std::string stage;
    std::string message;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::uint64_t downloadedBytes = 0;
    std::uint64_t totalBytes = 0;
};

using ProgressCallback = std::function<void(const Progress&)>;
using OutputCallback = std::function<void(bool standardError, std::string_view)>;
using CancelCheck = std::function<void()>;

enum class ProcessKind { Preparation, Game, Service };

struct ProcessSpec {
    std::filesystem::path executable;
    std::filesystem::path workingDirectory;
    std::vector<std::string> arguments;
    std::map<std::string, std::string, std::less<>> environment;
    std::vector<std::string> unsetEnvironment;
    std::optional<std::string> windowsShellCommand;
    ProcessKind purpose = ProcessKind::Preparation;
    std::uint64_t outputLimit = 256 * 1024;
    bool hideWindow = true;
    bool terminateOnDrop = false;
    bool highPerformanceGpu = false;
    std::function<void(const Error&)> gpuPreferenceWarning;
};

class OwnedProcess {
public:
    virtual ~OwnedProcess() = default;
    virtual std::uint64_t pid() const noexcept = 0;
    // Borrow the existing native owner; synthetic/custom services do not expose one.
    virtual const nickel::OwnedProcess* native_process() const noexcept { return nullptr; }
    virtual bool running() const = 0;
    virtual int wait(const CancelCheck& check, bool terminateOnCancellation,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) = 0;
    virtual void terminate_owned() = 0;
    // True only after this child is joined and both owned output streams reached EOF.
    virtual bool drain_owned(std::chrono::milliseconds timeout) = 0;
};

struct Services {
    std::function<std::string(std::string_view url, std::uint64_t maximumBytes,
                              const CancelCheck& check)> fetch;
    std::function<void(const DownloadFile&, const CancelCheck&, const ProgressCallback&)> download;
    std::function<bool(const std::filesystem::path&, const FileHashes&,
                       std::optional<std::uint64_t>, const CancelCheck&)> verify;
    std::function<std::vector<std::string>(const std::filesystem::path&, const CancelCheck&)> archiveEntries;
    std::function<std::string(const std::filesystem::path&, std::string_view entry,
                              std::uint64_t maximumBytes, const CancelCheck&)> archiveRead;
    std::function<void(const NativeArchive&, const CancelCheck&)> extractNatives;
    std::function<void(const std::filesystem::path&, const std::filesystem::path&,
                       unsigned stripComponents, const CancelCheck&)> extractRuntime;
    std::function<std::shared_ptr<OwnedProcess>(const ProcessSpec&, const OutputCallback&)> spawn;
    std::function<void(const std::filesystem::path&, std::string_view, const CancelCheck&)> atomicWrite;
    std::function<void(const Cancellation&)> cancellationCheck;
    std::function<std::vector<JavaCandidate>(const CancelCheck&)> discoverJava;
    std::function<void(const std::filesystem::path&, const CancelCheck&)> validateLaunchable;
    // One immutable network snapshot and typed cancellation per operation.
    std::function<Services(const Cancellation&)> forOperation;
};

void configure_native_services(Services services);
Services native_services();
CancelCheck cancellation_check(const Services& services, const Cancellation& cancellation,
                                std::stop_token extra = {});

}
