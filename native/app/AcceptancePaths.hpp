#pragma once

#include "core/config/path_guard.hpp"
#include "core/config/paths.hpp"
#include "core/tasks/secure_fs.hpp"

#include <QStringList>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nickel::app {

struct ProductionArguments {
    std::optional<std::filesystem::path> acceptance_context;
    bool performance = false;
    std::vector<std::string> business_arguments;
};

[[nodiscard]] ProductionArguments parse_production_arguments(const QStringList& arguments);
[[nodiscard]] bool has_acceptance_arguments(const QStringList& arguments);

struct AcceptancePathOptions {
    std::filesystem::path fixture_root;
    std::filesystem::path executable;
    std::filesystem::path config_root;
    std::filesystem::path game_root;
    std::filesystem::path cache_root;
    std::filesystem::path temporary_root;
    std::string executable_sha256;
};

struct AcceptancePhaseRequest {
    Json request = Json::object();
    std::string context_sha256;
};

// Test-only path root: maps every production path into one fixture namespace and verifies that
// the process environment, files and directories cannot escape it.
class AcceptancePaths final {
public:
    explicit AcceptancePaths(AcceptancePathOptions options);
    [[nodiscard]] static AcceptancePaths from_context(const std::filesystem::path& file,
                                                     const std::filesystem::path& executable);
    [[nodiscard]] const PathContext& context() const;
    [[nodiscard]] const AcceptancePathOptions& options() const;
    [[nodiscard]] const Json& request() const;
    [[nodiscard]] const std::filesystem::path& context_file() const;
    [[nodiscard]] std::optional<AcceptancePhaseRequest> phase_request(std::uint64_t root_pid,
        std::uint64_t root_creation_time_100ns, std::int64_t now_ns) const;
    void configure_process_environment() const;
    void verify() const;

private:
    void verify_context_namespace() const;
    AcceptancePathOptions options_;
    PathContext context_;
    Json request_ = Json::object();
    std::filesystem::path context_file_;
    std::optional<tasks::detail::RegularFileToken> context_token_;
    mutable std::optional<AcceptancePhaseRequest> ready_phase_;
    std::vector<foundation_detail::DirectoryGuard> directories_;
    tasks::detail::RegularFileToken executable_;
};

} // namespace nickel::app
