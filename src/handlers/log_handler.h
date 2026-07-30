#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace handlers::logging {
    nlohmann::json read(
        const std::string& path,
        unsigned long long offset,
        size_t max_bytes,
        const std::string& expected_file_id,
        const std::string& expected_continuity_token);
}
