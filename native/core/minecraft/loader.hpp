#pragma once

#include "catalog.hpp"

namespace nickel::minecraft {

struct LoaderPlan {
    LoaderSelection selection;
    Json profile;
    std::vector<DownloadFile> downloads;
    std::vector<WriteFile> writes;
    std::vector<CopyFile> copies;
    std::vector<ArchiveFile> archiveFiles;
    std::vector<ProcessorPlan> processors;
    std::vector<std::filesystem::path> directories;
    unsigned minimumJava = 0;
};

LoaderPlan plan_loader(const LoaderBuild& build, const InstallRequest& request,
                        const ResolvedVersion& vanilla, std::span<const LoaderSelection> companions,
                        const Services& services, const CancelCheck& check,
                        const ProgressCallback& progress = {});
std::string resolve_processor_argument(std::string_view argument, const Json& data,
                                       const std::filesystem::path& loaderRoot,
                                       const std::filesystem::path& installer,
                                       const ResolvedVersion& vanilla);
Json optifine_launchwrapper_profile(std::string_view minecraftVersion, std::string_view filename);

}
