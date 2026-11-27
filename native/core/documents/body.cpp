#include "body.hpp"

#include <gumbo.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <limits>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

namespace nickel::documents {
namespace {
using resources::fail;
using Clock = std::chrono::steady_clock;

std::string safeText(std::string_view raw, std::size_t maximum) {
    if (!resources::validUtf8(raw)) fail("invalid_response", "Description text is not valid UTF-8");
    std::string result;
    result.reserve(std::min(raw.size(), maximum));
    for (std::size_t i = 0; i < raw.size();) {
        const auto first = std::uint8_t(raw[i]);
        const unsigned length = first < 0x80 ? 1U : first < 0xe0 ? 2U : first < 0xf0 ? 3U : 4U;
        std::uint32_t point = first < 0x80 ? first : first & (length == 2 ? 0x1f : length == 3 ? 0x0f : 7);
        for (unsigned n = 1; n < length; ++n) point = (point << 6) | (std::uint8_t(raw[i + n]) & 0x3f);
        if (point == 0x7f || (point < 0x20 && point != 9 && point != 10 && point != 13) ||
            (point >= 0x202a && point <= 0x202e) || (point >= 0x2066 && point <= 0x2069)) { i += length; continue; }
        if (length > maximum - result.size()) break;
        result.append(raw.substr(i, length));
        i += length;
    }
    return result;
}

std::string escaped(std::string_view text) {
    std::string result;
    for (const auto c : text) {
        switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string nextDocumentId() {
    static std::atomic<std::uint64_t> sequence{0};
    static const auto nonce = [] { std::random_device random; return (std::uint64_t(random()) << 32) | random(); }();
    return "d" + std::to_string(nonce) + '-' + std::to_string(sequence.fetch_add(1));
}

std::string opaque(std::string_view scheme, const std::string& id, std::size_t index) {
    return std::string(scheme) + "://" + id + '/' + std::to_string(index);
}

// Arena ownership avoids Gumbo's recursive destroy walk on hostile trees.
// MSVC /EHsc- retains C-callback C++ unwind guards without catching SEH.
struct ParseArena {
    std::unordered_map<void*, std::size_t> live;
    std::size_t charged = 0;
    std::size_t maximum;
    Clock::time_point deadline;
    const Cancellation& cancel;
    void check() const {
        cancel.check();
        if (Clock::now() >= deadline) fail("too_large", "Description parser exceeded its time budget");
    }
    // Every parser allocation is charged before the pointer reaches Gumbo, so a hostile
    // document hits a typed limit instead of exhausting the process.
    static void* allocate(void* context, std::size_t bytes) {
        auto& arena = *static_cast<ParseArena*>(context);
        arena.check();
        if (bytes > std::numeric_limits<std::size_t>::max() - 64 || bytes + 64 > arena.maximum - arena.charged) fail("too_large", "Description parser exceeded its allocation budget");
        void* memory = std::malloc(bytes ? bytes : 1);
        if (!memory) fail("too_large", "Description parser could not allocate within budget");
        try { arena.live.emplace(memory, bytes + 64); }
        catch (...) { std::free(memory); throw; }
        arena.charged += bytes + 64;
        return memory;
    }
    static void deallocate(void* context, void* memory) {
        if (!memory) return;
        auto& arena = *static_cast<ParseArena*>(context);
        const auto found = arena.live.find(memory);
        if (found == arena.live.end()) return;
        arena.charged -= found->second;
        arena.live.erase(found);
        std::free(memory);
    }
    ~ParseArena() { for (const auto& [memory, charge] : live) { (void)charge; std::free(memory); } }
};

const GumboVector* children(const GumboNode* node) {
    if (node->type == GUMBO_NODE_DOCUMENT) return &node->v.document.children;
    if (node->type == GUMBO_NODE_ELEMENT || node->type == GUMBO_NODE_TEMPLATE) return &node->v.element.children;
    return nullptr;
}

std::string attribute(const GumboNode* node, const char* name) {
    const auto* value = gumbo_get_attribute(&node->v.element.attributes, name);
    return value && value->attr_namespace == GUMBO_ATTR_NAMESPACE_NONE && value->value ? std::string(value->value) : std::string{};
}

bool deniedTag(const std::string& tag) {
    static const std::set<std::string> denied{
        "script", "style", "iframe", "frame", "frameset", "object", "embed", "applet", "svg", "math", "template",
        "form", "input", "button", "textarea", "select", "option", "link", "meta", "base", "video", "audio",
        "source", "track", "canvas", "noscript", "head", "title"
    };
    return denied.contains(tag);
}

std::string allowedTag(const std::string& tag) {
    static const std::set<std::string> allowed{
        "p", "br", "hr", "h1", "h2", "h3", "h4", "h5", "h6", "strong", "em", "b", "i", "u", "s", "ul", "ol", "li",
        "blockquote", "pre", "code", "table", "thead", "tbody", "tfoot", "tr", "th", "td", "dl", "dt", "dd", "sup", "sub", "span", "div"
    };
    if (tag == "section" || tag == "article" || tag == "details" || tag == "summary" || tag == "figure" || tag == "figcaption") return "div";
    if (tag == "del") return "s";
    return allowed.contains(tag) ? tag : std::string{};
}

bool blockTag(const std::string& tag) {
    return tag == "p" || tag == "div" || tag == "li" || tag == "tr" || tag == "pre" || tag == "blockquote" ||
        (tag.size() == 2 && tag.front() == 'h') || tag == "dd" || tag == "dt";
}

unsigned displaySize(const std::string& value, unsigned fallback) {
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result ? std::min(result, 1024U) : fallback;
}

bool rasterBytes(const resources::Bytes& bytes) {
    static constexpr std::uint8_t png[]{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    return (bytes.size() >= 8 && std::equal(std::begin(png), std::end(png), bytes.begin())) ||
        (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) ||
        (bytes.size() >= 12 && std::equal(bytes.begin(), bytes.begin() + 4, "RIFF") && std::equal(bytes.begin() + 8, bytes.begin() + 12, "WEBP"));
}

} // namespace

struct SafeBody::Data {
    std::string html;
    std::string plain;
    std::string id;
    std::vector<BodyImage> images;
    std::vector<BodyLink> links;
    std::map<std::string, std::string> imageSources;
    std::map<std::string, std::string> linkTargets;
    resources::UrlRules imageRules;
    resources::UrlRules linkRules;
};

SafeBody::SafeBody(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
bool SafeBody::valid() const noexcept { return bool(data_); }
const std::string& SafeBody::html() const { static const std::string empty; return data_ ? data_->html : empty; }
const std::string& SafeBody::plainText() const { static const std::string empty; return data_ ? data_->plain : empty; }
const std::string& SafeBody::documentId() const { static const std::string empty; return data_ ? data_->id : empty; }
std::vector<BodyImage> SafeBody::images() const { return data_ ? data_->images : std::vector<BodyImage>{}; }
std::vector<BodyLink> SafeBody::links() const { return data_ ? data_->links : std::vector<BodyLink>{}; }

std::string SafeBody::externalLink(const std::string& key) const {
    if (!data_) fail("policy_denied", "Unapproved document link");
    const auto found = data_->linkTargets.find(key);
    if (found == data_->linkTargets.end()) fail("policy_denied", "Unapproved document link");
    return resources::checkedUrl(found->second, data_->linkRules);
}

std::size_t SafeBody::chargedBytes() const {
    if (!data_) return 0;
    auto total = sizeof(Data) + data_->html.capacity() + data_->plain.capacity() + data_->id.capacity();
    for (const auto& image : data_->images) total += sizeof(BodyImage) + image.key.capacity() + image.alt.capacity();
    for (const auto& link : data_->links) total += sizeof(BodyLink) + link.key.capacity() + link.label.capacity();
    for (const auto* mapping : {&data_->imageSources, &data_->linkTargets}) for (const auto& [key, value] : *mapping) total += 96 + key.capacity() + value.capacity();
    for (const auto* rules : {&data_->imageRules, &data_->linkRules}) for (const auto* hosts : {&rules->exactHosts, &rules->exactOrigins, &rules->testLoopbackOrigins}) for (const auto& value : *hosts) total += 64 + value.capacity();
    return total;
}

BodyReader::BodyReader(BodyLimits limits, resources::UrlRules images, resources::UrlRules links, MarkdownNormalizer markdown)
    : limits_(limits), imageRules_(std::move(images)), linkRules_(std::move(links)), markdown_(std::move(markdown)) {
    if (!limits_.maximumInputBytes || limits_.maximumInputBytes > 512 * 1024 || !limits_.maximumOutputBytes || limits_.maximumOutputBytes > 4 * 1024 * 1024 ||
        !limits_.maximumNodes || limits_.maximumNodes > 50000 || !limits_.maximumDepth || limits_.maximumDepth > 128 ||
        limits_.maximumImages > 256 || limits_.maximumLinks > 1024 || limits_.maximumTableCells > 4096 ||
        limits_.maximumParseTime.count() <= 0 || limits_.maximumParseTime.count() > 1000) fail("invalid_input", "Invalid bounded document limits");
}

SafeBody BodyReader::convert(const FullDescription& description, const Cancellation& cancel) const {
    cancel.check();
    const auto deadline = Clock::now() + limits_.maximumParseTime;
    if (!resources::validIdentifier(description.source, description.projectId)) fail("invalid_input", "Invalid description identity");
    if (description.text.size() > limits_.maximumInputBytes) fail("too_large", "Description exceeds input byte budget");
    if (!resources::validUtf8(description.text)) fail("invalid_response", "Description is not valid UTF-8");
    auto data = std::make_shared<SafeBody::Data>();
    data->id = nextDocumentId();
    data->imageRules = imageRules_;
    data->linkRules = linkRules_;
    if (description.format == BodyFormat::PlainText) {
        data->plain = safeText(description.text, limits_.maximumInputBytes);
        data->html = "<pre>" + escaped(data->plain) + "</pre>";
        if (data->html.size() > limits_.maximumOutputBytes) fail("too_large", "Description exceeds output byte budget");
        cancel.check();
        return SafeBody(data);
    }
    std::string html = description.text;
    if (description.format == BodyFormat::Markdown) {
        if (!markdown_) fail("unsupported", "Shared native Markdown normalizer is unavailable");
        html = markdown_(description.text, cancel);
    } else if (description.format != BodyFormat::Html) fail("invalid_input", "Unsupported description format");
    if (html.size() > limits_.maximumOutputBytes || !resources::validUtf8(html)) fail("too_large", "Normalized description exceeds its byte budget");
    ParseArena arena{{}, 0, limits_.maximumOutputBytes * 16, deadline, cancel};
    arena.check();
    auto options = kGumboDefaultOptions;
    options.allocator = &ParseArena::allocate;
    options.deallocator = &ParseArena::deallocate;
    options.userdata = &arena;
    options.max_errors = 0;
    auto* output = gumbo_parse_with_options(&options, html.data(), html.size());
    arena.check();
    if (!output || !output->root) fail("invalid_response", "HTML parser rejected the description");
    struct Frame { const GumboNode* node; unsigned depth; bool exit; std::string tag; std::optional<std::size_t> link; std::size_t plainStart; };
    std::vector<Frame> stack{{output->root, 0, false, {}, {}, 0}};
    std::size_t nodes = 0, cells = 0;
    const auto append = [&](std::string_view value) {
        if (value.size() > limits_.maximumOutputBytes - data->html.size()) fail("too_large", "Description exceeds output byte budget");
        data->html.append(value);
    };
    const auto plain = [&](std::string_view value) {
        if (value.size() > limits_.maximumOutputBytes - data->plain.size()) fail("too_large", "Description exceeds plain-text byte budget");
        data->plain.append(value);
    };
    while (!stack.empty()) {
        arena.check();
        auto frame = std::move(stack.back());
        stack.pop_back();
        if (frame.exit) {
            if (!frame.tag.empty()) append("</" + frame.tag + '>');
            if (frame.link) data->links[*frame.link].label = safeText(std::string_view(data->plain).substr(frame.plainStart), 512);
            if (blockTag(frame.tag)) plain("\n");
            continue;
        }
        if (++nodes > limits_.maximumNodes || frame.depth > limits_.maximumDepth) fail("too_large", "Description exceeds AST depth or node budget");
        const auto* node = frame.node;
        if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_WHITESPACE || node->type == GUMBO_NODE_CDATA) {
            const auto text = safeText(node->v.text.text ? node->v.text.text : "", limits_.maximumOutputBytes);
            append(escaped(text));
            plain(text);
            continue;
        }
        if (node->type == GUMBO_NODE_COMMENT || node->type == GUMBO_NODE_TEMPLATE) continue;
        std::string rendered;
        std::optional<std::size_t> link;
        const auto plainStart = data->plain.size();
        if (node->type == GUMBO_NODE_ELEMENT) {
            if (node->v.element.tag_namespace != GUMBO_NAMESPACE_HTML) continue;
            const auto tag = std::string(gumbo_normalized_tagname(node->v.element.tag));
            if (deniedTag(tag)) continue;
            if (tag == "td" || tag == "th") if (++cells > limits_.maximumTableCells) fail("too_large", "Description exceeds table-cell budget");
            if (tag == "img") {
                const auto alt = safeText(attribute(node, "alt"), 512);
                std::string source;
                try { source = resources::checkedRawUrl(attribute(node, "src"), imageRules_, description.canonicalBase); }
                catch (const ErrorException& failure) { if (failure.error().code != "policy_denied") throw; }
                if (source.empty() || data->images.size() >= limits_.maximumImages) {
                    if (!alt.empty()) append('[' + escaped(alt) + ']');
                    plain(alt);
                    continue;
                }
                const auto key = opaque("nickel-image", data->id, data->images.size());
                const auto width = displaySize(attribute(node, "width"), 160);
                const auto height = displaySize(attribute(node, "height"), 90);
                data->images.push_back({key, alt, width, height});
                data->imageSources.emplace(key, std::move(source));
                append("<img src=\"" + key + "\" alt=\"" + escaped(alt) + "\" width=\"" + std::to_string(width) + "\" height=\"" + std::to_string(height) + "\" />");
                plain(alt);
                continue;
            }
            if (tag == "a") {
                std::string target;
                try { target = resources::checkedRawUrl(attribute(node, "href"), linkRules_, description.canonicalBase); }
                catch (const ErrorException& failure) { if (failure.error().code != "policy_denied") throw; }
                if (!target.empty() && data->links.size() < limits_.maximumLinks) {
                    link = data->links.size();
                    const auto key = opaque("nickel-link", data->id, *link);
                    data->links.push_back({key, {}});
                    data->linkTargets.emplace(key, std::move(target));
                    rendered = "a";
                    append("<a href=\"" + key + "\">");
                }
            } else {
                rendered = allowedTag(tag);
                if (rendered == "br" || rendered == "hr") { append('<' + rendered + " />"); plain("\n"); continue; }
                if (!rendered.empty()) append('<' + rendered + '>');
            }
        }
        const auto* vector = children(node);
        if (vector) {
            if (vector->length > limits_.maximumNodes - nodes || stack.size() + vector->length > limits_.maximumNodes * 2) fail("too_large", "Description exceeds AST work budget");
            stack.push_back({node, frame.depth, true, rendered, link, plainStart});
            for (unsigned index = vector->length; index; --index) stack.push_back({static_cast<const GumboNode*>(vector->data[index - 1]), frame.depth + 1, false, {}, {}, 0});
        } else if (!rendered.empty()) append("</" + rendered + '>');
    }
    arena.check();
    return SafeBody(data);
}

std::shared_ptr<const RasterImage> resourcePlaceholder() {
    static const auto image = std::make_shared<const RasterImage>();
    return image;
}

void validateRasterImage(const RasterImage& image, const ImageLimits& limits) {
    if (!image.width || !image.height || image.width > limits.maximumDimension || image.height > limits.maximumDimension ||
        image.width > limits.maximumDisplayDimension || image.height > limits.maximumDisplayDimension ||
        std::uint64_t(image.width) * image.height > limits.maximumPixels || image.stride < std::uint64_t(image.width) * 4 ||
        image.stride > std::uint64_t(limits.maximumDimension) * 4 || image.stride > std::numeric_limits<std::size_t>::max() / image.height ||
        image.rgba.size() != image.stride * image.height) fail("integrity", "Image decoder returned invalid bounded RGBA pixels");
}

struct BodyResources::State {
    struct Entry { std::shared_ptr<const RasterImage> image; std::uint64_t used; std::size_t charge; };
    struct Flight {
        std::string key;
        std::string url;
        resources::UrlRules rules;
        Cancellation physical;
        std::uint64_t generation = 0;
        std::size_t subscribers = 0;
        bool done = false;
        ResourceReply reply{ResourceState::Pending, resourcePlaceholder()};
        std::optional<Error> error;
        std::condition_variable_any changed;
    };
    resources::CachedHttp& http;
    ImageLimits limits;
    ImageDecoder decoder;
    mutable std::mutex mutex;
    std::condition_variable available;
    SafeBody body;
    std::map<std::string, Entry> cache;
    std::map<std::string, ResourceState> states;
    std::map<std::string, std::shared_ptr<Flight>> flights;
    std::deque<std::shared_ptr<Flight>> queue;
    std::vector<std::jthread> workers;
    std::size_t bytes = 0;
    std::uint64_t tick = 0;
    std::uint64_t generation = 0;
    bool stopping = false;

    State(resources::CachedHttp& client, ImageLimits value, ImageDecoder decode) : http(client), limits(value), decoder(std::move(decode)) {
        workers.reserve(limits.maximumConcurrent);
        try { for (unsigned i = 0; i < limits.maximumConcurrent; ++i) workers.emplace_back([this] { run(); }); }
        catch (...) { shutdown(); throw; }
    }
    ~State() { shutdown(); }
    void stopLocked() {
        ++generation;
        for (auto& [key, flight] : flights) {
            (void)key;
            flight->physical.cancel();
            flight->done = true;
            flight->error = Error{"cancelled", "Document resources cancelled"};
            flight->changed.notify_all();
        }
        flights.clear();
        queue.clear();
        states.clear();
    }
    void shutdown() {
        { std::lock_guard lock(mutex); if (!stopping) { stopping = true; stopLocked(); } }
        available.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    }
    void store(const std::string& key, const std::shared_ptr<const RasterImage>& image) {
        const auto charge = sizeof(RasterImage) + image->rgba.capacity() + key.capacity() + 128;
        const auto previous = cache.find(key);
        if (previous != cache.end()) { bytes -= previous->second.charge; cache.erase(previous); }
        if (charge > limits.maximumCacheBytes) return;
        while (charge > limits.maximumCacheBytes - bytes && !cache.empty()) {
            const auto victim = std::min_element(cache.begin(), cache.end(), [](const auto& first, const auto& second) { return first.second.used < second.second.used; });
            bytes -= victim->second.charge;
            cache.erase(victim);
        }
        cache.emplace(key, Entry{image, ++tick, charge});
        bytes += charge;
    }
    void run() {
        for (;;) {
            std::shared_ptr<Flight> flight;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping) return;
                flight = std::move(queue.front());
                queue.pop_front();
                if (flight->done || flight->physical.cancelled()) continue;
            }
            ResourceReply reply{ResourceState::Failed, resourcePlaceholder()};
            std::optional<Error> error;
            try {
                if (!decoder) fail("unsupported", "Shared bounded raster decoder is unavailable");
                resources::HttpRequest request;
                request.url = flight->url;
                request.rules = flight->rules;
                request.maximumBytes = limits.maximumEncodedBytes;
                request.timeout = std::chrono::milliseconds(15000);
                request.followRedirects = false;
                request.headers = {{"accept", "image/png,image/jpeg,image/webp"}, {"accept-encoding", "identity"}};
                request.validationScope = "safe-raster-envelope-v2:" + std::to_string(limits.maximumEncodedBytes);
                request.validate = [](const resources::HttpResponse& response) {
                    if (!rasterBytes(response.bytes)) fail("policy_denied", "Only raster document images are accepted");
                    const auto header = resources::lower(resources::responseHeader(response, "content-type"));
                    const auto type = resources::trim(std::string_view(header).substr(0, header.find(';')));
                    if (!type.empty() && type != "image/png" && type != "image/jpeg" && type != "image/webp" && type != "application/octet-stream") fail("policy_denied", "Document image has an active or unexpected media type");
                };
                const auto response = http.fetch(std::move(request), {}, flight->physical);
                flight->physical.check();
                auto image = decoder(response.response->bytes, limits, flight->physical);
                validateRasterImage(image, limits);
                flight->physical.check();
                reply = {ResourceState::Ready, std::make_shared<const RasterImage>(std::move(image))};
            } catch (const ErrorException& failure) { error = failure.error(); }
              catch (...) { error = Error{"invalid_response", "Document raster decoding failed"}; }
            {
                std::lock_guard lock(mutex);
                if (flight->done) continue;
                if (flight->generation != generation || flight->physical.cancelled() || !flight->subscribers) {
                    error = Error{"cancelled", "Document generation changed"};
                    reply = {ResourceState::Pending, resourcePlaceholder()};
                } else {
                    if (reply.state == ResourceState::Ready) store(flight->key, reply.image);
                    states.insert_or_assign(flight->key, reply.state);
                }
                flight->reply = std::move(reply);
                flight->error = std::move(error);
                flight->done = true;
                const auto current = flights.find(flight->key);
                if (current != flights.end() && current->second == flight) flights.erase(current);
                flight->changed.notify_all();
            }
        }
    }
};

BodyResources::BodyResources(resources::CachedHttp& http, ImageLimits limits, ImageDecoder decoder) {
    if (!limits.maximumEncodedBytes || limits.maximumEncodedBytes > 8 * 1024 * 1024 || limits.maximumCacheBytes > 64 * 1024 * 1024 ||
        !limits.maximumPixels || limits.maximumPixels > 4 * 1024 * 1024 || !limits.maximumDimension || limits.maximumDimension > 4096 ||
        !limits.maximumDisplayDimension || limits.maximumDisplayDimension > 1024 || !limits.maximumConcurrent || limits.maximumConcurrent > 8) fail("invalid_input", "Invalid bounded image resource limits");
    state_ = std::make_shared<State>(http, limits, std::move(decoder));
}
BodyResources::~BodyResources() { state_->shutdown(); }

void BodyResources::setBody(const SafeBody& body) {
    std::lock_guard lock(state_->mutex);
    state_->stopLocked();
    state_->cache.clear();
    state_->bytes = 0;
    state_->body = body;
}

bool BodyResources::approved(const std::string& key) const {
    std::lock_guard lock(state_->mutex);
    return state_->body.data_ && state_->body.data_->imageSources.contains(key);
}

ResourceReply BodyResources::lookup(const std::string& key) const {
    std::lock_guard lock(state_->mutex);
    if (!state_->body.data_ || !state_->body.data_->imageSources.contains(key)) return {ResourceState::Denied, resourcePlaceholder()};
    const auto found = state_->cache.find(key);
    if (found != state_->cache.end()) { found->second.used = ++state_->tick; return {ResourceState::Ready, found->second.image}; }
    const auto status = state_->states.find(key);
    return {status != state_->states.end() && status->second == ResourceState::Failed ? ResourceState::Failed : ResourceState::Pending, resourcePlaceholder()};
}

ResourceReply BodyResources::requestVisible(const std::string& key, const Cancellation& cancel) {
    cancel.check();
    auto state = state_;
    std::unique_lock lock(state->mutex);
    if (state->stopping) fail("cancelled", "Document resources stopped");
    if (!state->body.data_ || !state->body.data_->imageSources.contains(key)) return {ResourceState::Denied, resourcePlaceholder()};
    const auto cached = state->cache.find(key);
    if (cached != state->cache.end()) { cached->second.used = ++state->tick; return {ResourceState::Ready, cached->second.image}; }
    const auto status = state->states.find(key);
    if (status != state->states.end() && status->second == ResourceState::Failed) return {ResourceState::Failed, resourcePlaceholder()};
    std::shared_ptr<State::Flight> flight;
    const auto active = state->flights.find(key);
    if (active != state->flights.end() && !active->second->physical.cancelled()) flight = active->second;
    else {
        if (state->flights.size() >= state->body.data_->images.size()) fail("busy", "Document image queue budget exceeded");
        flight = std::make_shared<State::Flight>();
        flight->key = key;
        flight->url = state->body.data_->imageSources.at(key);
        flight->rules = state->body.data_->imageRules;
        flight->generation = state->generation;
        state->flights.insert_or_assign(key, flight);
        state->queue.push_back(flight);
        state->available.notify_one();
    }
    if (flight->subscribers >= 1024) fail("busy", "Document image subscriber budget exceeded");
    ++flight->subscribers;
    flight->changed.wait(lock, cancel.stop_token(), [&] { return flight->done; });
    --flight->subscribers;
    if (cancel.cancelled()) {
        if (!flight->subscribers && !flight->done) {
            flight->physical.cancel();
            std::erase(state->queue, flight);
            const auto current = state->flights.find(key);
            if (current != state->flights.end() && current->second == flight) state->flights.erase(current);
        }
        fail("cancelled", "Visible document image request cancelled");
    }
    if (flight->error && flight->error->code == "cancelled") throw ErrorException(*flight->error);
    return flight->reply;
}

void BodyResources::cancel() { std::lock_guard lock(state_->mutex); state_->stopLocked(); }
std::size_t BodyResources::decodedCacheBytes() const { std::lock_guard lock(state_->mutex); return state_->bytes; }

} // namespace nickel::documents
