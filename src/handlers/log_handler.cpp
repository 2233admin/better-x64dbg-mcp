#include "handlers/log_handler.h"
#include "util/path_utils.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

constexpr size_t MAX_READ_BYTES = 1024 * 1024;
constexpr size_t UTF8_MAX_EXTRA_BYTES = 3;
constexpr size_t CURSOR_GUARD_BYTES = 64;
constexpr size_t MAX_CURSOR_STATES = 512;

class c_scoped_handle {
public:
    explicit c_scoped_handle(HANDLE handle) : m_handle(handle) {}
    ~c_scoped_handle() {
        if (m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
    }

    c_scoped_handle(const c_scoped_handle&) = delete;
    c_scoped_handle& operator=(const c_scoped_handle&) = delete;

    [[nodiscard]] HANDLE get() const {
        return m_handle;
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

struct s_cursor_state {
    std::string path;
    std::string file_id;
    unsigned long long next_offset = 0;
    unsigned long long observed_size = 0;
    std::string guard;
    unsigned long long last_used = 0;
};

class c_cursor_cache {
public:
    [[nodiscard]] std::optional<s_cursor_state> find(
        const std::string& token
    ) {
        std::lock_guard lock(m_mutex);
        const auto found = m_states.find(token);
        if (found == m_states.end())
            return std::nullopt;
        found->second.last_used = ++m_clock;
        return found->second;
    }

    [[nodiscard]] std::string store(s_cursor_state state) {
        std::lock_guard lock(m_mutex);

        // Continuation states are immutable. Reuse an existing token when a
        // retry or EOF poll reaches the same state so no-progress reads do not
        // consume the bounded cache one entry at a time.
        for (auto& [token, current] : m_states) {
            if (current.path == state.path
                && current.file_id == state.file_id
                && current.next_offset == state.next_offset
                && current.observed_size == state.observed_size
                && current.guard == state.guard) {
                current.last_used = ++m_clock;
                return token;
            }
        }

        if (m_states.size() >= MAX_CURSOR_STATES) {
            const auto oldest = (std::min_element)(
                m_states.begin(), m_states.end(),
                [](const auto& left, const auto& right) {
                    return left.second.last_used < right.second.last_used;
                });
            if (oldest != m_states.end())
                m_states.erase(oldest);
        }

        std::string token;
        do {
            std::ostringstream stream;
            stream << "v3:" << std::uppercase << std::hex
                   << std::setw(16) << std::setfill('0') << ++m_sequence;
            token = stream.str();
        } while (m_states.contains(token));

        state.last_used = ++m_clock;
        m_states.emplace(token, std::move(state));
        return token;
    }

    void invalidate(const std::string& path) {
        std::lock_guard lock(m_mutex);
        for (auto current = m_states.begin(); current != m_states.end();) {
            if (current->second.path == path)
                current = m_states.erase(current);
            else
                ++current;
        }
    }

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, s_cursor_state> m_states;
    unsigned long long m_clock = 0;
    unsigned long long m_sequence = 0;
};

[[nodiscard]] c_cursor_cache& get_cursor_cache() {
    static c_cursor_cache cache;
    return cache;
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty())
        return {};
    if (utf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        throw std::runtime_error("path is too long");

    const auto length = static_cast<int>(utf8.size());
    const auto needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
    if (needed <= 0)
        throw std::runtime_error("path is not valid UTF-8");

    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length,
            wide.data(), needed) != needed) {
        throw std::runtime_error("failed to convert path to UTF-16");
    }
    return wide;
}

[[nodiscard]] unsigned long long get_file_size(
    HANDLE file,
    const std::string& path
) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        const auto error = GetLastError();
        throw std::runtime_error(
            "GetFileSizeEx failed for '" + path + "' (error "
            + std::to_string(error) + ")");
    }
    if (size.QuadPart < 0)
        throw std::runtime_error("File size is negative for '" + path + "'");
    return static_cast<unsigned long long>(size.QuadPart);
}

[[nodiscard]] std::string get_file_id(
    HANDLE file,
    const std::string& path
) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) {
        const auto error = GetLastError();
        throw std::runtime_error(
            "GetFileInformationByHandle failed for '" + path + "' (error "
            + std::to_string(error) + ")");
    }

    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0')
           << std::setw(8) << information.dwVolumeSerialNumber << ':'
           << std::setw(8) << information.nFileIndexHigh
           << std::setw(8) << information.nFileIndexLow;
    return stream.str();
}

[[nodiscard]] std::string read_at(
    HANDLE file,
    const std::string& path,
    unsigned long long offset,
    size_t count
) {
    if (count == 0)
        return {};
    if (offset > static_cast<unsigned long long>(
                     (std::numeric_limits<LONGLONG>::max)())) {
        throw std::runtime_error("offset is outside the Windows file range");
    }

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        const auto error = GetLastError();
        throw std::runtime_error(
            "SetFilePointerEx failed for '" + path + "' (error "
            + std::to_string(error) + ")");
    }

    std::string bytes(count, '\0');
    size_t total = 0;
    while (total < bytes.size()) {
        DWORD read = 0;
        const auto remaining = static_cast<DWORD>(bytes.size() - total);
        if (!ReadFile(file, bytes.data() + total, remaining, &read, nullptr)) {
            const auto error = GetLastError();
            throw std::runtime_error(
                "ReadFile failed for '" + path + "' (error "
                + std::to_string(error) + ")");
        }
        if (read == 0)
            break;
        total += read;
    }
    bytes.resize(total);
    return bytes;
}

[[nodiscard]] bool is_utf8_continuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

[[nodiscard]] size_t utf8_page_size(
    const std::string& bytes,
    size_t max_bytes,
    unsigned long long offset
) {
    size_t page_size = (std::min)(bytes.size(), max_bytes);
    while (page_size < bytes.size()
           && is_utf8_continuation(
               static_cast<unsigned char>(bytes[page_size]))) {
        ++page_size;
    }

    if (page_size == 0)
        return 0;
    if (page_size > static_cast<size_t>((std::numeric_limits<int>::max)()))
        throw std::runtime_error("log page is too large to validate as UTF-8");

    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
            static_cast<int>(page_size), nullptr, 0) <= 0) {
        throw std::runtime_error(
            "Log file contains invalid UTF-8 at or before byte offset "
            + std::to_string(offset + page_size));
    }
    return page_size;
}

[[nodiscard]] std::string extend_guard(
    std::string_view previous,
    std::string_view content
) {
    if (content.size() >= CURSOR_GUARD_BYTES) {
        return std::string(
            content.substr(content.size() - CURSOR_GUARD_BYTES));
    }

    std::string guard;
    guard.reserve(previous.size() + content.size());
    guard.append(previous);
    guard.append(content);
    if (guard.size() > CURSOR_GUARD_BYTES)
        guard.erase(0, guard.size() - CURSOR_GUARD_BYTES);
    return guard;
}

[[nodiscard]] bool verify_guard(
    HANDLE file,
    const std::string& path,
    unsigned long long offset,
    const std::string& guard
) {
    if (guard.empty())
        return offset == 0;
    if (guard.size() > offset)
        return false;
    return read_at(file, path, offset - guard.size(), guard.size()) == guard;
}

[[nodiscard]] nlohmann::json make_response(
    const std::string& path,
    const std::string& file_id,
    std::string content,
    unsigned long long offset,
    unsigned long long file_size,
    std::string guard,
    bool reset,
    const std::string& reset_reason
) {
    const auto next_offset = offset + content.size();
    auto token = get_cursor_cache().store({
        .path = path,
        .file_id = file_id,
        .next_offset = next_offset,
        .observed_size = file_size,
        .guard = std::move(guard),
    });

    nlohmann::json response = {
        {"content", std::move(content)},
        {"offset", offset},
        {"next_offset", next_offset},
        {"bytes_read", next_offset - offset},
        {"size", file_size},
        {"eof", next_offset >= file_size},
        {"file_id", file_id},
        {"continuity_token", std::move(token)},
        {"continuity_mode", "append_only"},
        {"reset", reset},
    };
    if (reset)
        response["reset_reason"] = reset_reason;
    return response;
}

} // namespace

namespace handlers::logging {

nlohmann::json read(
    const std::string& path,
    unsigned long long offset,
    size_t max_bytes,
    const std::string& expected_file_id,
    const std::string& expected_continuity_token
) {
    if (path_utils::contains_embedded_nul(path))
        throw std::runtime_error("path must not contain an embedded NUL");
    if (!path_utils::is_absolute_windows_path(path)) {
        throw std::runtime_error(
            "path must be an absolute Windows file path "
            "(for example C:\\logs\\bp.log)");
    }
    if (offset != 0
        && (expected_file_id.empty()
            || expected_continuity_token.empty())) {
        throw std::runtime_error(
            "file_id and continuity_token are required when offset is non-zero");
    }
    max_bytes = std::clamp<size_t>(max_bytes, 1, MAX_READ_BYTES);

    const auto wide_path = utf8_to_wide(path);
    const auto raw_file = CreateFileW(
        wide_path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw_file == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        throw std::runtime_error(
            "Failed to open '" + path + "' (error "
            + std::to_string(error) + ")");
    }
    const c_scoped_handle file(raw_file);

    const auto file_id = get_file_id(file.get(), path);
    const auto initial_size = get_file_size(file.get(), path);

    bool reset = false;
    std::string reset_reason;
    std::optional<s_cursor_state> state;
    const auto reset_to_start = [&](std::string reason, bool invalidate) {
        if (invalidate)
            get_cursor_cache().invalidate(path);
        reset = true;
        reset_reason = std::move(reason);
        offset = 0;
        state.reset();
    };

    if (offset != 0) {
        state = get_cursor_cache().find(expected_continuity_token);
        if (state
            && (state->path != path
                || state->file_id != expected_file_id
                || state->next_offset != offset)) {
            reset_to_start("continuity_state_mismatch", false);
        } else if (file_id != expected_file_id) {
            reset_to_start("file_replaced", true);
        } else if (!state) {
            reset_to_start("continuity_state_expired", false);
        } else if (initial_size < offset
                   || initial_size < state->observed_size) {
            reset_to_start("file_truncated", true);
        } else if (!verify_guard(
                       file.get(), path, offset, state->guard)) {
            reset_to_start("file_rewritten", true);
        }
    }

    const auto available = initial_size > offset
        ? initial_size - offset
        : 0;
    const auto read_limit = max_bytes + UTF8_MAX_EXTRA_BYTES;
    const auto requested = static_cast<size_t>((std::min)(
        available, static_cast<unsigned long long>(read_limit)));
    auto raw_content = read_at(file.get(), path, offset, requested);

    if (raw_content.size() != requested) {
        const auto final_size = get_file_size(file.get(), path);
        get_cursor_cache().invalidate(path);
        return make_response(
            path, file_id, {}, 0, final_size, {},
            true, "file_truncated");
    }

    const auto page_size = utf8_page_size(raw_content, max_bytes, offset);
    raw_content.resize(page_size);
    auto guard = extend_guard(
        state ? std::string_view(state->guard) : std::string_view{},
        raw_content);
    const auto next_offset = offset + raw_content.size();
    const auto final_size = get_file_size(file.get(), path);

    if (final_size < initial_size || final_size < next_offset) {
        get_cursor_cache().invalidate(path);
        return make_response(
            path, file_id, {}, 0, final_size, {},
            true, "file_truncated");
    }
    if (!verify_guard(file.get(), path, next_offset, guard)) {
        get_cursor_cache().invalidate(path);
        return make_response(
            path, file_id, {}, 0, final_size, {},
            true, "file_rewritten");
    }

    return make_response(
        path, file_id, std::move(raw_content), offset, final_size,
        std::move(guard), reset, reset_reason);
}

} // namespace handlers::logging
