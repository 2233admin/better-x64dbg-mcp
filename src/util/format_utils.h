#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <vector>
#include <sstream>
#include <iomanip>

#include "_plugin_types.h"

namespace format_utils {

    [[nodiscard]] inline std::string format_address(duint addr) {
#ifdef _WIN64
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(addr));
#else
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(addr));
#endif
        return buf;
    }

    [[nodiscard]] inline std::string format_hex(duint value) {
#ifdef _WIN64
        char buf[32];
        snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(value));
#else
        char buf[16];
        snprintf(buf, sizeof(buf), "%X", static_cast<unsigned int>(value));
#endif
        return buf;
    }

    [[nodiscard]] inline std::string format_bytes_hex(const uint8_t* data, size_t size) {
        std::ostringstream oss;
        for (size_t i = 0; i < size; ++i) {
            if (i > 0) oss << ' ';
            oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                << static_cast<int>(data[i]);
        }
        return oss.str();
    }

    [[nodiscard]] inline std::string format_bytes_compact(const uint8_t* data, size_t size) {
        std::ostringstream oss;
        for (size_t i = 0; i < size; ++i) {
            oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                << static_cast<int>(data[i]);
        }
        return oss.str();
    }

    [[nodiscard]] inline std::vector<uint8_t> parse_hex_bytes(const std::string& hex) {
        std::vector<uint8_t> bytes;
        std::string clean;
        for (size_t i = 0; i < hex.size(); ++i) {
            char c = hex[i];
            if (c == ' ' || c == ',' || c == '\t' || c == '\n') continue;
            if (c == '0' && i + 1 < hex.size() && (hex[i + 1] == 'x' || hex[i + 1] == 'X')) {
                ++i;
                continue;
            }
            clean += c;
        }
        // Reject odd-length input instead of silently dropping the last nibble
        if (clean.size() % 2 != 0) return {};
        for (size_t i = 0; i + 1 < clean.size(); i += 2) {
            auto byte_str = clean.substr(i, 2);
            bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
        }
        return bytes;
    }

    [[nodiscard]] inline duint parse_address(const std::string& addr_str) {
        if (addr_str.empty()) return 0;
        std::string clean = addr_str;
        if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
            clean = clean.substr(2);
        }
#ifdef _WIN64
        return static_cast<duint>(std::stoull(clean, nullptr, 16));
#else
        return static_cast<duint>(std::stoul(clean, nullptr, 16));
#endif
    }

    [[nodiscard]] inline std::string format_protection(DWORD protect) {
        std::string result;
        switch (protect & 0xFF) {
            case PAGE_NOACCESS:          result = "PAGE_NOACCESS"; break;
            case PAGE_READONLY:          result = "PAGE_READONLY"; break;
            case PAGE_READWRITE:         result = "PAGE_READWRITE"; break;
            case PAGE_WRITECOPY:         result = "PAGE_WRITECOPY"; break;
            case PAGE_EXECUTE:           result = "PAGE_EXECUTE"; break;
            case PAGE_EXECUTE_READ:      result = "PAGE_EXECUTE_READ"; break;
            case PAGE_EXECUTE_READWRITE: result = "PAGE_EXECUTE_READWRITE"; break;
            case PAGE_EXECUTE_WRITECOPY: result = "PAGE_EXECUTE_WRITECOPY"; break;
            default:                     result = "UNKNOWN"; break;
        }
        if (protect & PAGE_GUARD)   result += " | PAGE_GUARD";
        if (protect & PAGE_NOCACHE) result += " | PAGE_NOCACHE";
        return result;
    }

    [[nodiscard]] inline std::string format_mem_state(DWORD state) {
        switch (state) {
            case MEM_COMMIT:  return "MEM_COMMIT";
            case MEM_RESERVE: return "MEM_RESERVE";
            case MEM_FREE:    return "MEM_FREE";
            default:          return "UNKNOWN";
        }
    }

    // Validate UTF-8, replacing each invalid byte with U+FFFD. Required before placing
    // externally-sourced bytes into nlohmann::json — json::dump() throws type_error.316
    // on invalid UTF-8. Rejects bad continuation bytes, overlong encodings, UTF-16
    // surrogates (U+D800-DFFF) and code points above U+10FFFF.
    [[nodiscard]] inline std::string to_utf8_safe(std::string_view in) {
        static constexpr const char* replacement = "\xEF\xBF\xBD"; // U+FFFD
        std::string out;
        out.reserve(in.size());
        size_t i = 0;
        while (i < in.size()) {
            const auto b0 = static_cast<unsigned char>(in[i]);
            size_t len;
            uint32_t cp;
            if (b0 < 0x80) { out += static_cast<char>(b0); ++i; continue; }
            else if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1F; }
            else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; }
            else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; }
            else { out += replacement; ++i; continue; }

            if (i + len > in.size()) { out += replacement; ++i; continue; }
            bool valid = true;
            for (size_t k = 1; k < len; ++k) {
                const auto bk = static_cast<unsigned char>(in[i + k]);
                if ((bk & 0xC0) != 0x80) { valid = false; break; }
                cp = (cp << 6) | (bk & 0x3F);
            }
            // Overlong encodings, surrogates, out-of-range code points
            if (valid) {
                if (len == 2 && cp < 0x80) valid = false;
                else if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) valid = false;
                else if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) valid = false;
            }
            if (valid) {
                out.append(in.substr(i, len));
                i += len;
            } else {
                out += replacement;
                ++i;
            }
        }
        return out;
    }

    [[nodiscard]] inline std::string format_mem_type(DWORD type) {
        switch (type) {
            case MEM_IMAGE:   return "MEM_IMAGE";
            case MEM_MAPPED:  return "MEM_MAPPED";
            case MEM_PRIVATE: return "MEM_PRIVATE";
            default:          return "UNKNOWN";
        }
    }

} // namespace format_utils
