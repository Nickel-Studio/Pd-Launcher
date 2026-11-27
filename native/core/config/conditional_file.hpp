#pragma once

#include "core/config/atomic_file.hpp"

namespace nickel {

// A bounded observation, not a retained lock. Missing is explicit; an existing
// empty file has exists()==true. The opaque expectation binds path and identity.
class ConditionalFileSnapshot {
public:
    const std::filesystem::path& path() const noexcept;
    bool exists() const noexcept;
    std::span<const std::uint8_t> bytes() const noexcept;
    std::string_view sha256() const noexcept;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit ConditionalFileSnapshot(std::shared_ptr<const Impl> impl);
    friend ConditionalFileSnapshot read_conditional_file(const std::filesystem::path&,
        std::uint64_t, const Cancellation*);
    friend struct ConditionalFileAccess;
};

struct ConditionalReplaceResult {
    bool committed = false;
    // Empty after complete cleanup. If nonempty, recover before launching a game.
    std::filesystem::path recovery_receipt;
};

struct ConditionalRecoveryReport {
    std::uint64_t committed = 0;
    std::uint64_t restored = 0;
};

// max_bytes is explicit and at most 8 MiB; recovery runs before observation.
ConditionalFileSnapshot read_conditional_file(const std::filesystem::path& path,
    std::uint64_t max_bytes, const Cancellation* cancellation = nullptr);

// Recoverable two-phase conditional replacement, NOT single-publication CAS.
// Retains the checked original, quarantines it to a unique same-parent backup,
// then publishes a private stage without replacement. A temporary name gap is
// possible. Precommit failures restore only without replacement. Conflicting
// occupants are preserved, with backup/receipt and RecoveryRequired details.
// Cancellation after publication cannot replace an already committed result.
ConditionalReplaceResult replace_file_conditionally(const ConditionalFileSnapshot& expected,
    std::span<const std::uint8_t> bytes, std::uint64_t max_bytes,
    const AtomicWriteOptions& options = {});
ConditionalReplaceResult replace_text_conditionally(const ConditionalFileSnapshot& expected,
    std::string_view text, std::uint64_t max_bytes, const AtomicWriteOptions& options = {});

// Complete cleanup of an identity+hash-verified published stage, or restore the
// verified original without replacement. Conflicts retain all external objects
// and throw RecoveryRequired. Call successfully before spawning the game.
ConditionalRecoveryReport recover_conditional_replacements(const std::filesystem::path& directory);

} // namespace nickel
