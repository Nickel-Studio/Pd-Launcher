#pragma once

#include "installer.hpp"

namespace nickel::minecraft {

struct LoaderBuild {
    LoaderKind kind = LoaderKind::Vanilla;
    std::string minecraftVersion;
    std::string version;
    std::string releaseTime;
    bool stable = true;
    bool recommended = false;
    bool installable = true;
    LoaderArtifact artifact;
};

struct CatalogEndpoints {
    std::string forgeMetadata = "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml";
    std::string forgePromotions = "https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json";
    std::string forgeMaven = "https://maven.minecraftforge.net/net/minecraftforge/forge";
    std::string forgeFiles = "https://files.minecraftforge.net/net/minecraftforge/forge";
    std::string forgeFilesMirror = "https://bmclapi2.bangbang93.com/forge/minecraft";
    std::string neoForgeMaven = "https://maven.neoforged.net/releases/net/neoforged";
    std::string fabric = "https://meta.fabricmc.net/v2/versions";
    std::string legacyFabric = "https://meta.legacyfabric.net/v2/versions";
    std::string quilt = "https://meta.quiltmc.org/v3/versions";
    std::string optiFineIndex = "https://optifine.net/downloads";
    std::string optiFineMirror = "https://bmclapi2.bangbang93.com/optifine/versionList";
    std::string optiFineDownload = "https://bmclapi2.bangbang93.com/optifine";
    std::string liteLoader = "https://dl.liteloader.com/versions/versions.json";
};

class Catalog {
public:
    explicit Catalog(Services services = native_services(), CatalogEndpoints endpoints = {});
    std::vector<LoaderBuild> builds(LoaderKind kind, std::string_view minecraftVersion,
                                    const CancelCheck& check) const;
    LoaderBuild resolve(const LoaderRequest& request, std::string_view minecraftVersion,
                         const CancelCheck& check) const;
private:
    Services services_;
    CatalogEndpoints endpoints_;
};

std::vector<LoaderBuild> parse_optifine_catalog(std::string_view text);
std::vector<LoaderBuild> parse_liteloader_catalog(std::string_view text);
Json liteloader_profile(std::string_view minecraftVersion, const Json& metadata);
Json loader_builds_json(std::span<const LoaderBuild> builds);

}
