#include "app/application_state.hpp"

#include "app/service/public_dto.hpp"
#include "app/service/trusted_launcher.hpp"
#include "core/packs/native_services.hpp"

namespace nickel::app {

packs::ExportServices ApplicationState::export_services(bool modrinth_only,
                                                        const Cancellation& cancellation) const {
    cancellation.check();
    if (stopping.load()) fail_static("cancelled");
    packs::ExportServices trusted;
    if (trusted_launcher_) trusted = trusted_launcher_->export_fields(cancellation);
    auto result = packs::native_export_services(community, modrinth_only, std::move(trusted));
    if (trusted_launcher_) result.archive = trusted_launcher_->protect_archive(std::move(result.archive));
    return result;
}

} // namespace nickel::app
