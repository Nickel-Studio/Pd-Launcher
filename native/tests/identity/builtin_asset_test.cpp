#include "fixtures.hpp"

namespace identity_test {
namespace {
std::string rgba_md5(const nickel::media::RgbaImage& image) {
    const auto digest = nickel::accounts::detail::md5({
        reinterpret_cast<const char*>(image.pixels.data()), image.pixels.size()});
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
}

void run_builtin_asset_tests() {
    using namespace nickel::media;
    const auto steve = builtin_head_asset(false);
    const auto alex = builtin_head_asset(true);
    require(steve.width == 256 && steve.height == 256 && alex.width == 256 && alex.height == 256,
            "original functional head dimensions");
    require(rgba_md5(steve) == "0a79c885b1e1d9b245ba9e37cca785a4", "Steve exact original decoded RGBA pixels");
    require(rgba_md5(alex) == "dfa52ef346d2da7fe1d1291a1e39135e", "Alex exact original decoded RGBA pixels");
    for (const auto size : {32u, 64u, 160u, 256u}) {
        require(builtin_head(false, size) == compose_head(steve, size, ImageKind::avatar),
                "Steve original asset shared margin and sampling");
        require(builtin_head(true, size) == compose_head(alex, size, ImageKind::avatar),
                "Alex original asset shared margin and sampling");
    }
    ImageLimits small;
    small.max_decoded_bytes = 256 * 256 * 4 - 1;
    expect_error(nickel::accounts::IdentityErrorCode::image_too_large, [&] { builtin_head_asset(false, small); });
    std::cout << "assets: Steve/Alex original PNG RGBA pixel parity and bounded fallback passed\n";
}
} // namespace identity_test
