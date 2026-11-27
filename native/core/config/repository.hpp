#pragma once

#include "core/common/json.hpp"
#include "core/config/atomic_file.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace nickel {

inline constexpr std::uint64_t settings_max_bytes = 256 * 1024;
inline constexpr std::uint64_t accounts_max_bytes = 8 * 1024 * 1024;

struct JsonReadResult {
    enum class State { Missing, Valid, Corrupt, Oversized, WrongShape };
    State state = State::Missing;
    Json value;
    std::optional<std::string> text;
};

struct RecoveryReport {
    std::size_t restored = 0;
    std::size_t conflicts = 0;
    std::size_t rejected = 0;
};

// Direct legacy JSON is not enveloped, merged across roots, or silently repaired.
class JsonRepository {
public:
    explicit JsonRepository(std::filesystem::path root);
    const std::filesystem::path& root() const noexcept;
    std::filesystem::path document_path(std::string_view name) const;

    std::optional<std::string> read_text(std::string_view name,
                                         std::uint64_t limit = accounts_max_bytes) const;
    JsonReadResult inspect(std::string_view name, const Json& fallback,
                             std::uint64_t limit = settings_max_bytes) const;
    Json read(std::string_view name, const Json& fallback = Json::object(),
               std::uint64_t limit = settings_max_bytes) const;
    void write_text(std::string_view name, std::string_view text,
                     std::uint64_t limit = settings_max_bytes);
    void write(std::string_view name, const Json& value,
                 std::uint64_t limit = settings_max_bytes);
    Json update(std::string_view name, const Json& fallback,
                 const std::function<void(Json&)>& mutator,
                 std::uint64_t limit = settings_max_bytes,
                 const Cancellation* cancellation = nullptr);
    void transaction(const std::function<void()>& callback,
                      const Cancellation* cancellation = nullptr);
    RecoveryReport recover();

private:
    std::filesystem::path root_;
};

} // namespace nickel
