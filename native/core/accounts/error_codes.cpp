#include "accounts.hpp"

namespace nickel::accounts {
std::string_view identity_error_code_name(IdentityErrorCode code) noexcept {
    switch (code) {
    case IdentityErrorCode::invalid_input: return "identity.invalid_input";
    case IdentityErrorCode::invalid_server: return "identity.invalid_server";
    case IdentityErrorCode::invalid_response: return "identity.invalid_response";
    case IdentityErrorCode::network: return "identity.network";
    case IdentityErrorCode::storage: return "identity.storage";
    case IdentityErrorCode::vault: return "identity.vault";
    case IdentityErrorCode::missing_account: return "identity.missing_account";
    case IdentityErrorCode::missing_credentials: return "identity.missing_credentials";
    case IdentityErrorCode::unauthorized: return "identity.unauthorized";
    case IdentityErrorCode::declined: return "identity.declined";
    case IdentityErrorCode::expired: return "identity.expired";
    case IdentityErrorCode::cancelled: return "cancelled";
    case IdentityErrorCode::busy: return "identity.busy";
    case IdentityErrorCode::stale_commit: return "identity.stale_commit";
    case IdentityErrorCode::no_entitlement: return "identity.no_entitlement";
    case IdentityErrorCode::no_profile: return "identity.no_profile";
    case IdentityErrorCode::invalid_profile: return "identity.invalid_profile";
    case IdentityErrorCode::xbox_account_missing: return "identity.xbox_account_missing";
    case IdentityErrorCode::xbox_child_account: return "identity.xbox_child_account";
    case IdentityErrorCode::xbox_region: return "identity.xbox_region";
    case IdentityErrorCode::xbox_banned: return "identity.xbox_banned";
    case IdentityErrorCode::xbox_verification: return "identity.xbox_verification";
    case IdentityErrorCode::quota: return "identity.quota";
    case IdentityErrorCode::unsupported_image: return "media.unsupported_image";
    case IdentityErrorCode::image_too_large: return "media.image_too_large";
    case IdentityErrorCode::untrusted_download: return "media.untrusted_download";
    }
    return "identity.failure";
}
} // namespace nickel::accounts
