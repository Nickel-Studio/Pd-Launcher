#include "app/service/trusted_launcher.hpp"

#include "core/common/error.hpp"

#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    using nickel::app::TrustedLauncherReceipt;
    static_assert(!std::is_default_constructible_v<TrustedLauncherReceipt>);
    static_assert(!std::is_copy_constructible_v<TrustedLauncherReceipt>);
    static_assert(!std::is_constructible_v<TrustedLauncherReceipt, std::filesystem::path>);
    try {
        require(!nickel::app::capture_trusted_launcher(), "A non-production executable minted trust");
        std::cout << "non-production process image / no compiled release capability: PASS\n";
        nickel::Cancellation cancelled;
        cancelled.cancel();
        bool refused = false;
        try {
            (void)nickel::app::capture_trusted_launcher(cancelled);
        } catch (const nickel::ErrorException& error) {
            refused = error.error().code == "cancelled";
        }
        require(refused, "Pre-cancelled capture did not preserve its typed cancellation");
        std::cout << "cancel before capture / typed cancelled / no capability: PASS\n";
        require(!nickel::app::capture_trusted_launcher(), "Cancelled capture poisoned a later capture");
        std::cout << "fresh capture remains non-production / no runtime trust injection: PASS\n";
        std::cout << "trusted-launcher primitive negatives: 3 PASS; actual production export: NOT_RUN\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "trusted-launcher primitive negative failed: " << error.what() << '\n';
        return 1;
    }
}
