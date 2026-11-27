#include "mods.hpp"
#include "utility.hpp"
#include "core/tasks/tasks.hpp"

#include <algorithm>
#include <exception>
#include <type_traits>

namespace nickel::resources {

ModUpdateResult ModsService::updateConfirmed(const InstanceIdentity& instance, const std::vector<ModUpdate>& updates,
                                             tasks::Task& operation, const ProgressCallback& progress) {
    static_assert(std::is_nothrow_move_constructible_v<ModUpdateReceipt>);
    static_assert(std::is_nothrow_move_assignable_v<Error>);
    ModUpdateResult result;
    std::shared_ptr<void> lease;
    std::vector<std::pair<std::string, std::string>> steps;
    const auto& cancel = operation.cancellation();
    // Allocate diagnostics before any publication, not while reporting its outcome.
    Error pendingError{"persistence_pending", "Mod publication completed; durability or its completion marker is pending"};
    Error fallbackError{"io", "Mod update operation failed"};
    const auto readPrefix = [&]() noexcept {
        if (steps.empty()) return;
        try {
            const auto committed = operation.committed_files();
            std::vector<ModUpdateReceipt> actual;
            actual.reserve(committed.size());
            for (const auto& file : committed) {
                const auto selected = std::find_if(steps.begin(), steps.end(),
                    [&](const auto& step) { return step.first == file.step_id; });
                if (selected == steps.end()) continue;
                actual.push_back({operation.id(), selected->second, file.published.destination,
                    file.step_id, file.published.sha256, file.final, file.durability_pending});
            }
            result.receipts.swap(actual);
            result.finalCommitted = !result.receipts.empty() && result.receipts.back().final;
            result.durabilityPending = std::any_of(result.receipts.begin(), result.receipts.end(),
                [](const auto& receipt) { return receipt.durabilityPending; });
        } catch (...) {
            // Querying copies receipts; allocation failure must not erase the recorded prefix.
        }
    };
    try {
        cancel.check();
        if (operation.snapshot().status != tasks::TaskStatus::Running || !operation.committed_files().empty())
            fail("conflict", "Mod update requires an unpublished running caller-owned Task");
        if (!services_.replaceInTask || !services_.prepareInTask || !services_.hashes || !community_)
            fail("unsupported", "Caller-owned Mod update transactions are unavailable");
        const auto selected = context(instance);
        if (selected.updatesDisabled || selected.running)
            fail("policy_denied", "Mod updates are disabled or the instance is running");
        validateConfirmedUpdates(selected, updates);
        const auto total = static_cast<std::size_t>(std::count_if(updates.begin(), updates.end(),
            [](const ModUpdate& update) { return update.available.has_value(); }));
        if (!total) fail("not_found", "No compatible updates were confirmed for the selected Mods");
        result.receipts.reserve(total);
        steps.reserve(total);
        for (const auto& update : updates) {
            if (update.available) steps.emplace_back("mod-" + std::to_string(steps.size()), update.fileName);
        }
        const auto settings = community_->settings();
        const std::function<DownloadPlan(const DownloadPlan&)> prepare = [&](const DownloadPlan& approved) {
            return services_.prepareInTask(approved, settings, operation);
        };
        lease = acquireConfirmedMutation(selected);
        operation.files(0, total);
        for (const auto& update : updates) {
            if (!update.available) continue;
            cancel.check();
            const auto index = result.receipts.size();
            const auto plan = prepareConfirmedUpdate(selected, update, cancel, prepare);
            auto receipt = services_.replaceInTask(selected, update.fileName, update.originalSha256,
                plan, lease, steps[index].first, operation, index + 1 == total, progress);
            // Reserved storage plus noexcept move records the real publication before reporting.
            result.receipts.push_back(std::move(receipt));
            result.finalCommitted = result.receipts.back().final;
            result.durabilityPending = result.receipts.back().durabilityPending;
            if (result.durabilityPending && !result.finalCommitted) result.error = std::move(pendingError);
            if (result.finalCommitted || result.durabilityPending) {
                if (progress) try { progress({"committed", result.receipts.size(), total}); } catch (...) { }
                return result;
            }
            operation.files(result.receipts.size(), total);
            if (progress) progress({"committed", result.receipts.size(), total});
        }
    } catch (const ErrorException& failure) {
        try { result.error = failure.error(); }
        catch (...) { result.error = std::move(fallbackError); }
    } catch (...) {
        result.error = std::move(fallbackError);
    }
    readPrefix();
    if (result.finalCommitted) {
        result.error.reset();
        return result;
    }
    if (result.durabilityPending) {
        result.error = std::move(pendingError);
        return result;
    }
    if (result.error) {
        operation.rollback(result.error->code, result.error->message);
        result.cancelled = operation.snapshot().status == tasks::TaskStatus::Cancelled;
    }
    return result;
}

} // namespace nickel::resources
