#include "handlers/log_handler.h"
#include "bridge/c_log_capture.h"
#include "util/format_utils.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace {

constexpr size_t MAX_LINES_CAP = 5000;
constexpr size_t MAX_READ_FILE_BYTES = 1024 * 1024;

[[nodiscard]] size_t clamp_lines(size_t requested) {
    return std::clamp<size_t>(requested, 1, MAX_LINES_CAP);
}

// A relative path would resolve against x64dbg's CWD, which shifts with the
// debuggee — the client cannot predict the target. Require an absolute path.
[[nodiscard]] bool is_absolute_path(const std::string& path) {
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') return true;      // UNC or "\\?\" prefix
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0]))
        && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) return true;    // X:\ or X:/
    return false;
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    auto needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) throw std::runtime_error("path is not valid UTF-8");
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

}  // namespace

namespace handlers::logging {

nlohmann::json read(unsigned long long cursor, size_t max_lines) {
    return get_log_capture().read(cursor, clamp_lines(max_lines));
}

nlohmann::json tail(size_t max_lines) {
    return get_log_capture().tail(clamp_lines(max_lines));
}

nlohmann::json clear() {
    return get_log_capture().clear_all();
}

nlohmann::json write(const std::string& text) {
    return get_log_capture().write_marker(text);
}

nlohmann::json status() {
    return get_log_capture().status();
}

nlohmann::json read_file(const std::string& path, unsigned long long offset, size_t max_bytes) {
    if (!is_absolute_path(path))
        throw std::runtime_error("path must be absolute (e.g. C:\\logs\\bp.log) — relative paths "
                                 "resolve against x64dbg's working directory, which shifts per debuggee");
    max_bytes = std::clamp<size_t>(max_bytes, 1, MAX_READ_FILE_BYTES);

    const auto wide = utf8_to_wide(path);
    // Share write+delete: x64dbg appends to breakpoint log files via WriteFile while
    // the debuggee runs; appended bytes are immediately visible to this read.
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Failed to open '" + path + "' (error "
                                 + std::to_string(GetLastError()) + ")");

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {  // taken from the open handle — no TOCTOU
        const auto err = GetLastError();
        CloseHandle(file);
        throw std::runtime_error("GetFileSizeEx failed for '" + path + "' (error "
                                 + std::to_string(err) + ")");
    }
    const auto file_size = static_cast<unsigned long long>(size.QuadPart);

    std::string content;
    if (offset < file_size) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) {
            const auto err = GetLastError();
            CloseHandle(file);
            throw std::runtime_error("SetFilePointerEx failed (error " + std::to_string(err) + ")");
        }
        content.resize(static_cast<size_t>((std::min)(
            static_cast<unsigned long long>(max_bytes), file_size - offset)));
        size_t total = 0;
        while (total < content.size()) {
            DWORD got = 0;
            if (!ReadFile(file, content.data() + total,
                          static_cast<DWORD>(content.size() - total), &got, nullptr) || got == 0)
                break;
            total += got;
        }
        content.resize(total);
    } else {
        offset = file_size;  // clamp — the file may have been rotated/truncated
    }
    CloseHandle(file);

    const auto next_offset = offset + content.size();
    return {
        // Sanitized: json::dump() rejects invalid UTF-8, and a bp log file is
        // arbitrary bytes. size/eof are a snapshot of a possibly-growing file.
        {"content", format_utils::to_utf8_safe(content)},
        {"offset", offset},
        {"next_offset", next_offset},
        {"bytes_read", content.size()},
        {"size", file_size},
        {"eof", next_offset >= file_size},
    };
}

}  // namespace handlers::logging
