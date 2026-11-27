#pragma once

#include "installer.hpp"

namespace nickel::minecraft {

Json fetch_metadata(const Services& services, std::string_view url, const CancelCheck& check,
                     std::uint64_t maximumBytes = 16 * 1024 * 1024);
void ensure_file(const Services& services, const DownloadFile& file, const CancelCheck& check,
                 const ProgressCallback& progress = {}, bool offline = false);
void write_managed(const Services& services, const WriteFile& file, const CancelCheck& check);
void copy_managed(const Services& services, const CopyFile& file, const CancelCheck& check);
JavaCandidate probe_java(const Services& services, const std::filesystem::path& executable,
                          const CancelCheck& check);
RuntimePlan plan_java_runtime(const Services& services, const InstallRequest& request,
                              const JavaRequirement& requirement, const Json& metadata,
                              const CancelCheck& check);
void execute_java_runtime(const Services& services, const RuntimePlan& runtime,
                           const CancelCheck& check, const ProgressCallback& progress,
                           bool offline);

}
