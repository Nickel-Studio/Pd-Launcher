#include "fixtures.hpp"

namespace identity_test {
using namespace nickel::media;
using nickel::accounts::IdentityErrorCode;

void run_geometry_tests() {
    const auto geometry = head_geometry(64, 64, 256, ImageKind::skin);
    require(geometry.face_source.x == 8 && geometry.face_source.y == 8 && geometry.face_source.width == 8, "face atlas coordinates");
    require(geometry.face_destination.x == 32 && geometry.face_destination.width == 192, "48/64 face geometry");
    require(geometry.hat_destination->x == 16 && geometry.hat_destination->width == 224, "56/64 hat geometry");
    auto skin = image(64, 64);
    fill(skin, 8, 8, 8, 8, {200, 100, 50, 255});
    auto head = compose_head(skin, 64);
    require(pixel(head, 7, 7)[3] == 0 && pixel(head, 8, 8) == std::array<std::uint8_t,4>{200,100,50,255}, "face transparent margin");
    require(pixel(head, 55, 55)[3] == 255 && pixel(head, 56, 56)[3] == 0, "face exact boundary");
    fill(skin, 40, 8, 8, 8, {20, 220, 40, 128});
    head = compose_head(skin, 64);
    require(pixel(head, 4, 4) == std::array<std::uint8_t,4>{20,220,40,128}, "hat outside face on transparency");
    const auto combined = pixel(head, 32, 32);
    require(combined[0] >= 109 && combined[0] <= 110 && combined[1] >= 160 && combined[1] <= 161 && combined[3] == 255, "unpremultiplied alpha composition");
    require(pixel(head, 3, 3)[3] == 0 && pixel(head, 59, 59)[3] == 128 && pixel(head, 60, 60)[3] == 0, "hat exact margin");

    auto old_skin = image(64, 32, {1,1,1,255});
    fill(old_skin, 8, 8, 8, 8, {33,55,77,255});
    fill(old_skin, 40, 8, 8, 8, {0,0,0,255});
    head = compose_head(old_skin, 64);
    require(pixel(head, 4, 4)[3] == 0 && pixel(head, 32, 32)[0] == 33, "legacy hat skipped");
    auto hd = image(128, 128);
    fill(hd, 16, 16, 16, 16, {11,22,33,255});
    fill(hd, 80, 16, 16, 16, {44,55,66,255});
    require(pixel(compose_head(hd, 64), 4, 4)[0] == 44, "HD hat unit");
    auto hd_old = image(128, 64);
    fill(hd_old, 16, 16, 16, 16, {11,22,33,255});
    fill(hd_old, 80, 16, 16, 16, {44,55,66,255});
    const auto hd_old_head = compose_head(hd_old, 64);
    require(pixel(hd_old_head, 4, 4)[3] == 0 && pixel(hd_old_head, 32,32)[0] == 11, "HD legacy does not become hat");

    auto avatar = image(20, 10, {99,77,55,255});
    fill(avatar, 10, 0, 10, 10, {0,255,0,255});
    head = compose_head(avatar, 64, ImageKind::avatar);
    require(pixel(head, 32, 32)[0] == 99, "avatar top-left minimum square fallback");
    const auto builtin = builtin_head(false, 64);
    require(builtin.width == 64 && builtin.height == 64 && pixel(builtin, 0,0)[3] == 0 && pixel(builtin, 32,32)[3] == 255, "built-in zero-network fallback");

    auto modern = image(64, 64);
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x)
        fill(modern, x, y, 1, 1, {static_cast<std::uint8_t>(x),static_cast<std::uint8_t>(y),77,255});
    auto slim = adapt_skin(modern, false, true, false);
    require(pixel(slim, 47,20) == pixel(modern,48,20), "slim side face reposition");
    require(pixel(slim, 55,20)[3] == 0, "slim unused UV clear");
    require(pixel(slim, 20,20) == pixel(modern,20,20), "slim torso unchanged");
    auto legacy = image(64,32);
    for (unsigned x = 0; x < 64; ++x) fill(legacy,x,20,1,1,{static_cast<std::uint8_t>(x),20,77,255});
    auto modernized = adapt_skin(legacy, false, false, false);
    require(modernized.height == 64 && pixel(modernized,16,52) == pixel(legacy,11,20), "legacy limb face mirror");
    auto exported_old = adapt_skin(modern,false,false,true);
    require(exported_old.height == 32 && exported_old.pixels.size() == 64*32*4, "legacy real height");
    expect_error(IdentityErrorCode::invalid_input, [&] { adapt_skin(legacy,true,false,false); });
    expect_error(IdentityErrorCode::unsupported_image, [&] { adapt_skin(avatar,false,false,false); });
    ImageLimits small;
    small.max_pixels = 4096;
    expect_error(IdentityErrorCode::image_too_large, [&] { compose_head(skin,256,ImageKind::skin,small); });
    skin.pixels.pop_back();
    expect_error(IdentityErrorCode::unsupported_image, [&] { compose_head(skin,64); });
    std::cout << "geometry: face/hat/alpha/HD/legacy/fallback/slim/limb mirror/budgets passed\n";
}

} // namespace identity_test
