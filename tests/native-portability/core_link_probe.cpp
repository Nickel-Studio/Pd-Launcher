#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"
#include "core/network/http.hpp"
#include "core/network/url_policy.hpp"
#include "core/archive/archive.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        nickel::transport::HttpClient client;
        if (client.snapshot().user_agent.empty()) throw std::runtime_error("real HTTP client initialization failed");
        if (!nickel::transport::detail::loopback_address("127.0.0.1") ||
            nickel::transport::detail::public_address("127.0.0.1")) {
            throw std::runtime_error("native address classification failed");
        }
        if (nickel::archive::Archive::validate_relative_path("owned/path.txt") != "owned/path.txt") {
            throw std::runtime_error("real archive policy failed");
        }
        std::cout << "{\"status\":\"PASS\",\"scope\":\"All actual std-core owner libraries whole-archive linked; basic initialization only; no full launcher/network release claim\"}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
