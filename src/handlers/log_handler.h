#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace handlers::logging {
    nlohmann::json read(unsigned long long cursor, size_t max_lines);
    nlohmann::json tail(size_t max_lines);
    nlohmann::json clear();
    nlohmann::json write(const std::string& text);
    nlohmann::json status();
    nlohmann::json read_file(const std::string& path, unsigned long long offset, size_t max_bytes);
}
