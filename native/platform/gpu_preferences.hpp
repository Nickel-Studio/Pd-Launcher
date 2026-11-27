#pragma once

#include <filesystem>
#include <memory>

namespace nickel {

enum class GpuPreferenceRestore { Restored, ExternalChange, Inactive };

// Platform adapter availability, not a claim that a discrete GPU was selected.
bool high_performance_gpu_supported() noexcept;

// Owns one process-local lease for the selected Java's temporary Windows preference.
// Call restore at the owned root's actual exit, not at pipe EOF or session destruction.
class GpuPreferenceGuard {
public:
    static GpuPreferenceGuard acquire(const std::filesystem::path& selected_java);
    ~GpuPreferenceGuard();
    GpuPreferenceGuard(GpuPreferenceGuard&&) noexcept;
    GpuPreferenceGuard& operator=(GpuPreferenceGuard&&) noexcept;
    GpuPreferenceGuard(const GpuPreferenceGuard&) = delete;
    GpuPreferenceGuard& operator=(const GpuPreferenceGuard&) = delete;

    const std::filesystem::path& java_executable() const noexcept;
    // Restores only while the current type and bytes equal our applied value.
    // External changes/deletion are preserved; this is not registry-atomic CAS.
    GpuPreferenceRestore restore();

private:
    struct Impl;
    explicit GpuPreferenceGuard(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace nickel
