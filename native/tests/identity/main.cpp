#include "fixtures.hpp"

namespace identity_test {
void run_geometry_tests();
void run_skin_png_tests();
void run_offline_skin_tests();
}

int main() {
    try {
        identity_test::run_accounts_tests();
        identity_test::run_account_safety_tests();
        identity_test::run_geometry_tests();
        identity_test::run_builtin_asset_tests();
        identity_test::run_skin_png_tests();
        identity_test::run_offline_skin_tests();
        identity_test::run_auth_tests();
        identity_test::run_common_tests();
        identity_test::run_lifecycle_tests();
#ifdef NICKEL_IDENTITY_FULL_TESTS
        identity_test::run_media_tests();
        identity_test::run_media_lifecycle_tests();
        identity_test::run_media_publication_tests();
#endif
        std::cout << "identity controlled tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "identity controlled test failure: " << error.what() << '\n';
        return 1;
    }
}
