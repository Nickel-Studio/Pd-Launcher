#pragma once

#include "services.hpp"
#include "core/network/http.hpp"
#include "core/tasks/task.hpp"

namespace nickel::minecraft {

struct NativeServiceOptions {
    transport::TrustPolicy trust = transport::TrustPolicy::PublicHttps;
    std::optional<transport::NetworkSnapshot> snapshot;
    std::shared_ptr<tasks::Task> task;
    bool useMirrors = true;
    unsigned parallelRanges = 1;
};

// Only this factory binds production HTTP/archive/hash/storage/process services.
// Borrow input options so allocation failure cannot leave an unguarded credential copy.
Services make_native_services(std::shared_ptr<transport::HttpClient> http,
                              const NativeServiceOptions& options = {});

}
