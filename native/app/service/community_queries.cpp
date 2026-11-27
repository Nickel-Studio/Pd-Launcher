#include "app/application_state.hpp"

#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/documents/qt_body.hpp"
#include "core/resources/utility.hpp"

namespace nickel::app {
namespace {

resources::Filter filter_from_scope(ApplicationState& state, const Json& scope) {
    resources::Filter filter;
    filter.minecraftVersion = bounded_text(scope, "gameVersion", 128, false);
    filter.loader = bounded_text(scope, "loader", 64, false);
    if (filter.minecraftVersion.empty() && scope.contains("root") && scope.contains("versionId")
        && !scope.value("root", std::string{}).empty() && !scope.value("versionId", std::string{}).empty()) {
        const auto current = state.context(state.identity(scope));
        filter.minecraftVersion = current.minecraftVersion;
        filter.loader = resources::defaultSearchLoader(current.loader);
    }
    return filter;
}

std::optional<resources::Dependency> dependency_from_scope(const Json& scope) {
    const auto found = scope.find("dependency");
    if (found == scope.end() || found->is_null() || (found->is_object() && found->empty())) return {};
    if (!found->is_object()) fail_static("invalid_argument");
    return resources::Dependency{bounded_text(*found, "projectId", 128, false),
        bounded_text(*found, "versionId", 128, false), bounded_text(*found, "fileName", 512, false)};
}

} // namespace

OperationResult ApplicationState::query_community(ui::UiQuery query, const Json& scope,
                                                  const Cancellation& cancellation) {
    OperationResult result;
    cancellation.check();
    if (query == ui::UiQuery::CommunitySearch) {
        resources::SearchQuery request;
        request.kind = selected_kind(scope);
        request.text = bounded_text(scope, "query", 4096, false);
        request.filter = filter_from_scope(*this, scope);
        const auto source = resources::lower(bounded_text(scope, "provider", 32, false));
        if (source.empty() || source == "all") request.sources = resources::SourceSelection::All;
        else if (source == "modrinth") request.sources = resources::SourceSelection::Modrinth;
        else if (source == "curseforge") request.sources = resources::SourceSelection::CurseForge;
        else fail_static("invalid_argument");
        request.pageSize = bounded_unsigned(scope, "limit", 50, 100);
        if (!request.pageSize) fail_static("invalid_argument");
        const auto offset = bounded_unsigned(scope, "offset", 0, 100000);
        if (offset % request.pageSize) fail_static("invalid_argument");
        request.page = offset / request.pageSize + 1;
        request.forceRefresh = scope.value("refresh", false);
        const auto page = community->search(request, cancellation);
        Json rows = Json::array();
        for (const auto& project : page.projects) {
            rows.push_back(project_dto(project));
            std::lock_guard lock(state_mutex);
            if (projects.size() >= 256) projects.erase(projects.begin());
            projects.insert_or_assign(resources::projectIdentity(project), project);
        }
        Json issues = Json::array();
        for (const auto& [provider, error] : page.sourceErrors)
            issues.push_back(resources::sourceName(provider) + ": " + safe_public_message(error.code));
        result.data = {{"items", rows}, {"total", page.total ? Json(*page.total) : Json(nullptr)},
                       {"hasMore", page.hasMore}, {"nextOffset", offset + request.pageSize},
                       {"revision", page.revision}, {"issues", issues}};
        return result;
    }
    const auto source = selected_source(scope);
    auto project_id = bounded_text(scope, "projectId", 128);
    if (const auto dependency = dependency_from_scope(scope)) {
        const auto resolved = community->resolveDependency(source, *dependency, cancellation);
        project_id = resolved.project.id;
    }
    const auto project = community->project(source, project_id, cancellation);
    if (scope.contains("kind") && selected_kind(scope) != project.kind) fail_static("invalid_argument");
    {
        std::lock_guard lock(state_mutex);
        if (projects.size() >= 256) projects.erase(projects.begin());
        projects.insert_or_assign(resources::projectIdentity(project), project);
    }
    if (query == ui::UiQuery::ProjectDetail) {
        const auto description = community->fullDescription(source, project.id, cancellation);
        auto body = body_reader.convert(description, cancellation);
        result.data = project_dto(project);
        result.data["body"] = own_description(std::move(body));
        if (!project.iconUrl.empty()) {
            try {
                resources::HttpRequest request;
                request.url = resources::checkedUrl(project.iconUrl, resources::descriptionImageRules());
                request.rules = resources::descriptionImageRules();
                request.maximumBytes = 2 * 1024 * 1024;
                request.followRedirects = true;
                request.validationScope = "project-icon:" + resources::projectIdentity(project);
                request.validate = [](const auto& response) {
                    if (response.status != 200) fail_static("network");
                };
                const auto response = cached_http.fetch(std::move(request), {}, cancellation);
                const auto image = ui::decodeDescriptionImage(response.response->bytes, {}, cancellation);
                result.media.emplace_back("project:" + resources::projectIdentity(project), ui::imageFromRaster(image));
            } catch (const ErrorException&) {
                cancellation.check();
            }
        }
        return result;
    }
    if (query == ui::UiQuery::ProjectVersions) {
        const auto filter = filter_from_scope(*this, scope);
        const auto offset = bounded_unsigned(scope, "offset", 0, 100000);
        const auto limit = bounded_unsigned(scope, "limit", 100, 100);
        if (!limit) fail_static("invalid_argument");
        if (const auto dependency = dependency_from_scope(scope); dependency && !dependency->versionId.empty()) {
            const auto resolved = community->resolveDependency(source, *dependency, cancellation);
            if (!resolved.exactVersion) fail_static("integrity");
            result.data = {{"items", Json::array({version_dto(*resolved.exactVersion)})},
                           {"hasMore", false}, {"nextOffset", 1}, {"pinned", true}};
            return result;
        }
        const auto page = community->versions(source, project.kind, project.id, filter, offset, limit, cancellation);
        Json rows = Json::array();
        for (const auto& version : page.versions) rows.push_back(version_dto(version));
        result.data = {{"items", rows}, {"hasMore", page.cursor.hasMore},
                       {"nextOffset", page.cursor.nextOffset}, {"revision", page.revision},
                       {"total", page.cursor.total ? Json(*page.cursor.total) : Json(nullptr)}};
        return result;
    }
    if (query == ui::UiQuery::RequiredDependencies) {
        const auto version = community->exactVersion(source, project.id,
            bounded_text(scope, "projectVersionId", 128), cancellation);
        if (!version.requiredDependencies) fail_static("integrity");
        Json rows = Json::array();
        Json issues = Json::array();
        for (const auto& dependency : resources::deduplicateDependencies(project, version)) {
            cancellation.check();
            try {
                const auto resolved = community->resolveDependency(source, dependency, cancellation);
                auto row = project_dto(resolved.project);
                row["dependency"] = {{"projectId", dependency.projectId}, {"versionId", dependency.versionId},
                                     {"fileName", dependency.fileName}};
                row["pinned"] = resolved.exactVersion.has_value();
                if (resolved.exactVersion) row["exactVersion"] = version_dto(*resolved.exactVersion);
                rows.push_back(std::move(row));
            } catch (const ErrorException& error) {
                cancellation.check();
                issues.push_back(safe_public_message(error.error().code));
            }
        }
        result.data = {{"items", rows}, {"issues", issues}};
        return result;
    }
    fail_static("invalid_argument");
}

} // namespace nickel::app
