#include "platform/memory.hpp"

#include <algorithm>
#include <charconv>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <fstream>
#endif

namespace nickel {

PhysicalMemory parse_linux_meminfo(std::string_view text) noexcept {
    auto field = [text](std::string_view name) -> std::optional<std::uint64_t> {
        auto remaining = text;
        while (!remaining.empty()) {
            const auto end = remaining.find('\n');
            auto line = remaining.substr(0, end);
            if (line.starts_with(name)) {
                line.remove_prefix(name.size());
                const auto first = line.find_first_not_of(" \t");
                if (first == std::string_view::npos) return std::nullopt;
                line.remove_prefix(first);
                std::uint64_t value = 0;
                const auto parsed = std::from_chars(line.data(), line.data() + line.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr == line.data()) return std::nullopt;
                auto unit = std::string_view(parsed.ptr, line.data() + line.size() - parsed.ptr);
                const auto unit_start = unit.find_first_not_of(" \t\r");
                if (unit_start == std::string_view::npos || !unit.substr(unit_start).starts_with("kB")) {
                    return std::nullopt;
                }
                return value / 1024;
            }
            if (end == std::string_view::npos) break;
            remaining.remove_prefix(end + 1);
        }
        return std::nullopt;
    };
    PhysicalMemory result{field("MemTotal:"), field("MemAvailable:")};
    if (result.total_mb && result.available_mb) {
        result.available_mb = std::min(*result.available_mb, *result.total_mb);
    }
    return result;
}

PhysicalMemory physical_memory() noexcept {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return {};
    return {status.ullTotalPhys / 1024 / 1024, status.ullAvailPhys / 1024 / 1024};
#elif defined(__APPLE__)
    std::uint64_t total = 0;
    std::size_t size = sizeof(total);
    PhysicalMemory result;
    if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0) result.total_mb = total / 1024 / 1024;
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const auto host = mach_host_self();
    vm_size_t page_size = 0;
    if (host_page_size(host, &page_size) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) == KERN_SUCCESS) {
        const auto pages = static_cast<std::uint64_t>(stats.free_count) + stats.inactive_count + stats.speculative_count;
        result.available_mb = pages * page_size / 1024 / 1024;
    }
    mach_port_deallocate(mach_task_self(), host);
    if (result.total_mb && result.available_mb) result.available_mb = std::min(*result.total_mb, *result.available_mb);
    return result;
#else
    try {
        std::ifstream file("/proc/meminfo", std::ios::binary);
        if (!file) return {};
        std::string text(64 * 1024, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(file.gcount()));
        return parse_linux_meminfo(text);
    } catch (...) { return {}; }
#endif
}

} // namespace nickel
