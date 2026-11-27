#include "images.hpp"
#include "../accounts/detail/protocol.hpp"
#include "../accounts/detail/util.hpp"
#include "builtin_heads.hpp"
#include "skin_png.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>

namespace nickel::media {
namespace {
using accounts::IdentityError;
using accounts::IdentityErrorCode;
using accounts::IdentityHttpRequest;
using accounts::IdentityHttpResponse;
using accounts::detail::Json;
using namespace accounts::detail;

std::string sniff(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t,8> png{0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a};
    if (bytes.size() >= png.size() && std::equal(png.begin(),png.end(),bytes.begin())) return "image/png";
    if (bytes.size() >= 12 && std::equal(bytes.begin(),bytes.begin()+4,"RIFF") && std::equal(bytes.begin()+8,bytes.begin()+12,"WEBP")) return "image/webp";
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) return "image/jpeg";
    if (bytes.size() >= 6 && (std::equal(bytes.begin(),bytes.begin()+6,"GIF87a") || std::equal(bytes.begin(),bytes.begin()+6,"GIF89a"))) return "image/gif";
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return "image/bmp";
    return {};
}

void validate_probe(const ImageProbe& probe, const ImageLimits& limits) {
    const auto pixels = static_cast<std::uint64_t>(probe.width) * probe.height;
    if (!probe.width || !probe.height || probe.width > limits.max_dimension || probe.height > limits.max_dimension ||
        pixels > limits.max_pixels || pixels > limits.max_decoded_bytes / 4 || pixels > std::numeric_limits<std::size_t>::max() / 4)
        throw IdentityError(IdentityErrorCode::image_too_large);
    if (probe.frames != 1 || (probe.mime != "image/png" && probe.mime != "image/webp" && probe.mime != "image/jpeg" &&
                             probe.mime != "image/gif" && probe.mime != "image/bmp") ||
        (probe.decoded_working_bytes && probe.decoded_working_bytes < static_cast<std::size_t>(pixels) * 4))
        throw IdentityError(IdentityErrorCode::unsupported_image);
}

bool loopback(const Url& url) {
    return url.host == "127.0.0.1" || url.host == "[::1]" || url.host == "localhost";
}

std::string controlled_origin(std::string_view raw) {
    const auto url = parse_url(raw);
    if (url.scheme != "https" && !loopback(url)) throw IdentityError(IdentityErrorCode::untrusted_download);
    if (!url.path.empty() && url.path != "/") throw IdentityError(IdentityErrorCode::untrusted_download);
    return url.origin();
}

std::string texture_url(std::string_view raw, const std::set<std::string>& trusted) {
    auto url = parse_url(raw, true);
    // Mojang's historical texture URLs use HTTP; upgrade only this fixed host.
    if (url.host == "textures.minecraft.net" && url.scheme == "http" && url.port == 80) {
        url.scheme = "https";
        url.port = 443;
    }
    if (!trusted.contains(url.origin())) throw IdentityError(IdentityErrorCode::untrusted_download);
    if (url.scheme != "https" && !loopback(url)) throw IdentityError(IdentityErrorCode::untrusted_download);
    if (url.host == "textures.minecraft.net") {
        if (!url.path.starts_with("/texture/") || !url.query.empty()) throw IdentityError(IdentityErrorCode::untrusted_download);
        const auto hash = std::string_view(url.path).substr(9);
        if (hash.size() < 32 || hash.size() > 128 || !std::all_of(hash.begin(),hash.end(),[](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            })) throw IdentityError(IdentityErrorCode::untrusted_download);
    }
    return url.str();
}

std::string resolve_redirect(std::string_view current, std::string_view location) {
    if (location.empty() || location.size() > 4096) throw IdentityError(IdentityErrorCode::untrusted_download);
    const auto base = parse_url(current,true);
    if (location.find("://") != std::string_view::npos) return parse_url(location,true).str();
    if (location.starts_with("//")) return parse_url(base.scheme + ":" + std::string(location),true).str();
    if (location.front() == '/') return parse_url(base.origin() + std::string(location),true).str();
    const auto slash = base.path.rfind('/');
    return parse_url(base.origin() + (slash == std::string::npos ? "/" : base.path.substr(0,slash+1)) + std::string(location),true).str();
}

std::optional<std::string> texture_property(const Json& profile) {
    const auto properties = profile.find("properties");
    if (properties == profile.end() || !properties->is_array() || properties->size() > 64) return std::nullopt;
    for (const auto& property : *properties) {
        if (string_field(property,"name",false,64) != "textures") continue;
        const auto encoded = string_field(property,"value",true,24*1024);
        const auto decoded = base64_decode(encoded,16*1024);
        const auto texture = parse_json({reinterpret_cast<const char*>(decoded.data()),decoded.size()},16*1024,16);
        const auto textures = texture.find("textures");
        if (textures == texture.end() || !textures->is_object()) continue;
        const auto skin = textures->find("SKIN");
        if (skin == textures->end() || !skin->is_object()) continue;
        const auto url = string_field(*skin,"url",true,4096);
        return url;
    }
    return std::nullopt;
}

void validate_request(const SkinRequest& request) {
    const auto valid = [](std::string_view text, std::size_t limit) {
        return text.size() <= limit && valid_utf8(text);
    };
    if (!valid(request.account_id,256) || !valid(request.name,256) ||
        (request.uuid && !valid(*request.uuid,64)) || (request.server && !valid(*request.server,4096)) ||
        request.trusted_texture_origins.size() > 16)
        throw IdentityError(IdentityErrorCode::invalid_input);
    switch (request.source) {
    case accounts::AccountType::offline:
    case accounts::AccountType::microsoft: break;
    case accounts::AccountType::littleskin: accounts::validate_server(request.server); break;
    case accounts::AccountType::yggdrasil:
        if (!request.server) throw IdentityError(IdentityErrorCode::invalid_server);
        parse_url(*request.server);
        break;
    default: throw IdentityError(IdentityErrorCode::invalid_input);
    }
    if (request.source != accounts::AccountType::offline && request.uuid && !request.uuid->empty() &&
        !valid_profile_id(accounts::normalize_uuid(*request.uuid)))
        throw IdentityError(IdentityErrorCode::invalid_input);
    for (const auto& origin : request.trusted_texture_origins) controlled_origin(origin);
}

std::string cache_key(const SkinRequest& request) {
    return Json::array({request.account_id,accounts::account_type_name(request.source),request.name,
        request.uuid.value_or(""),request.server.value_or(""),request.output_size,request.revision,
        request.trusted_texture_origins}).dump();
}
}

struct ImageService::Impl {
    struct Request {
        std::string account_id;
        nickel::Cancellation cancellation;
        std::uint64_t id = 0;
        std::uint64_t account_epoch = 0;
        std::uint64_t global_epoch = 0;
        bool active = true;
    };
    struct CacheEntry {
        std::string account_id;
        ImageResult result;
        std::chrono::steady_clock::time_point expires;
        std::uint64_t recency = 0;
        std::size_t charge = 0;
    };
    std::shared_ptr<accounts::IdentityHttp> http;
    ImageDecoder& decoder;
    ImageLimits limits;
    accounts::IdentityLog log;
    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::unordered_map<std::string,CacheEntry> cache;
    std::unordered_map<std::uint64_t,std::shared_ptr<Request>> requests;
    std::unordered_map<std::string,std::uint64_t> epochs;
    std::uint64_t global_epoch = 0;
    std::uint64_t sequence = 0;
    std::size_t cache_bytes = 0;
    std::size_t active_decodes = 0;
    std::size_t decode_bytes = 0;
    std::size_t active_bytes = 0;
    std::size_t active_fetches = 0;

    Impl(std::shared_ptr<accounts::IdentityHttp> http_, ImageDecoder& decoder_, ImageLimits limits_, accounts::IdentityLog log_)
        : http(std::move(http_)),decoder(decoder_),limits(limits_),log(std::move(log_)) {
        if (!http || !limits.max_encoded_bytes || limits.max_encoded_bytes > 32*1024*1024 || !limits.max_dimension ||
            limits.max_dimension > 32768 || !limits.max_pixels || !limits.max_decoded_bytes ||
            !limits.max_active_bytes || limits.max_active_bytes > 256*1024*1024 ||
            limits.max_cache_entries > 4096 || limits.max_cache_bytes > 256*1024*1024 ||
            !limits.max_concurrent_decodes || limits.max_concurrent_decodes > 16)
            throw IdentityError(IdentityErrorCode::invalid_input);
    }

    std::shared_ptr<Request> begin(const std::string& account_id) {
        std::lock_guard lock(mutex);
        if (requests.size() >= 32) throw IdentityError(IdentityErrorCode::quota);
        if (account_id.size() > 256 || !valid_utf8(account_id)) throw IdentityError(IdentityErrorCode::invalid_input);
        auto op = std::make_shared<Request>();
        op->account_id = account_id;
        op->id = ++sequence;
        op->global_epoch = global_epoch;
        op->account_epoch = epochs[account_id];
        requests[op->id] = op;
        return op;
    }
    void finish(const std::shared_ptr<Request>& op) {
        std::lock_guard lock(mutex);
        op->active = false;
        requests.erase(op->id);
        const bool still_active = std::any_of(requests.begin(),requests.end(),[&](const auto& item) {
            return item.second->account_id == op->account_id;
        });
        if (!still_active) epochs.erase(op->account_id);
    }
    void cancel(const std::shared_ptr<Request>& op) noexcept {
        {
            std::lock_guard lock(mutex);
            if (!op->active) return;
            op->active = false;
        }
        op->cancellation.cancel();
    }
    void check_locked(const std::shared_ptr<Request>& op, std::stop_token caller) const {
        check_stop(caller);
        check_stop(op->cancellation.stop_token());
        const auto found = epochs.find(op->account_id);
        if (!op->active || found == epochs.end() || found->second != op->account_epoch || global_epoch != op->global_epoch)
            throw IdentityError(IdentityErrorCode::cancelled);
    }
    void check(const std::shared_ptr<Request>& op, std::stop_token caller) const {
        std::lock_guard lock(mutex);
        check_locked(op,caller);
    }

    std::size_t output_bytes(std::uint32_t size) const {
        const auto pixels = static_cast<std::uint64_t>(size) * size;
        if (!size || size > limits.max_dimension || pixels > limits.max_pixels ||
            pixels > limits.max_decoded_bytes / 4 || pixels > std::numeric_limits<std::size_t>::max() / 4)
            throw IdentityError(IdentityErrorCode::image_too_large);
        return static_cast<std::size_t>(pixels) * 4;
    }

    struct PixelLease {
        Impl& state;
        std::size_t total;
        std::size_t decoded;
        bool decoding = true;
        PixelLease(Impl& state_, std::size_t total_, std::size_t decoded_, std::stop_token stop)
            : state(state_),total(total_),decoded(decoded_) {
            if (decoded > state.limits.max_decoded_bytes) throw IdentityError(IdentityErrorCode::image_too_large);
            if (total > state.limits.max_active_bytes) throw IdentityError(IdentityErrorCode::quota);
            check_stop(stop);
            std::unique_lock lock(state.mutex);
            if (!state.condition.wait(lock,stop,[&] {
                return state.active_decodes < state.limits.max_concurrent_decodes &&
                       decoded <= state.limits.max_decoded_bytes - state.decode_bytes &&
                       total <= state.limits.max_active_bytes - state.active_bytes;
            })) throw IdentityError(IdentityErrorCode::cancelled);
            check_stop(stop);
            ++state.active_decodes;
            state.decode_bytes += decoded;
            state.active_bytes += total;
        }
        void retain_output(std::size_t bytes) {
            if (bytes > total) throw IdentityError(IdentityErrorCode::quota);
            std::lock_guard lock(state.mutex);
            --state.active_decodes;
            state.decode_bytes -= decoded;
            state.active_bytes -= total - bytes;
            total = bytes;
            decoding = false;
            state.condition.notify_all();
        }
        ~PixelLease() {
            std::lock_guard lock(state.mutex);
            if (decoding) {
                --state.active_decodes;
                state.decode_bytes -= decoded;
            }
            state.active_bytes -= total;
            state.condition.notify_all();
        }
        PixelLease(const PixelLease&) = delete;
        PixelLease& operator=(const PixelLease&) = delete;
    };
    struct ReadyImage {
        std::unique_ptr<PixelLease> lease;
        RgbaImage image;
    };

    IdentityHttpResponse fetch(std::string_view method, std::string_view url,
                               std::string body, std::size_t bound, std::stop_token stop) {
        check_stop(stop);
        {
            std::unique_lock lock(mutex);
            if (!condition.wait(lock,stop,[&] { return active_fetches < 4; })) throw IdentityError(IdentityErrorCode::cancelled);
            ++active_fetches;
        }
        struct Release {
            Impl& state;
            ~Release() { std::lock_guard lock(state.mutex); --state.active_fetches; state.condition.notify_all(); }
        } release{*this};
        IdentityHttpRequest request;
        request.method = std::string(method);
        request.url = parse_url(url,true).str();
        request.max_response_bytes = bound;
        request.sensitive = false;
        request.body.assign(body.begin(),body.end());
        request.headers.emplace_back("Accept",body.empty() && method == "GET" ? "image/png,image/webp,image/jpeg,application/json" : "application/json");
        if (!body.empty()) request.headers.emplace_back("Content-Type","application/json");
        IdentityHttpResponse response;
        try { response = http->perform(request,stop); }
        catch (const IdentityError&) { throw; }
        catch (...) { check_stop(stop); throw IdentityError(IdentityErrorCode::network); }
        check_stop(stop);
        if (response.body.size() > bound) throw IdentityError(IdentityErrorCode::image_too_large);
        if (response.status < 100 || response.status > 599) throw IdentityError(IdentityErrorCode::invalid_response);
        return response;
    }

    std::optional<Json> json(std::string_view method, std::string_view url, std::string body, std::stop_token stop) {
        auto response = fetch(method,url,std::move(body),64*1024,stop);
        if (response.status != 200) return std::nullopt;
        return parse_json({reinterpret_cast<const char*>(response.body.data()),response.body.size()},64*1024,16);
    }

    std::optional<EncodedImage> download(std::string url, const std::set<std::string>& trusted,
                                         ImageKind kind, std::stop_token stop) {
        for (unsigned redirects = 0; redirects <= 3; ++redirects) {
            url = texture_url(url,trusted);
            auto response = fetch("GET",url,{},limits.max_encoded_bytes,stop);
            if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
                if (redirects == 3) throw IdentityError(IdentityErrorCode::untrusted_download);
                url = resolve_redirect(url,response.header("Location"));
                continue;
            }
            if (response.status != 200 || response.body.empty()) return std::nullopt;
            const auto actual = sniff(response.body);
            const auto declared = response.header("Content-Type");
            const auto mime = ascii_lower(trim(std::string_view(declared).substr(0,declared.find(';'))));
            if (actual.empty() || (kind == ImageKind::skin && actual != "image/png") ||
                (!mime.empty() && mime != "application/octet-stream" && mime != actual))
                throw IdentityError(IdentityErrorCode::unsupported_image);
            safe_log(log,"media.download","success",response.status);
            return EncodedImage{std::move(response.body),actual,kind};
        }
        return std::nullopt;
    }

    std::optional<EncodedImage> skin(const SkinRequest& request, std::stop_token stop) {
        check_stop(stop);
        const auto name = trim(request.name);
        if (request.source == accounts::AccountType::offline || name.empty()) return std::nullopt;
        if (name.size() > 256 || !valid_utf8(name)) throw IdentityError(IdentityErrorCode::invalid_input);
        std::set<std::string> trusted{"https://textures.minecraft.net"};
        for (const auto& origin : request.trusted_texture_origins) trusted.insert(controlled_origin(origin));
        std::string id = request.uuid ? accounts::normalize_uuid(*request.uuid) : std::string{};
        if (!id.empty() && !valid_profile_id(id)) throw IdentityError(IdentityErrorCode::invalid_input);
        if (request.source == accounts::AccountType::microsoft) {
            if (id.empty()) {
                const auto lookup = json("GET","https://api.mojang.com/users/profiles/minecraft/"+urlencode(name),{},stop);
                if (!lookup) return std::nullopt;
                id = string_field(*lookup,"id",true,64);
                if (!valid_profile_id(id)) throw IdentityError(IdentityErrorCode::invalid_response);
            }
            const auto profile = json("GET","https://sessionserver.mojang.com/session/minecraft/profile/"+accounts::normalize_uuid(id),{},stop);
            if (!profile) return std::nullopt;
            const auto texture = texture_property(*profile);
            return texture ? download(*texture,trusted,ImageKind::skin,stop) : std::nullopt;
        }
        const auto server = request.source == accounts::AccountType::littleskin ? accounts::validate_server(request.server)
            : (request.server ? parse_url(*request.server).str() : throw IdentityError(IdentityErrorCode::invalid_server));
        const auto api = authentication_base(request.source,server);
        const auto site = accounts::site_base(server);
        const auto site_url = parse_url(site);
        if (site_url.scheme != "https" && !loopback(site_url)) throw IdentityError(IdentityErrorCode::untrusted_download);
        trusted.insert(site_url.origin());
        try {
            if (id.empty()) {
                const auto lookup = json("POST",join_url(api,"/api/profiles/minecraft"),Json::array({name}).dump(),stop);
                if (lookup && lookup->is_array() && !lookup->empty()) id = string_field((*lookup)[0],"id",true,64);
            }
            if (!id.empty()) {
                if (!valid_profile_id(id)) throw IdentityError(IdentityErrorCode::invalid_response);
                const auto profile = json("GET",join_url(api,"/sessionserver/session/minecraft/profile/"+accounts::normalize_uuid(id)),{},stop);
                if (profile) {
                    const auto texture = texture_property(*profile);
                    if (texture) {
                        const auto downloaded = download(*texture,trusted,ImageKind::skin,stop);
                        if (downloaded) return downloaded;
                    }
                }
            }
        } catch (const IdentityError& error) {
            if (error.code() == IdentityErrorCode::cancelled) throw;
            safe_log(log,"media.texture","fallback");
        }
        return download(join_url(site,"/avatar/player/"+urlencode(name))+"?size=256",trusted,ImageKind::avatar,stop);
    }

    enum class DecodePurpose { head, icon };
    ReadyImage decode(const EncodedImage& encoded, std::stop_token stop, std::uint32_t output_size = 0,
                      DecodePurpose purpose = DecodePurpose::head) {
        check_stop(stop);
        if (encoded.bytes.empty() || encoded.bytes.size() > limits.max_encoded_bytes) throw IdentityError(IdentityErrorCode::image_too_large);
        const auto actual = sniff(encoded.bytes);
        if (actual.empty() || (!encoded.mime.empty() && encoded.mime != actual)) throw IdentityError(IdentityErrorCode::unsupported_image);
        ImageProbe probe;
        try { probe = decoder.probe(encoded.bytes); }
        catch (const IdentityError&) { throw; }
        catch (...) { check_stop(stop); throw IdentityError(IdentityErrorCode::unsupported_image); }
        if (probe.mime != actual) throw IdentityError(IdentityErrorCode::unsupported_image);
        validate_probe(probe,limits);
        const auto bytes = static_cast<std::size_t>(probe.width) * probe.height * 4;
        const auto working = probe.decoded_working_bytes ? probe.decoded_working_bytes : bytes;
        const auto output = output_size ? output_bytes(output_size) : 0;
        const auto encoding = purpose == DecodePurpose::icon ? icon_png_encoding_working_bytes : 0;
        if (output > std::numeric_limits<std::size_t>::max() - encoding ||
            working > std::numeric_limits<std::size_t>::max() - output - encoding)
            throw IdentityError(IdentityErrorCode::quota);
        ReadyImage result{std::make_unique<PixelLease>(*this,working + output + encoding,bytes,stop),{}};
        RgbaImage decoded;
        try { decoded = decoder.decode(encoded.bytes,probe,limits,stop); }
        catch (const IdentityError&) { throw; }
        catch (...) { check_stop(stop); throw IdentityError(IdentityErrorCode::unsupported_image); }
        check_stop(stop);
        if (decoded.width != probe.width || decoded.height != probe.height || decoded.pixels.size() != bytes ||
            decoded.pixels.capacity() != bytes)
            throw IdentityError(IdentityErrorCode::unsupported_image);
        if (output_size) {
            result.image = purpose == DecodePurpose::icon ? resize_image(decoded,output_size,output_size,limits)
                                                         : compose_head(decoded,output_size,encoded.kind,limits);
            decoded = {};
        } else result.image = std::move(decoded);
        check_stop(stop);
        // Icon encoding keeps its scanline/publication buffers charged after decode.
        result.lease->retain_output(result.image.pixels.capacity() + encoding);
        return result;
    }

    ReadyImage fallback(bool alex, std::uint32_t size, std::stop_token stop) {
        check_stop(stop);
        const auto width = alex ? detail::alex_head_width : detail::steve_head_width;
        const auto height = alex ? detail::alex_head_height : detail::steve_head_height;
        const auto asset_bytes = static_cast<std::size_t>(width) * height * 4;
        const auto output = output_bytes(size);
        if (asset_bytes > std::numeric_limits<std::size_t>::max() - output) throw IdentityError(IdentityErrorCode::quota);
        ReadyImage result{std::make_unique<PixelLease>(*this,asset_bytes + output,asset_bytes,stop),{}};
        result.image = builtin_head(alex,size,limits);
        check_stop(stop);
        result.lease->retain_output(result.image.pixels.capacity());
        return result;
    }

    void cache_insert(const std::string& key, const std::string& account_id, const ImageResult& value) {
        auto charge = value.head.pixels.capacity();
        if (!limits.max_cache_entries || charge > limits.max_cache_bytes) return;
        for (const auto bytes : {key.size(),account_id.size(),sizeof(CacheEntry)}) {
            if (bytes > limits.max_cache_bytes - charge) return;
            charge += bytes;
        }
        const auto existing = cache.find(key);
        if (existing != cache.end()) { cache_bytes -= existing->second.charge; cache.erase(existing); }
        while (!cache.empty() && (cache.size() >= limits.max_cache_entries || charge > limits.max_cache_bytes - cache_bytes)) {
            const auto oldest = std::min_element(cache.begin(),cache.end(),[](const auto& a,const auto& b) { return a.second.recency < b.second.recency; });
            cache_bytes -= oldest->second.charge;
            cache.erase(oldest);
        }
        cache.emplace(key,CacheEntry{account_id,value,std::chrono::steady_clock::now() + (value.fallback ? std::chrono::seconds(30) : std::chrono::seconds(900)),++sequence,charge});
        cache_bytes += charge;
    }
};

ImageService::ImageService(accounts::IdentityHttp& http, ImageDecoder& decoder, ImageLimits limits, accounts::IdentityLog log)
    : ImageService(std::shared_ptr<accounts::IdentityHttp>(&http,[](accounts::IdentityHttp*) {}),decoder,limits,std::move(log)) {}
ImageService::ImageService(std::shared_ptr<accounts::IdentityHttp> http, ImageDecoder& decoder, ImageLimits limits, accounts::IdentityLog log)
    : impl_(std::make_unique<Impl>(std::move(http),decoder,limits,std::move(log))) {}
ImageService::~ImageService() { clear(); }

std::optional<EncodedImage> ImageService::fetch_skin(const SkinRequest& request, std::stop_token stop) {
    check_stop(stop);
    validate_request(request);
    auto op = impl_->begin(request.account_id);
    struct Finish { Impl& state; std::shared_ptr<Impl::Request> op; ~Finish() { state.finish(op); } } finish{*impl_,op};
    std::stop_callback link(stop,[this,op] { impl_->cancel(op); });
    auto encoded = impl_->skin(request,op->cancellation.stop_token());
    {
        std::lock_guard lock(impl_->mutex);
        impl_->check_locked(op,stop);
        op->active = false;
    }
    return encoded;
}

ImageResult ImageService::head(const SkinRequest& request, std::stop_token stop) {
    check_stop(stop);
    validate_request(request);
    impl_->output_bytes(request.output_size);
    const auto key = cache_key(request);
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->cache.find(key);
        if (found != impl_->cache.end() && found->second.expires > std::chrono::steady_clock::now()) {
            const auto bytes = found->second.result.head.pixels.capacity();
            if (bytes > impl_->limits.max_active_bytes - impl_->active_bytes) throw IdentityError(IdentityErrorCode::quota);
            impl_->active_bytes += bytes;
            struct CopyLease {
                Impl& state;
                std::size_t bytes;
                ~CopyLease() { state.active_bytes -= bytes; state.condition.notify_all(); }
            } release{*impl_,bytes};
            found->second.recency = ++impl_->sequence;
            auto result = found->second.result;
            check_stop(stop);
            result.cached = true;
            return result;
        }
    }
    auto op = impl_->begin(request.account_id);
    struct Finish { Impl& state; std::shared_ptr<Impl::Request> op; ~Finish() { state.finish(op); } } finish{*impl_,op};
    std::stop_callback link(stop,[this,op] { impl_->cancel(op); });
    std::unique_ptr<Impl::PixelLease> pixels;
    ImageResult result;
    result.revision = request.revision;
    try {
        const auto encoded = impl_->skin(request,op->cancellation.stop_token());
        if (encoded) {
            auto decoded = impl_->decode(*encoded,op->cancellation.stop_token(),request.output_size);
            pixels = std::move(decoded.lease);
            result.head = std::move(decoded.image);
        }
    } catch (const IdentityError& error) {
        if (error.code() == IdentityErrorCode::cancelled || error.code() == IdentityErrorCode::invalid_input ||
            error.code() == IdentityErrorCode::invalid_server || error.code() == IdentityErrorCode::quota) throw;
        safe_log(impl_->log,"media.head","fallback");
    }
    if (result.head.pixels.empty()) {
        const auto hash = md5(request.name);
        auto builtin = impl_->fallback((hash[0] & 1) != 0,request.output_size,op->cancellation.stop_token());
        pixels = std::move(builtin.lease);
        result.head = std::move(builtin.image);
        result.fallback = true;
    }
    safe_log(impl_->log,"media.head","ready");
    {
        std::lock_guard lock(impl_->mutex);
        impl_->check_locked(op,stop);
        impl_->cache_insert(key,request.account_id,result);
        op->active = false;
    }
    return result;
}

RgbaImage ImageService::decode_bounded(const EncodedImage& encoded, std::stop_token stop) {
    auto result = impl_->decode(encoded,stop);
    return std::move(result.image);
}

EncodedImage ImageService::normalize_icon(const EncodedImage& encoded, std::stop_token stop) {
    auto result = impl_->decode(encoded,stop,32,Impl::DecodePurpose::icon);
    EncodedImage icon{encode_icon_png(result.image,stop),"image/png",ImageKind::avatar};
    if (icon.bytes.size() > impl_->limits.max_encoded_bytes) throw IdentityError(IdentityErrorCode::image_too_large);
    check_stop(stop);
    return icon;
}

EncodedImage ImageService::read_selected_file(const std::filesystem::path& selected, std::stop_token stop) {
    check_stop(stop);
    if (selected.empty() || !selected.is_absolute()) throw IdentityError(IdentityErrorCode::invalid_input);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(selected,error);
    if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) throw IdentityError(IdentityErrorCode::unsupported_image);
    std::ifstream input(selected,std::ios::binary);
    if (!input) throw IdentityError(IdentityErrorCode::unsupported_image);
    EncodedImage encoded;
    encoded.kind = ImageKind::avatar;
    std::array<char,16384> chunk{};
    while (input) {
        check_stop(stop);
        input.read(chunk.data(),static_cast<std::streamsize>(chunk.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > impl_->limits.max_encoded_bytes - encoded.bytes.size()) throw IdentityError(IdentityErrorCode::image_too_large);
        encoded.bytes.insert(encoded.bytes.end(),chunk.begin(),chunk.begin()+count);
    }
    if (!input.eof()) throw IdentityError(IdentityErrorCode::unsupported_image);
    encoded.mime = sniff(encoded.bytes);
    if (encoded.mime.empty()) throw IdentityError(IdentityErrorCode::unsupported_image);
    return encoded;
}

void ImageService::invalidate(std::string_view id) {
    if (id.size() > 256 || !valid_utf8(id)) throw IdentityError(IdentityErrorCode::invalid_input);
    std::vector<std::shared_ptr<Impl::Request>> cancelled;
    {
        std::lock_guard lock(impl_->mutex);
        const auto epoch = impl_->epochs.find(std::string(id));
        if (epoch != impl_->epochs.end()) ++epoch->second;
        for (auto it = impl_->cache.begin(); it != impl_->cache.end();) {
            if (it->second.account_id == id) { impl_->cache_bytes -= it->second.charge; it = impl_->cache.erase(it); }
            else ++it;
        }
        for (const auto& [key,op] : impl_->requests) {
            (void)key;
            if (op->active && op->account_id == id) { op->active = false; cancelled.push_back(op); }
        }
    }
    for (const auto& op : cancelled) op->cancellation.cancel();
    impl_->condition.notify_all();
}

void ImageService::clear() {
    std::vector<std::shared_ptr<Impl::Request>> cancelled;
    {
        std::lock_guard lock(impl_->mutex);
        ++impl_->global_epoch;
        impl_->cache.clear();
        impl_->cache_bytes = 0;
        impl_->epochs.clear();
        for (const auto& [key,op] : impl_->requests) {
            (void)key;
            if (op->active) { op->active = false; cancelled.push_back(op); }
        }
    }
    for (const auto& op : cancelled) op->cancellation.cancel();
    impl_->condition.notify_all();
}

ImageCacheStats ImageService::cache_stats() const {
    std::lock_guard lock(impl_->mutex);
    return {impl_->cache.size(),impl_->cache_bytes,impl_->active_decodes,impl_->active_bytes,
            impl_->requests.size(),impl_->epochs.size()};
}

} // namespace nickel::media
