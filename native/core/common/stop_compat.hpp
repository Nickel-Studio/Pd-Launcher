#pragma once

// C++20 stop-token family compatibility layer.
//
// Covers: std::stop_token, std::stop_source, std::stop_callback, std::jthread
// (the stop-aware std::condition_variable_any waits ride on the same
// availability switch).
//
// Why this layer exists (root cause, verified in CI run 45160199828 with
// AppleClang 17.0.0.17000013, MacOSX15.5.sdk, -std=c++20, deployment target
// 12.0): libc++ classifies the stop-token family as an "incomplete" feature
// and hides the declarations behind _LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN
// unless _LIBCPP_ENABLE_EXPERIMENTAL is defined before <stop_token> is parsed.
// Upstream libc++ headers gate the declarations with
//   #if _LIBCPP_STD_VER >= 20 && !defined(_LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN) && !defined(_LIBCPP_HAS_NO_THREADS)
// (libc++ include/stop_token, include/__stop_token/stop_token.h,
// include/__thread/jthread.h), while MSVC STL and libstdc++ (GCC >= 10) provide
// the family unconditionally. That is why Windows/Linux builds compile and the
// macOS app stage fails with "no type named 'stop_token' in namespace 'std'".
//
// Layered resolution, decided entirely at compile time:
//   1. Standard library provides the family (MSVC, GCC, non-gated libc++):
//      the real std headers are used, nothing else happens.
//   2. libc++ merely gates the family (Apple SDKs): re-enable it with libc++'s
//      own documented switch _LIBCPP_ENABLE_EXPERIMENTAL, scoped to this
//      feature only, before <stop_token> is parsed. The real std types are
//      used. The production macOS build takes this path: the root CMakeLists
//      force-includes this header into every C++ translation unit on Apple.
//   3. The standard library genuinely lacks the family (no __cpp_lib_jthread
//      and no libc++ gate to lift), or a test build defines
//      NICKEL_FORCE_PORTABLE_CANCELLATION: the portable implementation below
//      (namespace nickel::stop_compat) provides stop_token / stop_source /
//      stop_callback / jthread with standard semantics and is injected into
//      namespace std so the existing call sites keep compiling unchanged.
//      NICKEL_FORCE_PORTABLE_CANCELLATION is a test-only switch; production
//      Windows/Linux builds never define it.
//
// No runtime dependency and no platform branch is introduced on the
// Windows/Linux production paths.

#include <version>

#if !defined(NICKEL_FORCE_PORTABLE_CANCELLATION) && defined(_LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN) && !defined(_LIBCPP_ENABLE_EXPERIMENTAL)
// libc++ gate detected before <stop_token> was parsed in this translation unit:
// lift exactly the stop-token family with libc++'s own switch, then re-include.
#define _LIBCPP_ENABLE_EXPERIMENTAL
#undef _LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN
#define NICKEL_STOP_COMPAT_LIBCPP_REENABLED 1
#endif

#if __has_include(<stop_token>)
#include <stop_token>
#endif

#if defined(NICKEL_FORCE_PORTABLE_CANCELLATION)
#define NICKEL_STOP_COMPAT_PORTABLE 1
#elif defined(NICKEL_STOP_COMPAT_LIBCPP_REENABLED)
#define NICKEL_STOP_COMPAT_PORTABLE 0
#elif defined(__cpp_lib_jthread)
#define NICKEL_STOP_COMPAT_PORTABLE 0
#else
#define NICKEL_STOP_COMPAT_PORTABLE 1
#endif

#if NICKEL_STOP_COMPAT_PORTABLE

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace nickel::stop_compat {

struct nostopstate_t {
    explicit nostopstate_t() = default;
};

inline constexpr nostopstate_t nostopstate{};

namespace detail {

struct callback_entry {
    enum class Status : int { kRegistered, kExecuting, kDone, kRemoved };

    virtual void invoke() noexcept = 0;

    Status status = Status::kRegistered;

protected:
    callback_entry() = default;
    ~callback_entry() = default;
    callback_entry(const callback_entry&) = delete;
    callback_entry& operator=(const callback_entry&) = delete;
};

}  // namespace detail

// Shared control block of one stop request. All callback bookkeeping happens
// under mutex_; only the stopped flag is lock-free so stop_requested() stays
// cheap in polling loops.
class stop_state {
public:
    stop_state() = default;
    stop_state(const stop_state&) = delete;
    stop_state& operator=(const stop_state&) = delete;

    bool stop_requested() const noexcept { return stopped_.load(std::memory_order_acquire); }

    bool stop_possible_for_token() const {
        if (stopped_.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        return sources_ > 0;
    }

    // Returns false when the stop was already requested; the caller must then
    // invoke the callback itself (stop_callback does that synchronously).
    bool register_callback(detail::callback_entry* entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_.load(std::memory_order_acquire)) return false;
        callbacks_.push_back(entry);
        return true;
    }

    // Blocks until a concurrently executing callback finished, so the
    // stop_callback destructor can never race into freed callback state.
    void remove_callback(detail::callback_entry* entry) {
        using Status = detail::callback_entry::Status;
        std::unique_lock<std::mutex> lock(mutex_);
        if (entry->status == Status::kRegistered) {
            for (std::size_t index = 0; index < callbacks_.size(); ++index) {
                if (callbacks_[index] == entry) {
                    callbacks_.erase(callbacks_.begin() + static_cast<std::ptrdiff_t>(index));
                    break;
                }
            }
            entry->status = Status::kRemoved;
            return;
        }
        if (entry->status == Status::kExecuting) {
            // request_stop() marks the entry kDone under the same mutex before
            // notifying, so this wait cannot miss its wakeup.
            cv_.wait(lock, [entry] { return entry->status != Status::kExecuting; });
        }
    }

    // Idempotent per the standard: returns false when a stop was already
    // requested. Callbacks run outside the mutex, in the requesting thread.
    bool request_stop() {
        std::vector<detail::callback_entry*> executing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_.exchange(true, std::memory_order_acq_rel)) return false;
            executing.swap(callbacks_);
            for (auto* entry : executing) entry->status = detail::callback_entry::Status::kExecuting;
        }
        for (auto* entry : executing) entry->invoke();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto* entry : executing) entry->status = detail::callback_entry::Status::kDone;
        }
        cv_.notify_all();
        return true;
    }

    void add_source() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++sources_;
    }

    void release_source() {
        std::lock_guard<std::mutex> lock(mutex_);
        --sources_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic_bool stopped_{false};
    std::size_t sources_ = 0;
    std::vector<detail::callback_entry*> callbacks_;
};

class stop_source;

class stop_token {
public:
    stop_token() noexcept = default;
    stop_token(const stop_token&) noexcept = default;
    stop_token(stop_token&&) noexcept = default;
    stop_token& operator=(const stop_token&) noexcept = default;
    stop_token& operator=(stop_token&&) noexcept = default;
    ~stop_token() = default;

    void swap(stop_token& other) noexcept { state_.swap(other.state_); }
    [[nodiscard]] bool stop_requested() const noexcept { return state_ != nullptr && state_->stop_requested(); }
    [[nodiscard]] bool stop_possible() const noexcept { return state_ != nullptr && state_->stop_possible_for_token(); }
    friend bool operator==(const stop_token& left, const stop_token& right) noexcept { return left.state_ == right.state_; }
    friend bool operator!=(const stop_token& left, const stop_token& right) noexcept { return left.state_ != right.state_; }
    friend void swap(stop_token& left, stop_token& right) noexcept { left.swap(right); }

private:
    friend class stop_source;
    template <class Callback>
    friend class stop_callback;
    explicit stop_token(std::shared_ptr<stop_state> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<stop_state> state_;
};

class stop_source {
public:
    stop_source() : state_(std::make_shared<stop_state>()) { state_->add_source(); }
    explicit stop_source(nostopstate_t) noexcept {}
    stop_source(const stop_source& other) noexcept : state_(other.state_) {
        if (state_) state_->add_source();
    }
    stop_source(stop_source&& other) noexcept = default;
    stop_source& operator=(const stop_source& other) noexcept {
        if (this != &other) {
            if (state_) state_->release_source();
            state_ = other.state_;
            if (state_) state_->add_source();
        }
        return *this;
    }
    stop_source& operator=(stop_source&& other) noexcept {
        if (this != &other) {
            if (state_) state_->release_source();
            state_ = std::move(other.state_);
        }
        return *this;
    }
    ~stop_source() {
        if (state_) state_->release_source();
    }

    void swap(stop_source& other) noexcept { state_.swap(other.state_); }
    [[nodiscard]] stop_token get_token() const noexcept { return stop_token(state_); }
    [[nodiscard]] bool stop_possible() const noexcept { return state_ != nullptr; }
    [[nodiscard]] bool stop_requested() const noexcept { return state_ != nullptr && state_->stop_requested(); }
    bool request_stop() noexcept { return state_ != nullptr && state_->request_stop(); }
    friend void swap(stop_source& left, stop_source& right) noexcept { left.swap(right); }

private:
    std::shared_ptr<stop_state> state_;
};

template <class Callback>
class stop_callback : private detail::callback_entry {
public:
    using callback_type = Callback;

    static_assert(std::is_invocable_v<Callback&>, "stop_callback requires an invocable callback");

    template <class Initializer>
    explicit stop_callback(stop_token token, Initializer&& initializer)
        : state_(std::move(token).state_), callback_(std::forward<Initializer>(initializer)) {
        if (state_ && !state_->register_callback(this)) this->invoke();
    }

    stop_callback(const stop_callback&) = delete;
    stop_callback(stop_callback&&) = delete;
    stop_callback& operator=(const stop_callback&) = delete;
    stop_callback& operator=(stop_callback&&) = delete;

    ~stop_callback() {
        if (state_) state_->remove_callback(this);
    }

private:
    void invoke() noexcept override {
        // A throwing callback terminates here, matching the noexcept callback
        // contract of std::stop_callback (libc++ invokes through a noexcept
        // callback function pointer).
        static_cast<void>(std::invoke(callback_));
    }

    std::shared_ptr<stop_state> state_;
    Callback callback_;
};

template <class Callback>
stop_callback(stop_token, Callback&&) -> stop_callback<std::decay_t<Callback>>;

class jthread {
public:
    using id = std::thread::id;
    using native_handle_type = std::thread::native_handle_type;

    jthread() noexcept : ss_(nostopstate) {}

    template <class Function, class... Args,
              class = std::enable_if_t<!std::is_same_v<std::decay_t<Function>, jthread>>>
    explicit jthread(Function&& function, Args&&... args)
        : ss_(), thread_(start_thread(ss_, std::forward<Function>(function), std::forward<Args>(args)...)) {}

    ~jthread() {
        if (thread_.joinable()) {
            ss_.request_stop();
            thread_.join();
        }
    }

    jthread(const jthread&) = delete;
    jthread(jthread&&) noexcept = default;
    jthread& operator=(const jthread&) = delete;
    jthread& operator=(jthread&& other) noexcept {
        if (this != &other) {
            if (thread_.joinable()) {
                ss_.request_stop();
                thread_.join();
            }
            ss_ = std::move(other.ss_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    void swap(jthread& other) noexcept {
        ss_.swap(other.ss_);
        std::swap(thread_, other.thread_);
    }
    [[nodiscard]] bool joinable() const noexcept { return thread_.joinable(); }
    [[nodiscard]] id get_id() const noexcept { return thread_.get_id(); }
    native_handle_type native_handle() { return thread_.native_handle(); }
    void join() { thread_.join(); }
    void detach() { thread_.detach(); }
    [[nodiscard]] stop_source get_stop_source() noexcept { return ss_; }
    [[nodiscard]] stop_token get_stop_token() const noexcept { return ss_.get_token(); }
    bool request_stop() noexcept { return ss_.request_stop(); }
    friend void swap(jthread& left, jthread& right) noexcept { left.swap(right); }

private:
    // Mirrors std::jthread construction: decay-copy the callable and every
    // argument into the new thread; pass the stop token as the first argument
    // when the callable accepts one.
    //
    // The invocability test is a static constexpr variable template rather than
    // an inline if-constexpr condition: MSVC (verified 14.50, /permissive-,
    // /std:c++20) mis-evaluates this exact trait expression when written
    // directly inside an if-constexpr of a static member function template with
    // an empty trailing parameter pack, while the variable-template form is
    // evaluated correctly. GCC/Clang accept both shapes.
    template <class Function, class... Args>
    static constexpr bool kInvocableWithToken =
        std::is_invocable_v<std::decay_t<Function>, stop_token, std::decay_t<Args>...>;

    template <class Function, class... Args>
    static std::thread start_thread(stop_source& source, Function&& function, Args&&... args) {
        static_assert(std::is_constructible_v<std::decay_t<Function>, Function>);
        static_assert((std::is_constructible_v<std::decay_t<Args>, Args> && ...));
        if constexpr (kInvocableWithToken<Function, Args...>) {
            return std::thread(
                [token = source.get_token(), invoked = std::decay_t<Function>(std::forward<Function>(function)),
                 pack = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)]() mutable {
                    std::apply(std::move(invoked),
                               std::tuple_cat(std::tuple<stop_token>(std::move(token)), std::move(pack)));
                });
        } else {
            static_assert(std::is_invocable_v<std::decay_t<Function>, std::decay_t<Args>...>,
                          "jthread callable must be invocable with its arguments (optionally preceded by stop_token)");
            return std::thread(std::forward<Function>(function), std::forward<Args>(args)...);
        }
    }

    stop_source ss_;
    std::thread thread_;
};

}  // namespace nickel::stop_compat

// Injection into namespace std happens only when the standard library truly
// lacks the family; on MSVC/GCC (which define __cpp_lib_jthread) and on
// libc++ builds that took the re-enable path above, the real std types stay
// in place and nothing is injected.
#if NICKEL_STOP_COMPAT_PORTABLE && !defined(__cpp_lib_jthread) && !defined(NICKEL_STOP_COMPAT_LIBCPP_REENABLED)
namespace std {
using nickel::stop_compat::jthread;
using nickel::stop_compat::nostopstate;
using nickel::stop_compat::nostopstate_t;
template <class Callback>
using stop_callback = nickel::stop_compat::stop_callback<Callback>;
using nickel::stop_compat::stop_source;
using nickel::stop_compat::stop_token;
}  // namespace std
#endif

#endif  // NICKEL_STOP_COMPAT_PORTABLE

// Implementation routing used by core/common/cancellation.hpp (and any other
// consumer that must follow the active layer).
#if NICKEL_STOP_COMPAT_PORTABLE
#define NICKEL_STOP_SOURCE ::nickel::stop_compat::stop_source
#define NICKEL_STOP_TOKEN ::nickel::stop_compat::stop_token
#else
#define NICKEL_STOP_SOURCE ::std::stop_source
#define NICKEL_STOP_TOKEN ::std::stop_token
#endif

namespace nickel::stop_compat::detail {

// Compile-time probe of what the standard library actually declares. Guards
// against the (not observed) case where the re-enable switch exists but the
// declarations are still missing: such a toolchain must take the portable
// fallback instead of failing here.
inline constexpr bool kStdStopFamilyDeclared = requires {
    typename std::stop_token;
    typename std::stop_source;
    typename std::stop_callback<int>;
};

}  // namespace nickel::stop_compat::detail

static_assert(NICKEL_STOP_COMPAT_PORTABLE != 0 || nickel::stop_compat::detail::kStdStopFamilyDeclared,
              "std stop-token family is unavailable and the portable fallback did not activate");
