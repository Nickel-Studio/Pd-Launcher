#include "core/archive/zip_index.hpp"
#include "core/tasks/secure_fs.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: zip_index_test expected_accept zip\n"; return 2; }
    const bool expected = std::string(argv[1]) == "accept";
    try {
        auto file = nickel::tasks::detail::SecureFile::open_read(nickel::tasks::detail::utf8_path(argv[2]));
        const auto entries = nickel::archive::detail::inspect_zip(file, nickel::archive::Limits{}, {});
        if (!expected) { std::cerr << "FAIL unsafe ZIP accepted\n"; return 1; }
        std::cout << "PASS accept entries=" << entries.size() << '\n'; return 0;
    } catch (const std::exception& error) {
        if (expected) { std::cerr << "FAIL safe ZIP rejected: " << error.what() << '\n'; return 1; }
        std::cout << "PASS reject: " << error.what() << '\n'; return 0;
    }
}
